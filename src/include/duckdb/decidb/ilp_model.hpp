//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/ilp_model.hpp
//
// Solver-agnostic optimization model representation. Built once from
// SolverInput, consumed by any solver backend (HiGHS, Gurobi, etc.).
// Supports LP, MILP, and convex QP/MIQP.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/decidb/solver_input.hpp"

#include <map>

namespace duckdb {

//! Maps (decide_var_idx, row) pairs to flat solver variable indices.
//! Supports mixed row-scoped and entity-scoped variables with a three-block layout:
//!   Block 1: row-scoped vars  (num_rows * num_row_vars entries)
//!   Block 2: entity-scoped vars (sum of num_entities per scope)
//!   Block 3: global auxiliary vars
struct VarIndexer {
    idx_t num_rows = 0;
    idx_t num_row_vars = 0;          //!< Count of row-scoped decide variables
    idx_t entity_block_start = 0;    //!< First index of entity block
    idx_t global_block_start = 0;    //!< First index of global block
    idx_t total_vars = 0;

    //! Per decide-variable: true if entity-scoped
    vector<bool> is_entity_scoped;

    //! For row-scoped vars: offset within the row block (var_idx → position in row)
    vector<idx_t> row_var_offset;

    //! For entity-scoped vars: base offset in entity block
    vector<idx_t> entity_var_base;

    //! For entity-scoped vars: index into entity_mappings source
    vector<idx_t> var_entity_mapping_idx;

    //! Pointer to entity mappings (not owned — caller ensures lifetime)
    const vector<EntityMapping> *entity_mappings_ref = nullptr;
    //! Owned copy of entity mappings (used when VarIndexer must outlive its source)
    vector<EntityMapping> entity_mappings_owned;

    //! Get the flat solver variable index for a given decide variable at a given row
    inline idx_t Get(idx_t var_idx, idx_t row) const {
        if (!is_entity_scoped[var_idx]) {
            return row * num_row_vars + row_var_offset[var_idx];
        }
        auto &mappings = entity_mappings_ref ? *entity_mappings_ref : entity_mappings_owned;
        auto &mapping = mappings[var_entity_mapping_idx[var_idx]];
        idx_t entity_id = mapping.row_to_entity[row];
        return entity_var_base[var_idx] + entity_id;
    }

    //! Get the number of instances (copies) for a given decide variable
    inline idx_t NumInstances(idx_t var_idx) const {
        if (!is_entity_scoped[var_idx]) {
            return num_rows;
        }
        auto &mappings = entity_mappings_ref ? *entity_mappings_ref : entity_mappings_owned;
        return mappings[var_entity_mapping_idx[var_idx]].num_entities;
    }

    //! Build a VarIndexer that OWNS a copy of entity_mappings.
    //! Safe to use after the SolverInput is destroyed (e.g., stored on gstate for readback).
    static VarIndexer Build(const SolverInput &input);

    //! Build a VarIndexer that REFERENCES entity_mappings without copying.
    //! Caller must ensure the SolverInput outlives this VarIndexer.
    //! Used for temporary indexers (pre_indexer, model builder).
    static VarIndexer BuildRef(const SolverInput &input);
};

class DecideInfeasibleModelException : public Exception {
public:
	explicit DecideInfeasibleModelException(const string &message)
	    : Exception(ExceptionType::INVALID_INPUT, message) {
	}

