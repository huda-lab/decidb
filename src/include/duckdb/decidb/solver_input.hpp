//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/solver_input.hpp
//
// Solver-agnostic input structs for the DECIDE optimization formulation.
// These are built by physical_decide.cpp and consumed by the solver facade.
// Supports LP, MILP, and convex QP/MIQP objectives.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/decidb/solver_capabilities.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/expression.hpp"

#include <algorithm>
#include <cmath>

namespace duckdb {

//! Compact per-row coefficient/value column.
//!
//! Many constraint columns are *broadcast* — every row gets the same value
//! (e.g., the M coefficient on a hard MIN/MAX indicator term, the constant RHS
//! on most aggregate constraints, McCormick L1–L4 constants). Allocating a
//! `vector<double>(num_rows, K)` for these wastes memory bandwidth and
//! allocation pressure scaling with `num_rows × num_terms × num_constraints`.
//!
//! `CoefficientColumn` stores either a single scalar or a dense `vector<double>`
//! of length `logical_size`. Reads via `Get(row)` are branchless on the storage
//! kind. Mutation lazily promotes Scalar → Dense — once a row needs a unique
//! value, the underlying vector is materialized and subsequent ops are O(1).
//!
//! Invariants:
//!   - `logical_size` always equals num_rows for the constraint that owns the column.
//!   - When `kind == Dense`, `dense_values.size() == logical_size`.
//!   - `Empty()` (logical_size == 0) is reserved for not-yet-initialized columns.
struct CoefficientColumn {
    //! SparseMasked stores a uniform value at a sorted set of row indices, with
    //! every other row implicitly 0. Built for the NE per-row indicator path:
    //! every active row gets `-M`, every excluded row gets 0, so a single
    //! `sparse_value` plus a sorted `sparse_indices` list captures the column
    //! at ~10% the memory of Dense when the active rate is ~10%. Mutators all
    //! route through `EnsureDense()` so any in-place edit promotes back to
    //! Dense — sparse storage is read-only by design.
    enum class Kind : uint8_t { Scalar, Dense, SparseMasked };
    Kind kind = Kind::Dense;
    double scalar_value = 0.0;
    vector<double> dense_values;
    vector<idx_t> sparse_indices;     // sorted ascending; SparseMasked only
    double sparse_value = 0.0;        // value at every entry in sparse_indices
    idx_t logical_size = 0;

    CoefficientColumn() = default;

    static CoefficientColumn MakeScalar(double v, idx_t n) {
        CoefficientColumn c;
        c.kind = Kind::Scalar;
        c.scalar_value = v;
        c.logical_size = n;
        return c;
    }
    static CoefficientColumn MakeDense(idx_t n, double init = 0.0) {
        CoefficientColumn c;
        c.kind = Kind::Dense;
        c.dense_values.assign(n, init);
        c.logical_size = n;
        return c;
    }
    static CoefficientColumn FromVector(vector<double> &&v) {
        CoefficientColumn c;
        c.kind = Kind::Dense;
        c.logical_size = v.size();
        c.dense_values = std::move(v);
        return c;
    }
    //! Build a column whose only nonzero entries are the rows in `sorted_indices`,
    //! each holding `value`. `sorted_indices` must be strictly ascending and within
    //! [0, logical_size). Other rows return 0 from `Get`.
    static CoefficientColumn MakeSparseMasked(idx_t logical_size,
                                              vector<idx_t> &&sorted_indices,
                                              double value) {
        CoefficientColumn c;
        c.kind = Kind::SparseMasked;
        c.sparse_indices = std::move(sorted_indices);
        c.sparse_value = value;
        c.logical_size = logical_size;
        return c;
    }

    inline double Get(idx_t row) const {
        switch (kind) {
        case Kind::Scalar:
            return scalar_value;
        case Kind::Dense:
            return dense_values[row];
        case Kind::SparseMasked: {
            auto it = std::lower_bound(sparse_indices.begin(), sparse_indices.end(), row);
            return (it != sparse_indices.end() && *it == row) ? sparse_value : 0.0;
        }
        }
        return 0.0;
    }
    inline double operator[](idx_t row) const { return Get(row); }
    idx_t Size() const { return logical_size; }
    bool Empty() const { return logical_size == 0; }
    bool IsUniform() const { return kind == Kind::Scalar; }
    double UniformValue() const { return scalar_value; }
    bool IsSparseMasked() const { return kind == Kind::SparseMasked; }

