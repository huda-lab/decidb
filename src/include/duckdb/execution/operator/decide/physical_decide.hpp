#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Data Structures for Expression Term Extraction
//===--------------------------------------------------------------------===//

//! Represents a single term: variable_index * coefficient_expression
//! variable_index can be DConstants::INVALID_INDEX for constant terms.
//! Used in both linear and quadratic (inner) expressions.
struct Term {
    idx_t variable_index;              // Which DECIDE variable (or INVALID_INDEX for constants)
    unique_ptr<Expression> coefficient; // Row-varying expression to evaluate later
    int sign = 1;                       // +1 or -1, applied at coefficient evaluation time
    unique_ptr<Expression> filter;       // Optional aggregate-local WHEN filter
    bool avg_scale = false;              // True when this term came from AVG and needs 1/N scaling
    //! Entity scope this term's reducer is qualified by (`sum(D: ...)`), or
    //! INVALID_INDEX for an unqualified reducer. Indexes entity_scopes /
    //! entity_mappings; coefficient evaluation keeps one row per tuple identity
    //! of that relation and zeroes the duplicates the join introduced.
    idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;

    Term(idx_t var_idx, unique_ptr<Expression> coef, int s = 1)
        : variable_index(var_idx), coefficient(std::move(coef)), sign(s) {}
};

//! Represents a bilinear term in a constraint: coef * var_a * var_b
struct BilinearConstraintTerm {
    idx_t var_a;
    idx_t var_b;
    unique_ptr<Expression> coefficient;  // Data coefficient (or nullptr for 1.0)
    int sign = 1;
    unique_ptr<Expression> filter;        // Optional aggregate-local WHEN filter
    bool avg_scale = false;               // True when this term came from AVG and needs 1/N scaling
    //! Entity scope this term's reducer is qualified by (`sum(D: ...)`), or
    //! INVALID_INDEX for an unqualified reducer. Indexes entity_scopes /
    //! entity_mappings; coefficient evaluation keeps one row per tuple identity
    //! of that relation and zeroes the duplicates the join introduced.
    idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;
};

//! Represents a complete constraint after term extraction
struct DecideConstraint {
    vector<Term> lhs_terms;              // All additive terms from LHS
    unique_ptr<Expression> rhs_expr;     // RHS expression (may contain aggregates)
    ExpressionType comparison_type;      // COMPARE_LESSTHANOREQUALTO or GREATERTHANOREQUALTO
    bool lhs_is_aggregate = false;       // True if original LHS was an aggregate (e.g., SUM(...))
    bool was_minmax_easy = false;        // True if optimizer stripped an easy-direction MIN/MAX (MINMAX_EASY_REWRITE_TAG). Lets Site 1 enforce empty-WHEN rejection on user-written MIN/MAX even though the LHS is now per-row.
    idx_t minmax_indicator_idx = DConstants::INVALID_INDEX;  // Indicator var idx for hard MIN/MAX
    string minmax_agg_type;              // "min" or "max" (empty if not minmax)
    idx_t ne_indicator_idx = DConstants::INVALID_INDEX;      // Indicator var idx for not-equal
    idx_t abs_y_idx = DConstants::INVALID_INDEX;  // sign indicator for ABS Big-M (MAXIMIZE only)
    bool abs_is_pos_bound = false;                 // true=C1 (aux >= inner), false=C2 (aux >= -inner)
    unique_ptr<Expression> when_condition;           // DecidB: optional WHEN condition (nullptr = unconditional)
    vector<unique_ptr<Expression>> per_columns;     // DecidB: optional PER grouping columns (empty = no grouping)
    ConstraintKind kind = ConstraintKind::USER_PARAMETER;


    // Bilinear terms in constraint (non-Boolean pairs left by optimizer)
    vector<BilinearConstraintTerm> bilinear_terms;
    bool has_bilinear = false;

    // Quadratic groups in constraint: each POWER(expr, 2) or (expr)*(expr) self-product
    // becomes a separate group. The model builder computes an outer-product Q for each
    // group independently, then accumulates all into the same QuadraticConstraint.
    // This is necessary because POWER(x-t,2) + POWER(y-s,2) ≠ POWER(x-t+y-s, 2).
    struct QuadraticGroup {
        vector<Term> inner_terms;  // Inner linear expression of POWER(inner, 2)
        double sign = 1.0;         // +1, -1, or scalar (from negation/scaling)
        unique_ptr<Expression> filter; // Optional aggregate-local WHEN filter
        bool avg_scale = false;    // True when this group came from AVG and needs 1/N scaling
        //! Entity scope this term's reducer is qualified by (`sum(D: ...)`), or
        //! INVALID_INDEX for an unqualified reducer. Indexes entity_scopes /
        //! entity_mappings; coefficient evaluation keeps one row per tuple identity
        //! of that relation and zeroes the duplicates the join introduced.
        idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;

        QuadraticGroup() = default;
    };
    vector<QuadraticGroup> quadratic_groups;
    bool has_quadratic = false;

