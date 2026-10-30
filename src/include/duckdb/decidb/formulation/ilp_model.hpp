//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/decidb/formulation/ilp_model.hpp
//
// Solver-agnostic optimization model representation. Built once from
// SolverInput, consumed by any solver backend (HiGHS, Gurobi, etc.).
// Supports LP, MILP, and convex QP/MIQP.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/decide_solver_capabilities.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"

#include <cmath>

namespace duckdb {

//! Maps (decide_var_idx, row) pairs to flat solver variable indices.
//! Supports row-scoped, keyed and query-wide variables with a four-block layout:
//!   Block 1: row-scoped vars  (num_rows * num_row_vars entries)
//!   Block 2: keyed vars (sum of num_classes per scope)
//!   Block 3: scalar (query-wide) vars, one column each
//!   Block 4: global auxiliary vars
//! Scalars sit *below* global_block_start so the "user decide-variable columns
//! occupy [0, global_block_start)" invariant relied on by the model builder and
//! the column labeller keeps holding.
struct VarIndexer {
    idx_t num_rows = 0;
    idx_t num_row_vars = 0;          //!< Count of row-scoped decide variables
    idx_t keyed_block_start = 0;    //!< First index of keyed block
    idx_t scalar_block_start = 0;    //!< First index of scalar block
    idx_t global_block_start = 0;    //!< First index of global (auxiliary) block
    idx_t total_vars = 0;

    //! Per decide-variable scope
    vector<DecideVarScope> var_scope;

    //! For row-scoped vars: offset within the row block (var_idx → position in row)
    vector<idx_t> row_var_offset;

    //! For keyed vars: base offset in keyed block
    vector<idx_t> keyed_var_base;

    //! For keyed vars: index into keyed_mappings source
    vector<idx_t> var_keyed_mapping_idx;

    //! For scalar vars: the single flat column index in the scalar block
    vector<idx_t> scalar_var_index;

    //! Inverses of the three decide-variable blocks, for OwnerOf(). Kept as tables
    //! rather than recomputed per lookup because the only caller that matters is a
    //! refusal, and a refusal must not cost a scan to phrase itself.
    vector<idx_t> row_offset_var;   //!< position within a row → var_idx
    vector<idx_t> scalar_slot_var;  //!< position within the scalar block → var_idx
    vector<idx_t> keyed_block_var; //!< keyed var_idx, ascending by base

    //! Owned keyed mappings. The indexer can therefore outlive its SolverInput.
    vector<KeyedMapping> keyed_mappings;

    //! Get the flat solver variable index for a given decide variable at a given row.
    //! A scalar variable ignores `row` — every row resolves to the same column.
    inline idx_t Get(idx_t var_idx, idx_t row) const {
        switch (var_scope[var_idx]) {
        case DecideVarScope::ROW:
            return row * num_row_vars + row_var_offset[var_idx];
        case DecideVarScope::GLOBAL:
            return scalar_var_index[var_idx];
        default: {
            auto &mapping = keyed_mappings[var_keyed_mapping_idx[var_idx]];
            idx_t class_id = mapping.row_to_class[row];
            return keyed_var_base[var_idx] + class_id;
        }
        }
    }

    //! Flat column for the inst-th instance of a variable, where
    //! inst < NumInstances(var_idx). Unlike Get(), which resolves through a result
    //! row, this walks a variable's own instances — rows for row-scoped variables,
    //! class ids for keyed ones, and the single column for a scalar.
    inline idx_t InstanceColumn(idx_t var_idx, idx_t inst) const {
        switch (var_scope[var_idx]) {
        case DecideVarScope::ROW:
            return inst * num_row_vars + row_var_offset[var_idx];
        case DecideVarScope::GLOBAL:
            return scalar_var_index[var_idx];
        default:
            return keyed_var_base[var_idx] + inst;
        }
    }