    //! Force Dense storage (allocates if currently Scalar or SparseMasked).
    //! After this, dense_values.size() == logical_size.
    void EnsureDense() {
        if (kind == Kind::Scalar) {
            dense_values.assign(logical_size, scalar_value);
            kind = Kind::Dense;
        } else if (kind == Kind::SparseMasked) {
            vector<double> dense(logical_size, 0.0);
            for (idx_t r : sparse_indices) {
                dense[r] = sparse_value;
            }
            dense_values = std::move(dense);
            vector<idx_t>().swap(sparse_indices);
            sparse_value = 0.0;
            kind = Kind::Dense;
        }
    }

    //! Set one row to v. Promotes Scalar → Dense if needed.
    inline void Set(idx_t row, double v) {
        EnsureDense();
        dense_values[row] = v;
    }
    inline void MaskRow(idx_t row) { Set(row, 0.0); }
    inline void ScaleRow(idx_t row, double factor) {
        EnsureDense();
        dense_values[row] *= factor;
    }

    //! Bulk replace with a uniform scalar broadcast.
    void AssignScalar(idx_t n, double v) {
        kind = Kind::Scalar;
        scalar_value = v;
        logical_size = n;
        // Drop any prior dense or sparse allocation
        vector<double>().swap(dense_values);
        vector<idx_t>().swap(sparse_indices);
        sparse_value = 0.0;
    }
    //! Bulk replace with a dense column, all entries = init.
    void AssignDense(idx_t n, double init = 0.0) {
        kind = Kind::Dense;
        dense_values.assign(n, init);
        logical_size = n;
        vector<idx_t>().swap(sparse_indices);
        sparse_value = 0.0;
    }
    //! Reserve dense capacity for upcoming PushBack-style fills.
    void Reserve(idx_t n) {
        EnsureDense();
        dense_values.reserve(n);
    }
    //! Resize dense storage; values default to 0.
    void Resize(idx_t n, double init = 0.0) {
        EnsureDense();
        dense_values.resize(n, init);
        logical_size = n;
    }
    //! Append a value (used by ExtractDoubleColumn-style fills via MutableDense).
    inline void PushBack(double v) {
        EnsureDense();
        dense_values.push_back(v);
        logical_size = dense_values.size();
    }
    //! Mutable access to the underlying dense vector. Forces Dense kind.
    //! After mutating size externally (push_back/resize), call SyncSize().
    vector<double> &MutableDense() {
        EnsureDense();
        return dense_values;
    }
    //! Refresh logical_size after external mutation through MutableDense().
    void SyncSize() {
        D_ASSERT(kind == Kind::Dense);
        logical_size = dense_values.size();
    }
    //! For helpers that need to know whether all values are integral.
    //! Scalar: O(1). Dense: O(n). SparseMasked: O(1) (uniform sparse_value).
    bool AllIntegral() const {
        if (kind == Kind::Scalar) {
            return std::floor(scalar_value) == scalar_value;
        }
        if (kind == Kind::SparseMasked) {
            return std::floor(sparse_value) == sparse_value;
        }
        for (double c : dense_values) {
            if (std::floor(c) != c) return false;
        }
        return true;
    }
};

//! Represents an evaluated constraint ready for the solver
struct EvaluatedConstraint {
    vector<idx_t> variable_indices;           // Which variable for each term
    vector<CoefficientColumn> row_coefficients;  // [term_idx] = coefficient column for that term
    //! Printable symbolic label of each term's coefficient expression (parallel to
    //! variable_indices). For a data-weighted aggregate term like `SUM(buy * l_extendedprice)`
    //! this is `l_extendedprice`, so infeasible diagnosis can render the summed clause
    //! symbolically instead of dumping the per-row numeric fan-out. Empty entries (constant
    //! coefficients / non-aggregate rows) fall back to the numeric reconstruction.
    vector<string> coefficient_labels;
    //! Parallel to variable_indices. Records which canonical reducer owns each
    //! linear term so fixed LHS offsets cannot silently acquire generic SUM semantics.
    vector<LinearTermReduction> linear_term_reductions;
    CoefficientColumn rhs_values;                // RHS column (logical size = num_rows)
    ExpressionType comparison_type;
    bool lhs_is_aggregate = false;            // True if original LHS was an aggregate (e.g., SUM(...))
    //! Which hard MIN/MAX clause this row is, as an index into `minmax_clause_labels`.
    //! Names the extremum column the clause becomes; not a variable.
    idx_t minmax_clause_idx = DConstants::INVALID_INDEX;
    //! "min" or "max", empty when this row is not a hard MIN/MAX. THIS is the marking
    //! both arms read: it is set whenever stage 05 tagged the row, whichever formulation
    //! was chosen for it.
    string minmax_agg_type;
    idx_t ne_indicator_idx = DConstants::INVALID_INDEX;      // Indicator var idx for not-equal
    //! AVG(x) <> K path: original LHS was AVG; instead of dividing LHS coefficients by the
    //! AVG denominator (which would produce fractional coefficients and trip the NE
    //! integer-step guard), we keep LHS as SUM and multiply the per-group RHS by group size
    //! inside the deferred NE expansion.
    bool ne_avg_rhs_scale = false;
    //! ABS envelope tagging. `abs_aux_idx` names the ABS AUXILIARY this row bounds, and
    //! marks the row as one of the pair RewriteAbs emitted for it: abs_is_pos_bound
    //! distinguishes C1 (aux >= inner, true) from C2 (aux >= -inner, false). Both arms
    //! find their rows through it -- the lowering arm to hang the Big-M upper envelope
    //! off, the native arm to read the inner expression out of.
    //!
    //! It keys on the auxiliary and not on the sign indicator because the indicator only
    //! exists on the lowering arm: a natively-stated ABS needs no binary at all, so the
    //! only index both arms are guaranteed to have is the auxiliary's.
    idx_t abs_aux_idx = DConstants::INVALID_INDEX;
    bool abs_is_pos_bound = false;

