//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/decidb/formulation/ilp_linearization.cpp
//
// The lowering entry point and the global-auxiliary allocators every pass shares.
// The passes themselves live in the linearization_*.cpp siblings.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"

namespace duckdb {

// The lowering passes live in linearization_bigm.cpp, linearization_minmax.cpp,
// linearization_not_equal.cpp and linearization_bilinear_abs.cpp; this file keeps
// the global-auxiliary allocators they share and the flat-column entry point.
using namespace decide_linearize; // NOLINT: internal DECIDE linearization helpers

// --- Global auxiliary creation ---------------------------------------------
// Every auxiliary column DeciDB introduces is created here. Both helpers exist so
// that a continuous auxiliary cannot be declared without stating the range of the
// expression it stands for: an infinite bound is reachable only through
// `AuxRange::lo_unbounded` / `hi_unbounded`, i.e. only on an end no contributing
// decision variable's box could derive. The two are separate because a range open
// on one side still boxes the other. A continuous auxiliary left free
// when its range was in fact derivable costs the root LP dearly — the simplex has
// no box to start from and crawls toward the answer one pivot at a time.

//! Give the auxiliary at flat index `aux_idx` its diagnosis label, padding the label
//! channel out to that column first.
//!
//! `global_variable_labels` is positional — entry `i` names global column `i` — but it
//! is written only where a label exists, so it can trail the block and a bare
//! `push_back` would land the label on whatever column happens to be next. The pad is
//! what keeps the two aligned. Every creation site used to spell it out itself, which
//! left a new site one forgotten line away from naming the wrong column; both creation
//! helpers below call this instead, so a column and its label are set in one step.
//! Unnamed columns take an empty entry, which is what the readback fills in anyway.
static void LabelGlobalAux(SolverInput &input, const VarIndexer &indexer, idx_t aux_idx,
                           const string &label) {
    input.global_variable_labels.resize(aux_idx - indexer.global_block_start);
    input.global_variable_labels.push_back(label);
}

//! Append one continuous auxiliary column bounded by the family it reduces over.
//! `label` is the clause text a diagnosis should render for it; empty means unnamed.
//! Returns its flat column index.
idx_t decide_linearize::AddGlobalContinuousAux(SolverInput &input, const VarIndexer &indexer,
                                              const AuxRange &range, double obj_coeff,
                                              const string &label) {
    idx_t aux_idx = indexer.global_block_start + input.num_global_vars;
    input.num_global_vars += 1;
    LabelGlobalAux(input, indexer, aux_idx, label);
    input.global_variable_types.push_back(LogicalType::DOUBLE);
    // Per side. An auxiliary over `x >= 0` with no ceiling is emitted `[0, 1e30]`,
    // not `[-1e30, 1e30]`: the floor was derived, and throwing it away because the
    // ceiling was not is a box given up for nothing.
    input.global_lower_bounds.push_back(range.lo_unbounded ? -1e30 : range.lo);
    input.global_upper_bounds.push_back(range.hi_unbounded ? 1e30 : range.hi);
    input.global_bounds_unbounded.push_back(range.Unbounded());
    input.global_obj_coeffs.push_back(obj_coeff);
    return aux_idx;
}

//! Append one binary auxiliary column. Its [0,1] box comes from the domain, so it
//! needs no range.
idx_t decide_linearize::AddGlobalBinaryAux(SolverInput &input, const VarIndexer &indexer,
                                          double obj_coeff, const string &label) {
    idx_t aux_idx = indexer.global_block_start + input.num_global_vars;
    input.num_global_vars += 1;
    LabelGlobalAux(input, indexer, aux_idx, label);
    input.global_variable_types.push_back(LogicalType::BOOLEAN);
    input.global_lower_bounds.push_back(0.0);
    input.global_upper_bounds.push_back(1.0);
    input.global_bounds_unbounded.push_back(false);
    input.global_obj_coeffs.push_back(obj_coeff);
    return aux_idx;
}

//! Pin an already-created auxiliary to exactly `value`. Used where a reducer turns
//! out to range over nothing at all, so the auxiliary has no pinning rows and would
//! otherwise float on whatever box it was given.
void decide_linearize::PinGlobalAux(SolverInput &input, const VarIndexer &indexer, idx_t aux_idx,
                                    double value) {
    idx_t local = aux_idx - indexer.global_block_start;
    input.global_lower_bounds[local] = value;
    input.global_upper_bounds[local] = value;
}

//===--------------------------------------------------------------------===//
// Lowering: constructs the chosen backend cannot state
//===--------------------------------------------------------------------===//

//! The box of one flat column, in the coordinates every construct emits in. Mirrors
//! what `SolverModel::Build` will expand into `col_lower` / `col_upper`, and must:
//! a Big-M derived from a wider box than the model declares would be slack, and one
//! derived from a narrower box would cut the feasible region.
static void FlatColumnBox(const SolverInput &input, const FormulationBox &box,
                          const VarIndexer &indexer, int col, double &lo, double &hi) {
    idx_t c = static_cast<idx_t>(col);
    if (c >= indexer.global_block_start) {
        idx_t g = c - indexer.global_block_start;
        lo = box.global_lower[g];
        hi = box.global_upper[g];
        if (input.global_variable_types[g] == LogicalType::BOOLEAN) {
            lo = MaxValue<double>(lo, 0.0);
            hi = MinValue<double>(hi, 1.0);
        }
        return;
    }
    idx_t v = indexer.OwnerOf(c);
    D_ASSERT(v != DConstants::INVALID_INDEX);
    // The same asymmetry the model builder applies: the lower bound is authoritative
    // (stage 08 already resolved it, negatives included), the upper is intersected with
    // the type ceiling.
    lo = box.lower[v];
    hi = box.upper[v];
    if (input.variable_types[v] == LogicalType::BOOLEAN) {
        hi = MinValue<double>(hi, 1.0);
    }
}

//! How far a row's left-hand side can reach, toward the end its bound is on. `hi_end`
//! asks for the maximum (a `<=` row), otherwise the minimum (a `>=` row). Returns false
//! and names the offending column when the box that end depends on is open — there is no
//! Big-M then, and the query is refused rather than given a constant.
static bool FlatRowReach(const SolverInput &input, const FormulationBox &box,
                         const VarIndexer &indexer,
                         const vector<int> &indices, const vector<double> &coefficients,
                         bool hi_end, double &reach, idx_t &blame_col) {
    reach = 0.0;
    for (idx_t k = 0; k < indices.size(); k++) {
        double c = coefficients[k];
        if (c == 0.0) {
            continue;
        }
        double lo, hi;
        FlatColumnBox(input, box, indexer, indices[k], lo, hi);
        // A negative coefficient swaps which end of the box feeds which end of the term,
        // so the sign has to be respected before an end is blamed.
        double end = (c > 0.0) == hi_end ? hi : lo;
        if (end >= 1e20 || end <= -1e20) {
            blame_col = static_cast<idx_t>(indices[k]);
            return false;
        }
        reach += c * end;
    }
    return true;
}

void LowerDecideConstructs(SolverInput &input, const VarIndexer &indexer,
                           const FormulationBox &box,
                           const vector<string> &var_names,
                           const SolverConstructSupport &constructs) {
    if (constructs.not_equal || input.indicator_constraints.empty()) {
        return;
    }
    for (auto &ic : input.indicator_constraints) {
        auto &row = ic.row;
        D_ASSERT(row.sense == '<' || row.sense == '>');
        // The Big-M is the distance from the row's bound to the far end of its own
        // reach: relaxed by exactly that much, the row admits everything the columns can
        // produce and cuts nothing. Derived per HALF, from that half's own row, which is
        // tighter than the single constant the two used to share.
        bool hi_end = row.sense == '<';
        double reach = 0.0;
        idx_t blame_col = DConstants::INVALID_INDEX;
        if (!FlatRowReach(input, box, indexer, row.indices, row.coefficients, hi_end, reach,
                          blame_col)) {
            // No finite M exists over an open box. Refuse, naming a column the user can
            // bound — which is what `OwnerOf` is for: this layer works in flat columns
            // and still has to speak the user's language.
            ThrowUnboundedBigMNaming(indexer.OwnerOf(blame_col), var_names, "<>");
        }
        double M = hi_end ? reach - row.rhs : row.rhs - reach;
        // A row already implied by its own box needs no relaxation at all. The margin is
        // the integer-step band the `<>` rewrite works on: the bound is `K±1` on an
        // integer lattice, so one unit of slack costs nothing and keeps the relaxed
        // branch clear of floating-point wobble in the solver's own row activity.
        M = MaxValue<double>(M, 0.0) + 1.0;

        SolverInput::RawConstraint lowered = std::move(row);
        // `z == v` implies the row, so the row must be slackened by M exactly when
        // `z != v`: by `M*z` when v is 0, and by `M*(1-z)` when v is 1 — which moves the
        // bound as well as adding the term.
        double m_coeff;
        if (ic.binary_value == 0) {
            m_coeff = hi_end ? -M : M;
        } else {
            m_coeff = hi_end ? M : -M;
            lowered.rhs += hi_end ? M : -M;
        }
        lowered.indices.push_back(ic.binary_column);
        lowered.coefficients.push_back(m_coeff);
        input.global_constraints.push_back(std::move(lowered));
    }
    input.indicator_constraints.clear();
}

} // namespace duckdb