    //! The instance of a variable that data row `row` reads: the row itself, its class, or the single query-wide
    //! instance. Up to the block offset the inverse of Get(): `InstanceColumn(v, InstanceOf(v, row)) == Get(v, row)`.
    inline idx_t InstanceOf(idx_t var_idx, idx_t row) const {
        switch (var_scope[var_idx]) {
        case DecideVarScope::ROW:
            return row;
        case DecideVarScope::GLOBAL:
            return 0;
        default:
            return keyed_mappings[var_keyed_mapping_idx[var_idx]].row_to_class[row];
        }
    }

    //! The instance of `var_idx` that instance `inst` of `finer` reads, for a variable whose scope is no finer than
    //! `finer`'s: the instance of a query-wide variable is the only one, a row-scoped `finer` reads the instance its
    //! row does, and a keyed one reads the instance of a variable keyed alike (or of a row-scoped one: its own).
    inline idx_t InstanceUnder(idx_t var_idx, idx_t finer, idx_t inst) const {
        switch (var_scope[finer]) {
        case DecideVarScope::ROW:
            return InstanceOf(var_idx, inst);
        case DecideVarScope::GLOBAL:
            return 0;
        default:
            return var_scope[var_idx] == DecideVarScope::GLOBAL ? 0 : inst;
        }
    }

    //! The instance of `var_idx` that flat column `col` is, for a column in the decide-variable blocks: the inverse of
    //! InstanceColumn().
    inline idx_t InstanceOfColumn(idx_t var_idx, idx_t col) const {
        switch (var_scope[var_idx]) {
        case DecideVarScope::ROW:
            return col / num_row_vars;
        case DecideVarScope::GLOBAL:
            return 0;
        default:
            return col - keyed_var_base[var_idx];
        }
    }

    //! Get the number of instances (copies) for a given decide variable
    inline idx_t NumInstances(idx_t var_idx) const {
        switch (var_scope[var_idx]) {
        case DecideVarScope::ROW:
            return num_rows;
        case DecideVarScope::GLOBAL:
            return 1;
        default: {
            return keyed_mappings[var_keyed_mapping_idx[var_idx]].num_classes;
        }
        }
    }

    //! Which decide variable a flat column instantiates — the inverse of `Get()` and
    //! `InstanceColumn()`. `DConstants::INVALID_INDEX` for a global-block column, which
    //! no decide variable owns, and for a column past the end.
    //!
    //! Exists so a layer working in flat columns can still speak the user's language.
    //! A Big-M lowering reads column boxes, so its refusal has a column and needs a
    //! NAME; without this it could only say "some variable is unbounded".
    idx_t OwnerOf(idx_t col) const;

    //! Build a VarIndexer that owns a copy of keyed_mappings.
    //! Safe to use after the SolverInput is destroyed (e.g., stored on gstate for readback).
    static VarIndexer Build(const SolverInput &input);
};

//! The box a reader that tells the instances of a decision apart takes, by the data row that reads the instance. A
//! reader that takes the decision's box instead is sound, only looser.
//!
//! Two views, and a reader picks one. The rigid view (the default) refines the floor by the instance's own floor
//! (SolverInput::instance_lower_bounds), which holds against any box. The formulation view (`reach`) also lowers the
//! ceiling and raises the floor to a helper's reach (SolverInput::instance_reach_lower / instance_reach_upper), which
//! holds only for the formulation it was derived from: a reader of `rigid_*` (the `<>` range collapse) never sets it.
struct InstanceBox {
    const SolverInput &input;
    const VarIndexer &indexer;
    bool reach = false;

    //! The floor of `var` as data row `row` reads it, against the box floors `box_lower`.
    double LowerOfRow(idx_t var, idx_t row, const vector<double> &box_lower) const {
        idx_t instance = indexer.InstanceOf(var, row);
        double floor = input.InstanceLower(var, instance, box_lower);
        return reach ? std::max(floor, InstanceReachLower(input.instance_reach_lower, var, instance)) : floor;
    }