    //! Bilinear terms in this constraint (var_a * var_b with per-row coefficients)
    struct BilinearTerm {
        idx_t var_a;
        idx_t var_b;
        CoefficientColumn row_coefficients;  // logical size = num_rows
    };
    vector<BilinearTerm> bilinear_terms;

    //! Quadratic groups in this constraint. Each POWER(expr, 2) or self-product
    //! becomes a separate group with its own sign and inner coefficients.
    //! The model builder computes outer-product Q = sign * A^T A for each group
    //! and accumulates all groups into the same QuadraticConstraint.
    struct QuadraticGroup {
        double sign = 1.0;
        vector<idx_t> variable_indices;             // [term_idx]
        vector<CoefficientColumn> row_coefficients; // [term_idx]
    };
    vector<QuadraticGroup> quadratic_groups;
    bool has_quadratic = false;
    ConstraintKind kind = ConstraintKind::USER_PARAMETER;

    //! Stable user-clause identity retained when one cast comparison expands to
    //! multiple boundary rows.
    idx_t source_clause_id = DConstants::INVALID_INDEX;
    //! Elastic grouping identity. Kept separate because one source comparison can
    //! produce multiple independently repairable rows/directions.
    idx_t repair_group_id = DConstants::INVALID_INDEX;
    //! True when the complete canonical RHS is one query-wide scalar value. The value
    //! is one editable knob shared across every row this clause emits, so the elastic
    //! engine collapses those rows to ONE shared slack (ElasticShape::SHARED_SCALAR);
    //! a data-backed RHS stays per-row independent.
    bool rhs_is_shared_scalar = false;
    //! What a lowering added to the user's literal to build this row's RHS, so the
    //! diagnosis can subtract it back off and quote the number the user typed. A hard
    //! `MAX(e) >= K` lowers to N rows of `e_i - M*y_i >= K - M`: the clause is still one
    //! editable K and the row still repairs by loosening it, but the row's own RHS is
    //! `K - M` and quoting that would name a bound nobody wrote. 0.0 means the row's RHS
    //! IS the user's literal, which is the ordinary case.
    double rhs_mechanism_offset = 0.0;

