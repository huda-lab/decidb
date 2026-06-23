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
    CoefficientColumn rhs_values;                // RHS column (logical size = num_rows)
    ExpressionType comparison_type;
    bool lhs_is_aggregate = false;            // True if original LHS was an aggregate (e.g., SUM(...))
    idx_t minmax_indicator_idx = DConstants::INVALID_INDEX;  // Indicator var idx for hard MIN/MAX
    string minmax_agg_type;                    // "min" or "max" (empty if not minmax)
    idx_t ne_indicator_idx = DConstants::INVALID_INDEX;      // Indicator var idx for not-equal
    //! AVG(x) <> K path: original LHS was AVG; instead of dividing LHS coefficients by the
    //! AVG denominator (which would produce fractional coefficients and trip the NE
    //! integer-step guard), we keep LHS as SUM and multiply the per-group RHS by group size
    //! inside the deferred NE expansion.
    bool ne_avg_rhs_scale = false;
    //! ABS MAXIMIZE Big-M upper-bound tagging.
    //! abs_y_idx != INVALID_INDEX marks a lower-bound ABS constraint (aux >= ±inner)
    //! emitted by RewriteAbs for a MAXIMIZE objective. abs_is_pos_bound distinguishes
    //! C1 (aux >= inner, true) from C2 (aux >= -inner, false). Used at finalization to
    //! derive and emit the two upper-bound constraints that pin aux = |inner|.
    idx_t abs_y_idx = DConstants::INVALID_INDEX;
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

    //! Unified WHEN+PER row→group mapping
    //! Empty = all rows in one implicit group (fast path: no WHEN, no PER)
    //! DConstants::INVALID_INDEX = row excluded (WHEN filter or NULL PER value)
    //! 0..K-1 = group assignment
    vector<idx_t> row_group_ids;
    idx_t num_groups = 0;                     // 0 = ungrouped, >0 = number of distinct groups

    //! CSR-style group→rows index, computed lazily by EnsureGroupCSR().
    //! group_offsets has size num_groups + 1 when populated; empty otherwise.
    //! Group g's active rows occupy [group_offsets[g], group_offsets[g+1]) in group_row_ids.
    //! Empty groups (filtered out by WHEN) have group_offsets[g] == group_offsets[g+1].
    //! Built once and reused across the model builder, deferred-NE expansion, and
    //! PER objective MIN/MAX paths to avoid repeated O(num_rows) reconstructions.
    mutable vector<idx_t> group_offsets;
    mutable vector<idx_t> group_row_ids;
};

//! Maps result rows to unique entities in a source table.
//! Used for table-scoped decision variables where one variable value
//! is shared across all result rows from the same base table entity.
struct EntityMapping {
    idx_t num_entities = 0;          //! Number of distinct entities in this table
    vector<idx_t> row_to_entity;     //! [row_idx] -> entity_id (0..num_entities-1)
};

//! Input for the deterministic solver
struct SolverInput {
    idx_t num_rows;
    idx_t num_decide_vars;

    // Per-variable configuration (size = num_decide_vars)
    vector<LogicalType> variable_types;
    vector<double> lower_bounds;
    vector<double> upper_bounds;

    // Constraints
    vector<EvaluatedConstraint> constraints;

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

    // Raw ILP constraints involving global variables (indices are absolute into the
    // flattened variable array including global vars)
    struct RawConstraint {
        vector<int> indices;
        vector<double> coefficients;
        char sense;     // '<' (<=), '>' (>=), '=' (==)
        double rhs;
        ConstraintKind kind = ConstraintKind::STRUCTURAL;
    };
    vector<RawConstraint> global_constraints;

    // --- Table-scoped variable support ---

    //! Entity mappings: one per EntityScopeInfo (source table with scoped vars)
    vector<EntityMapping> entity_mappings;

    //! Per-variable scope: INVALID_INDEX = row-scoped (default),
    //! otherwise index into entity_mappings
    vector<idx_t> variable_entity_scope;
};

} // namespace duckdb