    //! The ceiling of `var` as data row `row` reads it, against the box ceilings `box_upper`.
    double UpperOfRow(idx_t var, idx_t row, const vector<double> &box_upper) const {
        if (!reach) {
            return box_upper[var];
        }
        idx_t instance = indexer.InstanceOf(var, row);
        return std::min(box_upper[var], InstanceReachUpper(input.instance_reach_upper, var, instance));
    }
};

//! One summed decide variable of an aggregate LHS, paired with the coefficient the user
//! wrote for it. `has_unit` is false when that coefficient is data-varying
//! (`SUM(keepS * price)`): there is no literal to quote, and the render falls back to the
//! symbolic name in `ConstraintProvenance::weight_labels`. See `folded_terms`.
struct FoldedAggTerm {
    idx_t decide_var_idx = DConstants::INVALID_INDEX;
    double unit = 1.0;
    bool has_unit = false;
};

//! Row → clause provenance carried by every emitted constraint (F2). Lets diagnosis
//! report at the user-clause level instead of at raw matrix rows.
struct ConstraintProvenance {
    //! Stable source comparison used only for display provenance.
    idx_t source_clause_id = DConstants::INVALID_INDEX;
    //! Elastic grouping identity. DConstants::INVALID_INDEX for rigid/source-less rows.
    idx_t repair_group_id = DConstants::INVALID_INDEX;
    //! Atomic DROP identity. All rows and native constructs sharing it are omitted together.
    idx_t removal_group_id = DConstants::INVALID_INDEX;
    //! `by`/`when` group id at emission (or the row id for per-row clauses).
    //! DConstants::INVALID_INDEX when the clause is ungrouped.
    idx_t group_key = DConstants::INVALID_INDEX;
    //! User parameter vs rigid mechanism/structural row (see ConstraintKind).
    ConstraintKind kind = ConstraintKind::USER_PARAMETER;
    //! Elastic-diagnosis shape (I2): does this row share ONE slack with its
    //! (repair_group_id, group_key) siblings, or get its own? Relaxable user-clause
    //! rows must be stamped explicitly by the builder site.
    ElasticShape shape = ElasticShape::UNSET;
    //! True when the row's coefficients were pre-scaled by 1/N_g for an AVG rewrite.
    //! The slack is then already in the user's AVG units (report the raw slack).
    bool avg_scaled = false;
    //! True when the user wrote a strict `<` / `>` (a δ offset was baked into `rhs`
    //! at build time). `typed_k` carries the user's original literal so the reported
    //! suggestion can be re-quoted against it instead of the δ-adjusted rhs.
    bool strict = false;
    double typed_k = 0.0;
    //! What a lowering added to the user's literal to build this row's RHS. The reported
    //! clause and its suggested edit quote `rhs - rhs_mechanism_offset`, so a hard
    //! `MAX(e) >= K` lowered to `e_i - M*y_i >= K - M` still reads as `MAX(e) >= K` and
    //! still loosens by the slack the engine actually spent. 0.0 (the ordinary case)
    //! means the row's RHS is the user's literal already. Distinct from `strict`/`typed_k`,
    //! which re-quote a δ the *binder* baked in; both are resolved by `DisplayRhs`.
    double rhs_mechanism_offset = 0.0;
    //! Flat solver column of the `<>` disjunction binary this row belongs to.
    //! Describes the formulation itself; atomic DROP identity lives independently in
    //! `removal_group_id`, and display identity in `source_clause_id`.
    idx_t indicator_col = DConstants::INVALID_INDEX;
    //! The name of this row's group (SolverModel::key_values); none when the clause's groups
    //! have no key. Lets infeasible diagnosis identify which group an edit belongs to and
    //! populate the finding's `group` column, so grouped aggregates can fold to `SUM(x)`
    //! without colliding in the relation (Facet A).
    GroupName group_name;
    //! The classes this row's group stands for (SolverModel::class_sets), or
    //! INVALID_INDEX: an expanded diagnosis reports the row once per class (D28).
    idx_t class_set = DConstants::INVALID_INDEX;
    //! The data row a per-row model row was emitted at -- what DIAGNOSE's `row` column
    //! reports for a row that stands for it -- or INVALID_INDEX for a group's row.
    idx_t data_row = DConstants::INVALID_INDEX;
    //! A per-row row of a clause that stands for no class
    //! set (no `per` key): each kept row is a class of its own, so the expanded scope
    //! reports it once per row rather than folding the clause's rows into one finding (D28).
    bool per_kept_row = false;
    //! True when the LHS was an aggregate (`SUM`/`AVG`/...). Lets the diagnosis wrap a
    //! single-row / WHEN group's reconstruction in `SUM(...)` even when there is no
    //! per-row fan-out to key on (distinct from SHARED_SCALAR, which a per-row
    //! query-wide bound also carries) (Facet B).
    bool is_aggregate = false;
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
    //! Per-term coefficient the user actually wrote, for an aggregate row whose terms *fold*
    //! onto fewer solver columns than there are contributing rows. A keyed variable
    //! (`DECIDE S.keepS`) maps every joined row of a class onto ONE column, so the builder
    //! accumulates instead of fanning out: `SUM(keepS)` over two rows of one sensor arrives
    //! as a single index with coefficient 2. The fan-out that `FormatSumLhs` keys on is then
    //! structurally absent, and the reconstruction quotes a constant the user never typed
    //! (`SUM(2*keepS)`, or `SUM(keepS * 1)` when the group mixes multiplicities). Recording
    //! the written coefficient lets the diagnosis quote the clause as written.
    //! Populated only on the accumulating build path; empty for row-scoped fan-out rows,
    //! which `FormatSumLhs` already folds correctly.
    vector<FoldedAggTerm> folded_terms;
};

//! A single linear constraint: sum(coefficients[i] * x[indices[i]]) <sense> rhs
struct ModelConstraint {
    vector<int> indices;       //!< Variable indices into the flattened variable array
    vector<double> coefficients; //!< Coefficient for each variable
    char sense;                //!< '<' (<=), '>' (>=), '=' (==)
    double rhs;                //!< Right-hand side value
    ConstraintProvenance provenance; //!< Row → clause origin (F2)
    //! A row with no terms is the constant comparison `0 <sense> rhs`, judged once as the model is built
    //! (ConstantRowFails, or DuckDB's own comparison for a bound on one decision): the builder keeps it only
    //! when it fails, and SolverModel::ProvesInfeasible reads this verdict.
    bool constant_fails = false;
};

//! Can any assignment meet this row's bound? A finite left-hand side never reaches
//! `Ax >= +inf`, `Ax <= -inf`, or `Ax = ±inf`, so a row spelled that way is infeasible
//! on its own — no other row is implicated and no finite loosening closes the gap. The
//! mirrored spellings (`Ax <= +inf`, `Ax >= -inf`) are the vacuous ones: they constrain
//! nothing and are never the reason a solve failed.
//!
//! This is a rule about the MODEL, not about any backend: a row spelled this way is not
//! a hard problem to solve, it is a row with no solution. SolverModel::ProvesInfeasible
//! answers such a model before any backend sees it, so every backend reports the same
//! verdict, and infeasible diagnosis names the clause, which no finite loosening repairs.
inline bool IsUnreachableBound(char sense, double rhs) {
	if (!std::isinf(rhs)) {
		return false;
	}
	return sense == '=' || ((rhs > 0.0) == (sense == '>'));
}

//! A row whose left-hand side reduced to no terms (`SUM(0 * x) <= -1`, `x - x <= -1`) is
//! the constant comparison `0 <sense> rhs`. It fails when `rhs` is out of 0's reach by
//! more than the builder's tolerance; one that holds is a tautology and is dropped. A
//! bound on one decision whose coefficient is 0 on a row is judged by DuckDB instead.
inline bool ConstantRowFails(char sense, double rhs) {
	constexpr double EPS = 1e-9;
	if (sense == '<') {
		return rhs < -EPS;
	}
	if (sense == '>') {
		return rhs > EPS;
	}
	return std::fabs(rhs) > EPS;
}

//! Solver-agnostic optimization model, ready for any backend to consume.
//! Supports linear objectives (LP/MILP) and convex quadratic objectives (QP/MIQP).
struct SolverModel {
    //! Total number of variables across all three blocks (row-scoped + keyed +
    //! global auxiliary). Initialized so a default-constructed model (one Build never
    //! populated) reads as empty rather than as garbage: every `col < num_vars` guard
    //! downstream depends on it.
    idx_t num_vars = 0;