    DecideConstraint() = default;
};

//! Represents the objective function after term extraction.
//! Supports both linear objectives (terms only) and quadratic objectives
//! of the form MINIMIZE SUM((linear_expr)^2) + linear_terms.
struct Objective {
    vector<Term> terms;                    // Linear objective terms
    unique_ptr<Expression> when_condition; // DecidB: optional WHEN condition (nullptr = unconditional)
    vector<unique_ptr<Expression>> per_columns; // DecidB: optional PER grouping columns (empty = no grouping)

    //! Quadratic objective: the inner linear expression of each SUM(POWER(expr, 2)) term.
    //! When non-empty, the objective includes a quadratic component: sign * SUM((inner_expr)^2).
    //! sign = +1.0 for SUM(POWER(expr, 2)), sign = -1.0 for SUM(-POWER(expr, 2)).
    vector<Term> squared_terms;
    bool has_quadratic = false;
    double quadratic_sign = 1.0;

    //! Bilinear objective terms: x_a * x_b with data coefficient.
    //! These are products of two different DECIDE variables where neither is Boolean
    //! (Boolean cases are linearized by the optimizer into McCormick auxiliary variables).
    struct BilinearTerm {
        idx_t var_a;                       // First DECIDE variable index
        idx_t var_b;                       // Second DECIDE variable index
        unique_ptr<Expression> coefficient; // Data coefficient expression (or nullptr for 1.0)
        int sign = 1;                       // +1 or -1
        unique_ptr<Expression> filter;       // Optional aggregate-local WHEN filter
        bool avg_scale = false;              // True when this term came from AVG and needs 1/N scaling
        //! Entity scope this term's reducer is qualified by (`sum(D: ...)`), or
        //! INVALID_INDEX for an unqualified reducer. Indexes entity_scopes /
        //! entity_mappings; coefficient evaluation keeps one row per tuple identity
        //! of that relation and zeroes the duplicates the join introduced.
        idx_t qualifier_scope_idx = DConstants::INVALID_INDEX;
    };
    vector<BilinearTerm> bilinear_terms;
    bool has_bilinear = false;

    Objective() = default;
};

//===--------------------------------------------------------------------===//
// PhysicalDecide Operator
//===--------------------------------------------------------------------===//

//! PhysicalDecide represents a blocking operator that solves an ILP over its
//! entire input before producing output.
class PhysicalDecide : public PhysicalOperator {
public:
    static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::DECIDE;

public:
    PhysicalDecide(vector<LogicalType> types, idx_t estimated_cardinality, 
                    unique_ptr<PhysicalOperator> child, idx_t decide_index, 
                    vector<unique_ptr<Expression>> decide_variables,
                    unique_ptr<Expression> decide_constraints, DecideSense decide_sense,
                    unique_ptr<Expression> decide_objective);

    // The table index for the new columns
    idx_t decide_index;

    // The variables to be decided (e.g., x, y)
    vector<unique_ptr<Expression>> decide_variables;

    // O(1) lookup: ColumnBinding → decide_variables index (built in constructor)
    column_binding_map_t<idx_t> decide_variable_map;

    // The bound constraints expression
    unique_ptr<Expression> decide_constraints;

    // The optimization sense (MINIMIZE or MAXIMIZE)
    DecideSense decide_sense;

    // The bound objective function expression
    unique_ptr<Expression> decide_objective;

    // Number of auxiliary variables (e.g. from ABS linearization) at the end of decide_variables
    idx_t num_auxiliary_vars = 0;

    // Per-variable flag: true if the variable was declared IS BOOLEAN (copied from
    // LogicalDecide). A BOOLEAN variable is lowered to an INTEGER with a 0/1 domain
    // (synthesized `x >= 0` / `x <= 1` constraints), so its runtime type is INTEGER;
    // this flag is the only surviving signal that the 0/1 box is the variable's
    // intrinsic domain, not a user-editable bound. Indexed by decide_variables position.
    vector<bool> is_boolean_var;

    // Indices of auxiliary indicator variables for not-equal (<>) constraints
    vector<idx_t> ne_indicator_indices;

    // Links from MIN/MAX indicator variables: (agg_name "min"/"max", indicator_idx)
    vector<pair<string, idx_t>> minmax_indicator_links;

    // Links from bilinear McCormick auxiliary variables: w = b * x
    // (aux_idx, bool_var_idx, other_var_idx) — for execution-time Big-M constraint generation
    vector<LogicalDecide::BilinearLink> bilinear_links;
    vector<LogicalDecide::AbsMaximizeLink> abs_maximize_links;

    // F6: auxiliary variable index -> user's original source expression string
    // (ABS(...) / MIN/MAX(...) / product / <>), for naming an escaping aux column
    // in the unbounded diagnosis. Sparse; only auxiliary variables appear.
    vector<pair<idx_t, string>> aux_var_expressions;