	template <typename... ARGS>
	explicit DecideInfeasibleModelException(const string &message, ARGS... params)
	    : DecideInfeasibleModelException(Exception::ConstructMessage(message, params...)) {
	}
};

//! Row → clause provenance carried by every emitted constraint (F2). Lets diagnosis
//! report at the user-clause level instead of at raw matrix rows.
struct ConstraintProvenance {
    //! Index into SolverInput::constraints of the user clause that produced this row.
    //! DConstants::INVALID_INDEX for synthetic/structural rows with no user clause.
    idx_t clause_id = DConstants::INVALID_INDEX;
    //! PER/WHEN group id at emission (or the row id for per-row clauses).
    //! DConstants::INVALID_INDEX when the clause is ungrouped.
    idx_t group_key = DConstants::INVALID_INDEX;
    //! User parameter vs rigid mechanism/structural row (see ConstraintKind).
    ConstraintKind kind = ConstraintKind::USER_PARAMETER;
    //! Elastic-diagnosis shape (I2): does this row share ONE slack with its
    //! (clause_id, group_key) siblings, or get its own? Default PER_ROW_DATA.
    ElasticShape shape = ElasticShape::PER_ROW_DATA;
    //! True when the row's coefficients were pre-scaled by 1/N_g for an AVG rewrite.
    //! The slack is then already in the user's AVG units (report the raw slack).
    bool avg_scaled = false;
    //! True when the user wrote a strict `<` / `>` (a δ offset was baked into `rhs`
    //! at build time). `typed_k` carries the user's original literal so the reported
    //! suggestion can be re-quoted against it instead of the δ-adjusted rhs.
    bool strict = false;
    double typed_k = 0.0;
    //! Flat solver column of the `<>` disjunction binary this row belongs to (I4).
    //! Set at the `<>` mechanism sites — per-row (row-scoped indicator column) and
    //! aggregate (global-block z, propagated from SolverInput::RawConstraint) — so it
    //! doubles as the removal marker (`!= INVALID` ⇒ remove-only row) and the grouping
    //! key (rows sharing one indicator = one `<>` instance). Also sources the removal
    //! Big-M (|row coeff on this column|) and the user-facing label. INVALID otherwise.
    idx_t indicator_col = DConstants::INVALID_INDEX;
    //! Printable PER key of this row's group (`'a'`, or `EU, 2024` for a composite key).
    //! Empty when the clause is ungrouped or not PER-grouped. Lets infeasible diagnosis
    //! identify which group an edit belongs to and emit a `group` EAV row, so PER
    //! aggregates can fold to `SUM(x)` without colliding in the relation (Facet A).
    string group_label;
    //! True when the LHS was an aggregate (`SUM`/`AVG`/...). Lets the diagnosis wrap a
    //! single-row / WHEN group's reconstruction in `SUM(...)` even when there is no
    //! per-row fan-out to key on (distinct from SHARED_LITERAL, which a per-row constant
    //! bound also carries) (Facet B).
    bool is_aggregate = false;
    //! WHEN/PER qualifier text (`PER grp`, `PER region, year`, or a `WHEN` predicate)
    //! appended to the reconstructed clause label so it is fully recognizable. Empty when
    //! the clause has no WHEN/PER qualifier (Facet C).
    string qualifier;
    //! Symbolic coefficient label per summed decide variable, for a data-weighted aggregate
    //! (`SUM(buy * l_extendedprice)`): `decide_var_idx → "l_extendedprice"`. Lets infeasible
    //! diagnosis render `SUM(buy * l_extendedprice)` instead of the per-row numeric fan-out
    //! (`24710*buy + 56688*buy + …`) when the summed coefficients are data-varying, so there
    //! is no single literal to quote. Empty for uniform-coefficient or non-aggregate rows.
    vector<std::pair<idx_t, string>> weight_labels;
    //! Symbolic name of a data-backed (PER_ROW_DATA) RHS column (`x <= cap_col` → "cap_col").
    //! Lets query-mode infeasible diagnosis render a virtual offset (`x <= cap_col + delta`)
    //! instead of the numeric representative RHS. Empty for a literal/shared RHS or when no
    //! column name is available (the render then falls back to the numeric representative).
    string rhs_label;
};

//! A single linear constraint: sum(coefficients[i] * x[indices[i]]) <sense> rhs
struct ModelConstraint {
    vector<int> indices;       //!< Variable indices into the flattened variable array
    vector<double> coefficients; //!< Coefficient for each variable
    char sense;                //!< '<' (<=), '>' (>=), '=' (==)
    double rhs;                //!< Right-hand side value
    ConstraintProvenance provenance; //!< Row → clause origin (F2)
};

//! Solver-agnostic optimization model, ready for any backend to consume.
//! Supports linear objectives (LP/MILP) and convex quadratic objectives (QP/MIQP).
struct SolverModel {
    idx_t num_vars;            //!< Total number of variables across all three blocks (row-scoped + entity-scoped + global auxiliary)