    //! Per-variable configuration (size = num_vars)
    vector<double> col_lower;  //!< Lower bounds
    vector<double> col_upper;  //!< Upper bounds
    vector<bool> is_integer;   //!< True for INTEGER/BOOLEAN vars, false for REAL (continuous)
    vector<bool> is_binary;    //!< True if binary (0/1), subset of integer

    //! Linear objective: minimize/maximize c^T x + obj_constant
    vector<double> obj_coeffs; //!< Coefficient per variable (linear part)
    bool maximize;             //!< True = maximize, false = minimize
    //! The objective's constant term (an LP's objective offset): what the objective as written adds whatever the
    //! decisions are -- a constant beside the reducers (`SUM(x) + 3`), a constant inside one (`SUM(x + 7)`), and the
    //! constant of an expanded square (`POWER(x - 5, 2)`). It moves no optimum, so no backend is given it: a session
    //! adds it to the objective value and bound a solve reads back (SolverSession::Solve), so every caller reads the
    //! objective as the user wrote it. Summed unchecked: past a DOUBLE it is infinite (NaN when infinities of both
    //! signs meet), which only the reported value shows.
    double obj_constant = 0.0;

    //! Quadratic objective: x^T Q x, added to the linear part. NOT the (1/2) x^T Q x
    //! form some solvers take -- see the convention note below, which every backend
    //! adapter must honour rather than assume.
    //!
    //! CONVENTION. A stored value is the plain coefficient of its monomial in the
    //! objective polynomial: q_vals[k] multiplies x[q_rows[k]] * x[q_cols[k]] exactly
    //! once. So `3*x*x` stores 3 and `4*x*y` stores 4, and the objective is the sum of
    //! q_vals[k] * x[q_rows[k]] * x[q_cols[k]] over all k. Held in COO (coordinate)
    //! format -- three parallel arrays, one entry per nonzero -- lower triangle only
    //! (q_rows[k] >= q_cols[k]), since x*y and y*x are one term.
    //!
    //! Backends differ here and MUST convert. Gurobi's GRBaddqpterms takes exactly this
    //! form, so its adapter passes Q through. HiGHS's passHessian instead takes the Q of
    //! (1/2) x^T Q x, whose 1/2 cancels the double-counting of an off-diagonal pair but
    //! has nothing to cancel on the diagonal; its adapter therefore doubles diagonal
    //! entries and leaves off-diagonal entries alone. Getting that backwards silently
    //! optimizes a different objective -- it does not fail loudly.
    //!
    //! Empty when the objective is purely linear (LP/MILP).
    vector<int> q_rows;        //!< Row indices into Q (>= the paired q_cols entry)
    vector<int> q_cols;        //!< Column indices into Q
    vector<double> q_vals;     //!< Coefficient of x[q_rows[k]] * x[q_cols[k]]
    bool has_quadratic_obj = false;
    //! True when the quadratic objective + sense combination is non-convex.
    //! Non-convex when: MAXIMIZE + PSD Q, or MINIMIZE + NSD Q.
    //! Gurobi handles this with NonConvex=2; HiGHS cannot solve it.
    bool nonconvex_quadratic = false;