    //! Symbolic name of a data-backed (non-shared-scalar) RHS expression (`x <= cap_col`
    //! → "cap_col"). Lets query-mode infeasible diagnosis render a virtual offset
    //! (`x <= cap_col + delta`) instead of a numeric representative. Empty for a shared
    //! scalar RHS. Stamped onto ConstraintProvenance::rhs_label at the per-row builder site.
    string rhs_label;

    //! True when this is a pure-linear AVG aggregate whose row coefficients were
    //! pre-scaled by 1/N_g for the AVG→SUM rewrite. Propagated to the row provenance
    //! so infeasible diagnosis renders the clause as `AVG(...)` (and the slack is
    //! already in the user's AVG units — report it raw). I2.d.
    bool avg_scaled = false;

    //! Unified WHEN+PER row→group mapping
    //! Empty = all rows in one implicit group (fast path: no WHEN, no PER)
    //! DConstants::INVALID_INDEX = row excluded (WHEN filter or NULL PER value)
    //! 0..K-1 = group assignment
    vector<idx_t> row_group_ids;
    idx_t num_groups = 0;                     // 0 = ungrouped, >0 = number of distinct groups

    //! Printable PER key per group (size num_groups when PER-grouped; empty otherwise).
    //! Surfaces each group's key value (`'a'`, or `EU, 2024` for a composite key) so
    //! infeasible diagnosis can identify which group an edit belongs to. Stamped onto
    //! ConstraintProvenance::group_label at the aggregate-PER emission sites.
    vector<string> group_labels;

    //! WHEN/PER qualifier text for this clause (`PER grp`, `WHEN g='a' PER (region, year)`),
    //! same for every group. Stamped onto ConstraintProvenance::qualifier so infeasible
    //! diagnosis can append it to the reconstructed clause label. Empty when unqualified.
    string qualifier;