    //! Per-variable configuration (size = num_vars)
    vector<double> col_lower;  //!< Lower bounds
    vector<double> col_upper;  //!< Upper bounds
    vector<bool> is_integer;   //!< True for INTEGER/BOOLEAN vars, false for REAL (continuous)
    vector<bool> is_binary;    //!< True if binary (0/1), subset of integer

    //! Linear objective: minimize/maximize c^T x
    vector<double> obj_coeffs; //!< Coefficient per variable (linear part)
    bool maximize;             //!< True = maximize, false = minimize

    //! Quadratic objective: (1/2) x^T Q x (added to linear part).
    //! Stored in COO (coordinate) format, lower triangle only.
    //! Empty when the objective is purely linear (LP/MILP).
    vector<int> q_rows;        //!< Row indices into Q
    vector<int> q_cols;        //!< Column indices into Q
    vector<double> q_vals;     //!< Values in Q
    bool has_quadratic_obj = false;
    //! True when the quadratic objective + sense combination is non-convex.
    //! Non-convex when: MAXIMIZE + PSD Q, or MINIMIZE + NSD Q.
    //! Gurobi handles this with NonConvex=2; HiGHS cannot solve it.
    bool nonconvex_quadratic = false;

    //! Constraints (linear)
    vector<ModelConstraint> constraints;

    //! Quadratic constraints: sum(linear) + sum(q * x_i * x_j) <sense> rhs
    //! Used for bilinear terms in constraints (QCQP). Gurobi only.
    struct QuadraticConstraint {
        vector<int> linear_indices;
        vector<double> linear_coefficients;
        vector<int> q_rows;
        vector<int> q_cols;
        vector<double> q_coefficients;
        char sense;
        double rhs;
        ConstraintProvenance provenance; //!< Row → clause origin (F2)
    };
    vector<QuadraticConstraint> quadratic_constraints;