    // Composed MIN/MAX constraints (additive LHS with MIN/MAX terms mixed in).
    // Each is emitted as a block of RawConstraints in global_constraints at sink finalize.
    vector<LogicalDecide::ComposedMinMaxConstraint> composed_minmax_constraints;

    // Composed MIN/MAX objective: additive sum of SUM/AVG/MIN/MAX terms.
    // Empty when the objective is not composed.
    vector<LogicalDecide::ComposedMinMaxTerm> composed_minmax_objective_terms;

    // --- MIN/MAX objective metadata (set by DecideOptimizer::RewriteMinMaxObjective) ---

    // Flat (non-PER) objective: original aggregate type before rewrite to SUM
    ObjectiveAggregateType flat_objective_agg = ObjectiveAggregateType::NONE;
    // Pre-computed: true if easy formulation (MAXIMIZE+MIN or MINIMIZE+MAX)
    bool flat_objective_is_easy = false;

    // PER nested objective: OUTER(INNER(expr)) PER col
    ObjectiveAggregateType per_inner_agg = ObjectiveAggregateType::NONE;
    ObjectiveAggregateType per_outer_agg = ObjectiveAggregateType::NONE;
    // Pre-computed easy/hard at each level (only meaningful when agg is MIN_AGG or MAX_AGG)
    bool per_inner_is_easy = false;
    bool per_outer_is_easy = false;
    // True if inner aggregate was originally AVG (coefficients need 1/n_g scaling)
    bool per_inner_was_avg = false;

    // --- Table-scoped variable metadata ---

    //! Entity scope info for each source table with table-scoped variables
    vector<EntityScopeInfo> entity_scopes;

    //! Per-variable scope assignment (row / entity / query-wide scalar);
    //! entity_scope_idx indexes entity_scopes
    vector<DecideVarScopeInfo> variable_scopes;

    //! Source column name per physical child-output column (positionally aligned
    //! with gstate.data columns), resolved post-pruning in plan_decide.cpp. Used by
    //! the unbounded diagnosis to label escaping categorical groups (affected_rows).
    vector<string> input_column_names;

public:
    // Source interface
    unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
    SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;

    // Sink interface
    unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
    unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
    SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
    SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
    SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                              OperatorSinkFinalizeInput &input) const override;

    bool IsSource() const override {
        return true;
    }

    bool IsSink() const override {
        return true;
    }
    bool ParallelSink() const override {
        return true;
    }

public:
    string GetName() const override;
    InsertionOrderPreservingMap<string> ParamsToString() const override;

public:
    //! Helper methods for expression analysis (used by DecideGlobalSinkState)

    //! Find a DECIDE variable in an expression tree
    //! Returns variable index or DConstants::INVALID_INDEX if not found
    idx_t FindDecideVariable(const Expression &expr) const;

    //! Check if expression contains a specific DECIDE variable
    bool ContainsVariable(const Expression &expr, idx_t var_idx) const;

    //! Returns true iff `expr` is linear (degree ≤ 1) in the DECIDE variables.
    //! Used as a precondition when routing subtrees into quadratic / bilinear
    //! slots: the "inner" of POWER(inner, 2) and each side of a bilinear x*y
    //! must be linear, otherwise the total degree would exceed 2 and the Q
    //! matrix would be silently wrong. Sums/subtractions/unary-negation of
    //! linear subexpressions are linear; a multiplication is linear only if
    //! at most one factor contains a decide variable. Any other function
    //! (POWER, SIN, ...) is linear only when it contains no decide variable.
    bool IsLinearInDecideVars(const Expression &expr) const;

    //! Extract coefficient expression, removing the specified variable
    //! For example: from "x * 5 * l_tax", removes x and returns "5 * l_tax"
    //! `context` is needed to re-bind a rebuilt product: stripping the variable
    //! also strips the casts above it, so the surviving children no longer match
    //! the original `*`'s signature.
    unique_ptr<Expression> ExtractCoefficientWithoutVariable(ClientContext &context, const Expression &expr,
                                                             idx_t var_idx) const;

    //! Main visitor: extract all terms from a SUM argument
    //! Handles + operators (recursively), * operators (extract var and coef), constants
    void ExtractTerms(ClientContext &context, const Expression &expr, vector<Term> &out_terms) const;

    //! Forward declaration — see physical_decide.cpp for the full definition.
    //! The struct holds a non-owning `const Expression *` into the caller's
    //! expression tree plus a scalar sign multiplier; defining it in the .cpp
    //! keeps that lifetime contract internal to the execution operator.
    struct QuadraticPattern;

    //! Detect quadratic patterns in a DECIDE objective/constraint expression:
    //!   POWER(expr, 2), POW(expr, 2), expr ** 2,
    //!   (expr) * (expr) with identical children,
    //!   -(quadratic_pattern),
    //!   K * quadratic_pattern (K constant, either side).
    //! Cast wrappers are unwrapped transparently. The `inner linear expression`
    //! returned is the argument inside POWER / one side of the self-product.
    QuadraticPattern DetectQuadraticPattern(const Expression &expr) const;
};

} // namespace duckdb