    //! Constructs the backend takes natively rather than as rows: the result column,
    //! the argument columns, and the provenance every row carries. Non-empty only when
    //! the chosen backend declared the construct in its SolverCapabilities, so an
    //! adapter never sees a kind it did not ask for. Every other backend received the
    //! lowered rows in `constraints` instead.
    struct GeneralConstraint {
        GeneralConstraintKind kind = GeneralConstraintKind::ABS;
        int result_column = -1;
        vector<int> argument_columns;
        ConstraintProvenance provenance;
    };
    vector<GeneralConstraint> general_constraints;

    //! `binary_column == binary_value` implies this row. Carries a full row, unlike a
    //! general constraint, so infeasible diagnosis can still reach it — the removal
    //! dial groups these by provenance exactly as it groups matrix rows.
    struct IndicatorConstraint {
        int binary_column = -1;
        int binary_value = 1;
        vector<int> indices;
        vector<double> coefficients;
        char sense = '<';
        double rhs = 0.0;
        ConstraintProvenance provenance;
    };
    vector<IndicatorConstraint> indicator_constraints;

    //! Constraints (linear)
    vector<ModelConstraint> constraints;

    //! The classes a scoped clause's group stands for, by ConstraintProvenance::class_set
    //! (SolverInput::class_sets, moved here by Build).
    vector<SolverInput::ClassSet> class_sets;
    //! What names the groups a GroupName refers to (SolverInput::key_values,
    //! moved here by Build).
    vector<SolverInput::KeyValues> key_values;
    //! Stable source display registry, indexed by source_clause_id.
    vector<ConstraintSourceInfo> constraint_sources;