    //! Build a SolverModel from a SolverInput (the shared model-building logic).
    //! Takes a non-const reference because raw global constraints are moved out of `input`
    //! into `model.constraints` to avoid deep copies.
    //! `indexer` must already be constructed for `input` (typically built once in
    //! PhysicalDecide::Finalize() and threaded through).
    static SolverModel Build(SolverInput &input, const VarIndexer &indexer);
};

enum class ConstraintRowType : uint8_t { LINEAR, QUADRATIC };

struct ConstraintRowRef {
	ConstraintRowType type = ConstraintRowType::LINEAR;
	idx_t index = DConstants::INVALID_INDEX;
};

//! Reverse index over linear and quadratic constraint rows. `by_clause` groups all
//! emitted rows for a clause; `by_clause_group` narrows to one PER/WHEN group.
struct ClauseRowIndex {
	std::map<idx_t, vector<ConstraintRowRef>> by_clause;
	std::map<std::pair<idx_t, idx_t>, vector<ConstraintRowRef>> by_clause_group;
};

//! Reverse index: user clause_id / (clause_id, group_key) → emitted row refs (F2/B3).
//! Rows with INVALID clause ids are skipped. Ordered maps keep reporting/tests
//! deterministic. Structural rows are retained in the index and filtered by
//! provenance.kind by consumers that need relaxable-only rows.
ClauseRowIndex BuildClauseRowIndex(const SolverModel &model);

//! What a flat solver column represents to the user (F6 variable provenance —
//! the column-side complement of ConstraintProvenance):
//!   USER       — a user decision variable; `label` is its name.
//!   AUX        — an auxiliary decision variable from a linearization rewrite
//!                (ABS / MIN-MAX / McCormick / <>); `label` is the user's original
//!                source expression captured at optimizer time.
//!   GLOBAL_AUX — an internal global-block auxiliary (e.g. composed MIN/MAX term)
//!                with no user-facing source; `label` is empty.
enum class ColumnKind : uint8_t { USER, AUX, GLOBAL_AUX };

//! Column → user-facing source provenance carried per flat solver column (F6).
struct ColumnProvenance {
    ColumnKind kind = ColumnKind::GLOBAL_AUX;
    //! User variable name (USER), source expression (AUX), or empty (GLOBAL_AUX).
    string label;
    //! Source decide-variable index; INVALID for global-block columns.
    idx_t decide_var_idx = DConstants::INVALID_INDEX;
    //! Row (row-scoped) or entity id (entity-scoped) this column instantiates;
    //! INVALID for global-block columns. Disambiguates repeated names.
    idx_t instance = DConstants::INVALID_INDEX;
};

//! Reverse map: flat solver column index → ColumnProvenance (F6). The column-side
//! complement of BuildClauseRowIndex. Pure (no planner/Expression dependency): the
//! caller pre-extracts per-decide-variable labels (user name or aux source
//! expression) and an is-aux flag. Output is sized `indexer.total_vars`; every
//! entry defaults to GLOBAL_AUX (the global block stays unnamed), then the row /
//! entity blocks are filled by inverting `indexer.Get(var, row)` over all
//! decide variables and rows — mirrors the solution-readback iteration.
//!   var_labels[v]  — name (USER) or source expression (AUX) for decide var v
//!   var_is_aux[v]  — true if decide var v is an auxiliary linearization variable
//!   global_var_labels[g] — clause text for global var g (e.g. an aggregate `<>`
//!                          indicator "(SUM(x) <> K)"), empty for unnamed globals.
//!                          Surfaces a label on the otherwise-unnamed global block
//!                          so the infeasible removal dial can name a dropped
//!                          aggregate `<>`. Parallel to SolverInput::global_variable_types.
vector<ColumnProvenance> BuildColumnProvenance(const VarIndexer &indexer,
                                               const vector<string> &var_labels,
                                               const vector<bool> &var_is_aux,
                                               const vector<string> &global_var_labels = {});

//! Build CSR group→rows index from a per-row group_id array.
//!   row_group_ids[r] in [0..num_groups) → row r belongs to that group
//!   row_group_ids[r] == DConstants::INVALID_INDEX → row r is excluded
//! Output:
//!   offsets has size num_groups + 1; group g's rows live in
//!   [offsets[g], offsets[g+1]) of flat_rows. Empty groups are still represented
//!   (offsets[g] == offsets[g+1]).
//! No-op if row_group_ids is empty (ungrouped fast path) or already populated.
void BuildGroupCSR(const vector<idx_t> &row_group_ids,
                   idx_t num_groups,
                   vector<idx_t> &offsets,
                   vector<idx_t> &flat_rows);

//! Reusable scratch storage for accumulating sparse coefficients keyed by flat
//! variable index. Two strategies, picked once per constraint via Begin*():
//!   - Dense:  vector<double> indexed by flat var idx, with a `touched` list
//!             so reset is O(touched), not O(total_vars).
//!   - Sparse: append-only (idx, coeff) pairs, sorted+merged at Flush().
//! The same instance can be reused across groups (within a constraint) and
//! across constraints.
struct SparseCoeffAccumulator {
    //! Dense path
    vector<double> dense;
    vector<int> touched;
    bool use_dense = false;

    //! Sparse path
    vector<std::pair<int, double>> pairs;

    //! Configure for dense accumulation. Allocates `total_vars` doubles
    //! (zero-initialized) on first use; subsequent calls only clear `touched`.
    void BeginDense(idx_t total_vars);

    //! Configure for sorted-pairs accumulation. `expected_size` is a hint for reserve().
    void BeginSparse(idx_t expected_size);

    //! Add a contribution. Branchless on use_dense via the pre-set mode.
    inline void Add(int idx, double coeff) {
        if (use_dense) {
            if (dense[idx] == 0.0) {
                touched.push_back(idx);
            }
            dense[idx] += coeff;
        } else {
            pairs.emplace_back(idx, coeff);
        }
    }

    //! Drain accumulated entries into out_indices/out_coefficients (dropping zeros)
    //! and reset for the next group. Dense path zeros only touched cells; sparse
    //! path sorts+merges pairs.
    void Flush(vector<int> &out_indices, vector<double> &out_coefficients);
};

} // namespace duckdb