    //! CSR-style group→rows index, computed lazily by EnsureGroupCSR().
    //! group_offsets has size num_groups + 1 when populated; empty otherwise.
    //! Group g's active rows occupy [group_offsets[g], group_offsets[g+1]) in group_row_ids.
    //! Empty groups (filtered out by WHEN) have group_offsets[g] == group_offsets[g+1].
    //! Built once and reused across the model builder, deferred-NE expansion, and
    //! PER objective MIN/MAX paths to avoid repeated O(num_rows) reconstructions.
    mutable vector<idx_t> group_offsets;
    mutable vector<idx_t> group_row_ids;
};

//! Sum decision-free LHS terms over a row range after checking that each one is
//! explicitly owned by a canonical SUM reducer.
double SumFixedAggregateLhsOffset(const EvaluatedConstraint &constraint,
                                  const vector<idx_t> *rows, idx_t begin, idx_t end,
                                  const char *error_context);

//! Maps result rows to unique entities in a source table.
//! Used for table-scoped decision variables where one variable value
//! is shared across all result rows from the same base table entity.
struct EntityMapping {
    idx_t num_entities = 0;          //! Number of distinct entities in this table
    vector<idx_t> row_to_entity;     //! [row_idx] -> entity_id (0..num_entities-1)
};

//! Links a McCormick auxiliary `w` to the pair it linearizes: `w = b * x`, with
//! `b` Boolean and `x` bounded. Stage 05 chooses the auxiliary and records the
//! link; stage 06 derives the corner constraints from `x`'s evaluated bounds.
struct BilinearLinkSpec {
    idx_t aux_idx;       //!< auxiliary variable w
    idx_t bool_var_idx;  //!< Boolean variable b
    idx_t other_var_idx; //!< non-Boolean variable x
};

//! Links an ABS auxiliary to its binary sign indicator. Stage 05 emits the two
//! lower bounds (`aux >= inner`, `aux >= -inner`) and records the link; under
//! MAXIMIZE those alone leave `aux` free to run above `|inner|`, so stage 06
//! derives the matching Big-M upper bounds that pin it.
struct AbsMaximizeLinkSpec {
    idx_t aux_idx; //!< ABS auxiliary variable
    //! Binary sign indicator, or INVALID_INDEX when the backend states ABS natively --
    //! the native arm has no Big-M to switch, so no indicator is allocated for it.
    idx_t y_idx;
    //! The largest |inner| any row can reach, filled in by DeriveAbsAuxiliaryBounds.
    //! It is both the Big-M the lowering path needs and the auxiliary's upper box, so
    //! it is derived once, before any other linearizer reads that box.
    double abs_range = 0.0;
    //! True when no finite `abs_range` exists because a contributing variable is
    //! unbounded. The lowering path cannot proceed (it is refused up front); the
    //! native path does not care, because a general constraint needs no Big-M.
    bool range_unbounded = false;
};

//! A construct handed to the backend whole, instead of being lowered into rows.
//!
//! Named in flat COLUMNS, not expressions, and that is the design decision: every
//! backend's general-constraint API relates variables to variables, so a record
//! carrying an expression would force each adapter to synthesize the same auxiliary
//! column and equality row. That is routing, and routing belongs in the gate — an
//! adapter only translates. The linear part of the construct is therefore an ordinary
//! model row emitted alongside this record.
//!
//! An adapter never receives a kind its backend did not declare in
//! SolverConstructSupport, so it may treat an unknown kind as an internal error.
enum class GeneralConstraintKind : uint8_t {
    ABS, //!< result = |argument|
    MIN, //!< result = min(arguments)
    MAX  //!< result = max(arguments)
};

//! Which construct flag a backend must declare before it may be handed a general
//! constraint of this kind. The ONE place a kind turns into a capability question:
//! adding a kind adds a row to the table below, not another `?:` at a call site. MIN
//! and MAX share a row because they share one flag and one Gurobi symbol pair.
//!
//! Lives beside the enum rather than beside SolverConstructSupport so a kind added
//! here without a flag cannot be missed. Such a kind reads as UNDECLARED, which every
//! caller turns into a loud internal error rather than a silent wrong lowering.
inline bool DeclaresGeneralConstraint(const SolverConstructSupport &constructs, GeneralConstraintKind kind) {
    struct KindFlag {
        GeneralConstraintKind kind;
        bool SolverConstructSupport::*flag;
    };
    static constexpr KindFlag KIND_FLAGS[] = {
        {GeneralConstraintKind::ABS, &SolverConstructSupport::abs},
        {GeneralConstraintKind::MIN, &SolverConstructSupport::min_max},
        {GeneralConstraintKind::MAX, &SolverConstructSupport::min_max},
    };
    for (auto &entry : KIND_FLAGS) {
        if (entry.kind == kind) {
            return constructs.*entry.flag;
        }
    }
    return false;
}

struct GeneralConstraintSpec {
    GeneralConstraintKind kind = GeneralConstraintKind::ABS;
    //! Flat column the construct's value lands in (Gurobi's `resvar`).
    int result_column = -1;
    //! Flat columns of the construct's arguments. ABS takes exactly one.
    vector<int> argument_columns;
    //! Clause origin, in the same loose fields `RawConstraint` uses — the full
    //! `ConstraintProvenance` lives in stage 06's header, which includes this one.
    //! `SolverModel::Build` stamps these into a real provenance record. A native
    //! construct has no matrix row for the elastic engine to slacken, but it still
    //! has to be nameable.
    idx_t source_clause_id = DConstants::INVALID_INDEX;
    idx_t repair_group_id = DConstants::INVALID_INDEX;
};

//! Input for the deterministic solver
struct SolverInput {
    idx_t num_rows;
    idx_t num_decide_vars;

    // Per-variable configuration (size = num_decide_vars)
    vector<LogicalType> variable_types;
    vector<double> lower_bounds;
    vector<double> upper_bounds;