    //! Quadratic constraints: sum(linear) + sum(q * x_i * x_j) <sense> rhs
    //! Used for bilinear terms in constraints (QCQP). Gurobi only.
    //! q_coefficients follows the same convention as the objective's q_vals above:
    //! the plain coefficient of x[q_rows[k]] * x[q_cols[k]], lower triangle only.
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

    //! True when the model has no solution and needs no solver to say so: a coefficient-free
    //! row that fails (ConstantRowFails), a row no finite assignment reaches
    //! (IsUnreachableBound), or an inverted column box. Such a model is not hard, it is
    //! empty, and no backend contract says it must accept one (HiGHS errors on load). Every
    //! solve asks this first and answers INFEASIBLE without a backend; the rows stay in the
    //! model with their provenance, so diagnosis can name or relax them.
    bool ProvesInfeasible() const;

    //! Remove the objective: every linear coefficient zero, no constant, no quadratic part. The sense and the
    //! columns stay, so a caller can set a new objective over the same columns or solve for feasibility alone.
    void ClearObjective();

    //! Build a SolverModel from a SolverInput (the shared model-building logic).
    //! Takes a non-const reference because raw global constraints are moved out of `input`
    //! into `model.constraints` to avoid deep copies.
    //! `indexer` must already be constructed for `input` (typically built once in
    //! PhysicalDecide::Finalize() and threaded through).
    static SolverModel Build(SolverInput &input, const VarIndexer &indexer);

    //! What this model, as actually built, demands of a solver. The FACT that the
    //! plan-time prediction (stage 05) is checked against — see SolverModelClass.
    SolverModelClass ModelClass() const;
};

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
    //! Row (row-scoped) or class id (keyed) this column instantiates;
    //! INVALID for global-block columns. Disambiguates repeated names.
    idx_t instance = DConstants::INVALID_INDEX;
};

//! Reverse map: flat solver column index → ColumnProvenance (F6).
//! Pure (no planner/Expression dependency): the
//! caller pre-extracts per-decide-variable labels (user name or aux source
//! expression) and an is-aux flag. Output is sized `indexer.total_vars`; every
//! entry defaults to GLOBAL_AUX (the global block stays unnamed), then the row /
//! keyed blocks are filled by inverting `indexer.Get(var, row)` over all
//! decide variables and rows — mirrors the solution-readback iteration.
//!   var_labels[v]  — name (USER) or source expression (AUX) for decide var v
//!   var_is_aux[v]  — true if decide var v is an auxiliary linearization variable
//!   global_var_labels[g] — clause text for global var g (e.g. an aggregate `<>`
//!                          indicator "(SUM(x) <> K)"), empty for unnamed globals.
//!                          Surfaces a label on the otherwise-unnamed global block
//!                          so diagnostics and model inspection can name an aggregate
//!                          `<>`. Parallel to SolverInput::global_variable_types.
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
//! Both outputs are cleared if row_group_ids is empty (ungrouped fast path).
void BuildGroupCSR(const vector<idx_t> &row_group_ids,
                   idx_t num_groups,
                   vector<idx_t> &offsets,
                   vector<idx_t> &flat_rows);

//! The data rows each row of a grouped aggregate clause sums, kept on `ec`: group g's row sums rows
//! [group_offsets[g], group_offsets[g + 1]) of the returned list. Built from row_group_ids on the first call
//! (BuildGroupCSR), so implied-bound propagation, the aggregate `<>` expansion and the model builder share one list.
//! row_group_ids is final by then: the passes after implied bounds that change a clause's rows (a MIN/MAX rewrite, a
//! per-row `<>` mask) change only clauses implied bounds passes over.
const vector<idx_t> &ClauseRows(const EvaluatedConstraint &ec);

//! The decision terms of every row `ec` emits (EmittedRows), kept on `ec`: each solver column's coefficient summed over
//! the row's data rows (ClauseRows for a grouped aggregate). Built on the first call, so implied-bound propagation, the
//! aggregate `<>` expansion and the model builder read one matrix; the builder, the last reader, takes it. Its rows
//! are final by the first call, as ClauseRows' are. `written` names the clause when a sum grows past a DOUBLE.
const EmittedRows &ClauseEmittedRows(const EvaluatedConstraint &ec, const VarIndexer &indexer, const string &written);

//! Adds `value` into `target`, a number the model merges from several: like coefficients on one solver column, a
//! square's products, the fixed terms moved to the bound side. The one place such a sum grows. Every value it adds was
//! read finite, so a sum that is not finite has grown past a DOUBLE, and the user error names `written`, the clause the
//! sum belongs to (DecideWrittenClause).
inline void AddCoefficient(double &target, double value, const string &written) {
    target += value;
    if (!std::isfinite(target)) {
        throw InvalidInputException(DecideInvalidValueMessage(DecideValueRole::TOO_LARGE, written));
    }
}

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

    //! The clause the sums belong to, for the error of one past a DOUBLE (AddCoefficient). Set by Begin*; the string
    //! outlives the rows the clause emits.
    const string *written = nullptr;

    //! Configure for dense accumulation. Allocates `total_vars` doubles
    //! (zero-initialized) on first use; subsequent calls only clear `touched`.
    void BeginDense(idx_t total_vars, const string &written);

    //! Configure for sorted-pairs accumulation. `expected_size` is a hint for reserve().
    void BeginSparse(idx_t expected_size, const string &written);

    //! Configure for the decision columns of `indexer`, [0, global_block_start): dense while that span is small
    //! enough to keep as scratch (about 8MB), sorted pairs past it.
    void BeginForDecisions(const VarIndexer &indexer, idx_t expected_size, const string &written);

    //! Add a contribution. Branchless on use_dense via the pre-set mode.
    inline void Add(int idx, double coeff) {
        if (use_dense) {
            if (dense[idx] == 0.0) {
                touched.push_back(idx);
            }
            AddCoefficient(dense[idx], coeff, *written);
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