    //! The subset of the column box that no diagnosis will ever open: a variable's
    //! intrinsic domain (BOOLEAN 0/1, default non-negativity) and nothing else.
    //!
    //! `lower_bounds` / `upper_bounds` above accumulate three different things — the
    //! intrinsic domain, user bounds absorbed into the box by stage 05, and implied
    //! tightenings derived by `DecidePropagateImpliedBounds`. The last two are backed by
    //! rows the elastic engine may loosen, and it reverts them for exactly that reason
    //! (see the `user_absorbed_bounds` re-emission in `PhysicalDecide::Finalize`), so a
    //! rewrite that bakes one into a constraint's *structure* cannot be reverted with it
    //! and would misstate the query under repair.
    //!
    //! A rewrite that changes what a constraint means depending on a bound must
    //! therefore read these, not the box above. A Big-M constant may read either: it
    //! only has to dominate, and a loosened bound makes it conservative rather than
    //! wrong. Size = num_decide_vars.
    vector<double> rigid_lower_bounds;
    vector<double> rigid_upper_bounds;

    // Constraints
    vector<EvaluatedConstraint> constraints;
    vector<ConstraintSourceInfo> constraint_sources;

    // Formulations stage 05 chose and stage 06 encodes. Each is a tag pointing at
    // auxiliary variables the optimizer already created; the rows that realize
    // them need evaluated bounds, so they are derived at stage 06.
    vector<BilinearLinkSpec> bilinear_links;
    vector<AbsMaximizeLinkSpec> abs_maximize_links;

    // Linear objective
    vector<CoefficientColumn> objective_coefficients; // [term_idx] = column of length num_rows
    vector<idx_t> objective_variable_indices;          // [term_idx]
    DecideSense sense;

    // Quadratic objective: inner linear expression of SUM(sign * POWER(expr, 2)).
    // When has_quadratic_objective is true, the objective includes a quadratic
    // component. The inner expression coefficients are stored per-term and per-row,
    // just like the linear objective. The model builder expands these into the
    // Q matrix via outer products: Q = sign * A^T A.
    // sign = +1.0 → PSD Q (convex), sign = -1.0 → NSD Q (concave).
    bool has_quadratic_objective = false;
    double quadratic_sign = 1.0;
    vector<CoefficientColumn> quadratic_inner_coefficients; // [term_idx]
    vector<idx_t> quadratic_inner_variable_indices;          // [term_idx]

    // Bilinear objective terms: products of two different DECIDE variables.
    // Only used for non-Boolean pairs (Boolean×anything is McCormick-linearized).
    struct BilinearObjectiveTerm {
        idx_t var_a;                       // First DECIDE variable index
        idx_t var_b;                       // Second DECIDE variable index
        CoefficientColumn row_coefficients; // logical size = num_rows
    };
    vector<BilinearObjectiveTerm> bilinear_objective_terms;

    // Objective PER grouping (mirrors constraint row_group_ids pattern)
    vector<idx_t> objective_row_group_ids;  // per-row group assignment
    idx_t objective_num_groups = 0;          // 0 = ungrouped
    //! CSR-style group→rows index for objectives (mirrors EvaluatedConstraint).
    vector<idx_t> objective_group_offsets;
    vector<idx_t> objective_group_row_ids;

    // Global auxiliary variables (exist once, not replicated per row)
    // Appended after the per-row grid at indices num_rows * num_decide_vars + i
    idx_t num_global_vars = 0;
    vector<LogicalType> global_variable_types;
    vector<double> global_lower_bounds;
    vector<double> global_upper_bounds;
    vector<double> global_obj_coeffs;  // Objective coefficients for global vars
    //! Parallel to `global_variable_types`. True where an auxiliary was deliberately
    //! left free because the expression it stands for reaches a decision variable with
    //! no finite bound, so no box exists to give it. Every other auxiliary must carry a
    //! derived box — `SolverModel::Build` asserts exactly that, which is what keeps a
    //! free continuous column from creeping back in as the accidental default.
    vector<bool> global_bounds_unbounded;
    //! Parallel to `global_variable_types`. Clause text for a `<>` indicator
    //! global (e.g. "(SUM(x) <> 5)"), so the infeasible removal dial can name a
    //! dropped aggregate `<>`; empty for every other global aux (MIN/MAX,
    //! McCormick, …). Surfaced via BuildColumnProvenance onto the global column.
    vector<string> global_variable_labels;

    // Raw ILP constraints involving global variables (indices are absolute into the
    // flattened variable array including global vars)
    struct RawConstraint {
        vector<int> indices;
        vector<double> coefficients;
        char sense;     // '<' (<=), '>' (>=), '=' (==)
        double rhs;
        ConstraintKind kind = ConstraintKind::STRUCTURAL;
        ElasticShape shape = ElasticShape::UNSET;
        idx_t source_clause_id = DConstants::INVALID_INDEX;
        idx_t repair_group_id = DConstants::INVALID_INDEX;
        //! Flat column of the `<>` disjunction binary this row belongs to (mirrors
        //! ConstraintProvenance::indicator_col). Set at the `<>` sites so the infeasible
        //! removal dial groups the rows of one clause; INVALID otherwise.
        idx_t indicator_col = DConstants::INVALID_INDEX;
        //! PER/WHEN group id, its printable key, the reducer qualifier text, and whether
        //! the clause's LHS was an aggregate — the display half of the provenance every
        //! row carries. A per-row clause emitted through `EvaluatedConstraint` gets these
        //! from its `row_group_ids` / `group_labels` / `qualifier`; a clause emitted in
        //! flat columns has to state them, and does, so that changing a row's SHAPE never
        //! changes what diagnosis calls it.
        idx_t group_key = DConstants::INVALID_INDEX;
        string group_label;
        string qualifier;
        bool is_aggregate = false;
        //! What a lowering (or a folded LHS constant) added to the user's literal to
        //! build this row's `rhs`; the reported clause quotes `rhs` minus this.
        double rhs_mechanism_offset = 0.0;
        //! Symbolic name of a data-backed RHS column, for a PER_ROW_DATA row. Empty for a
        //! literal/shared bound.
        string rhs_label;
    };
    vector<RawConstraint> global_constraints;

    //! A ROW, conditioned: it holds whenever `binary_column == binary_value`, and says
    //! nothing otherwise. This is the semantic form of a disjunctive clause, and it is
    //! its own list rather than a `GeneralConstraintSpec` kind because the kinds there
    //! relate columns to columns and carry no row at all.
    //!
    //! A `<>` is emitted this way and only this way. On a backend that states indicator
    //! constraints the pair goes down as written; on one that does not,
    //! `LowerDecideConstructs` turns each half into an ordinary matrix row with a Big-M
    //! that switches it. Holding the row itself — rather than a description of one — is
    //! what makes that lowering a rewrite of one field, and what keeps infeasible
    //! diagnosis reaching the clause either way: the removal dial walks these alongside
    //! the matrix rows.
    struct IndicatorConstraintSpec {
        int binary_column = -1;
        int binary_value = 1;
        //! The implied row, provenance included. Composed rather than restated so a
        //! conditional row and the matrix row it lowers to cannot drift apart.
        RawConstraint row;
    };
    vector<IndicatorConstraintSpec> indicator_constraints;

    //! Diagnosis text of each hard MIN/MAX clause (`MAX(x * c)`), indexed by
    //! `EvaluatedConstraint::minmax_clause_idx`. Names the extremum column the clause
    //! becomes, so a repair reads the clause rather than an internal column.
    vector<string> minmax_clause_labels;

    //! Constructs left native for the backend, in flat columns. Emitted at stage 08
    //! once the VarIndexer exists — like `global_constraints`, and for the same reason.
    //! Empty whenever the chosen backend declared the construct unsupported, in which
    //! case the lowering path filled `constraints` instead.
    vector<GeneralConstraintSpec> general_constraints;

    // --- Table-scoped variable support ---

    //! Entity mappings: one per EntityScopeInfo (source table with scoped vars)
    vector<EntityMapping> entity_mappings;

    //! Per-variable scope (row / entity / query-wide scalar).
    //! For entity-scoped variables, entity_scope_idx indexes entity_mappings.
    vector<DecideVarScopeInfo> variable_scopes;

    //! When set (infeasible diagnosis armed), SolverModel::Build does NOT throw on an
    //! inverted column box (col_lower > col_upper). The model is built and retained so the
    //! elastic engine can reset the box to the intrinsic domain and diagnose the conflict
    //! as a least-change edit instead of dying with the static "conflicting bounds" error.
    //! Off by default: the fast build-time infeasible throw is kept for non-diagnosis solves.
    bool tolerate_infeasible_bounds = false;
};

} // namespace duckdb
