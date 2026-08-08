#include "duckdb/planner/expression_binder/decide_constraints_binder.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

DecideConstraintsBinder::DecideConstraintsBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                                                 const case_insensitive_set_t &scalar_variables,
                                                 optional_ptr<DecideQualifierContext> qualifier_context)
    : DecideBinder(binder, context, variables, scalar_variables, qualifier_context) {
}

static bool IsAllowedConstraintRHS(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables);

static bool IsSupportedComparison(ExpressionType type) {
    switch (type) {
    case ExpressionType::COMPARE_EQUAL:
    case ExpressionType::COMPARE_LESSTHAN:
    case ExpressionType::COMPARE_GREATERTHAN:
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
    case ExpressionType::COMPARE_NOTEQUAL:
        return true;
    default:
        return false;
    }
}

static bool IsDecideConstraintLHS(DecideExpression type) {
    return type == DecideExpression::VARIABLE || type == DecideExpression::SUM;
}

static bool IsAllowedOperatorChildren(const vector<unique_ptr<ParsedExpression>> &children,
                                      const case_insensitive_map_t<idx_t> &variables) {
    for (auto &child : children) {
        if (!IsAllowedConstraintRHS(*child, variables)) {
            return false;
        }
    }
    return true;
}

static bool IsAllowedConstraintRHS(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
    switch (expr.GetExpressionClass()) {
        case ExpressionClass::CONSTANT:
            return true;
        case ExpressionClass::FUNCTION: {
            auto &func = expr.Cast<FunctionExpression>();
            if (func.is_operator) {
                if (StringUtil::Lower(func.function_name) == "-") {
                    return false;
                }
                if (!IsAllowedOperatorChildren(func.children, variables)) {
                    return false;
                }
                if (func.filter && !IsAllowedConstraintRHS(*func.filter, variables)) {
                    return false;
                }
                return true;
            }
            auto fn = StringUtil::Lower(func.function_name);
            if (fn == "sum" || fn == "avg" || fn == "min" || fn == "max") {
                if (func.children.empty()) {
                    return false;
                }
                if (func.children.size() != 1) {
                    return false;
                }
                if (func.filter && !IsAllowedConstraintRHS(*func.filter, variables)) {
                    return false;
                }
                if (ExpressionContainsDecideVariable(*func.children[0], variables)) {
                    return false;
                }
                return true;
            }
            for (auto &child : func.children) {
                if (!IsAllowedConstraintRHS(*child, variables)) {
                    return false;
                }
            }
            if (func.filter && !IsAllowedConstraintRHS(*func.filter, variables)) {
                return false;
            }
            return true;
        }
        case ExpressionClass::OPERATOR: {
            auto &op = expr.Cast<OperatorExpression>();
            return IsAllowedOperatorChildren(op.children, variables);
        }
        case ExpressionClass::CAST: {
            auto &cast = expr.Cast<CastExpression>();
            return IsAllowedConstraintRHS(*cast.child, variables);
        }
        case ExpressionClass::SUBQUERY: {
            auto &subquery = expr.Cast<SubqueryExpression>();
            if (subquery.subquery_type != SubqueryType::SCALAR) {
                return false;
            }
            return true;
        }
        default:
            return false;
    }
}

BindResult DecideConstraintsBinder::BindComparison(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &comp = expr.Cast<ComparisonExpression>();
    string error_msg;
    auto left_type = GetExpressionType(*comp.left, error_msg);

    // DuckDB's parser preserves the written comparison shape. DECIDE's constraint
    // binder expects the DECIDE-bearing expression on the LHS, so normalize
    // equivalent reversed forms like `5 >= x` to `x <= 5` before validation.
    if (left_type == DecideExpression::INVALID && IsSupportedComparison(comp.type) &&
        !ExpressionContainsDecideVariable(*comp.left, variables) && !ContainsDecideAggregate(*comp.left)) {
        string right_error;
        auto right_type = GetExpressionType(*comp.right, right_error);
        if (IsDecideConstraintLHS(right_type)) {
            comp.type = FlipComparisonExpression(comp.type);
            std::swap(comp.left, comp.right);
            left_type = right_type;
            error_msg = right_error;
        }
    }

    auto SimplifyZeroAddition = [&](auto &&self, unique_ptr<ParsedExpression> &node) -> void {
        if (!node) {
            return;
        }
        switch (node->GetExpressionClass()) {
        case ExpressionClass::FUNCTION: {
            auto &func = node->Cast<FunctionExpression>();
            for (auto &child : func.children) {
                self(self, child);
            }
            if (func.is_operator && func.function_name == "+" && func.children.size() == 2) {
                auto IsZeroConstant = [](const ParsedExpression &expr) {
                    if (expr.GetExpressionClass() != ExpressionClass::CONSTANT) {
                        return false;
                    }
                    auto &c = expr.Cast<const ConstantExpression>();
                    if (!c.value.type().IsNumeric()) {
                        return false;
                    }
                    return fabs(c.value.GetValue<double>()) < 1e-12;
                };
                auto &lhs = func.children[0];
                auto &rhs = func.children[1];
                if (IsZeroConstant(*lhs)) {
                    node = std::move(rhs);
                    self(self, node);
                    return;
                }
                if (IsZeroConstant(*rhs)) {
                    node = std::move(lhs);
                    self(self, node);
                    return;
                }
            }
            break;
        }
        case ExpressionClass::CAST: {
            auto &cast = node->Cast<CastExpression>();
            self(self, cast.child);
            break;
        }
        default:
            break;
        }
    };
    SimplifyZeroAddition(SimplifyZeroAddition, comp.right);
    // DebugPrintParsed("BindComparison.right (simplified)", *comp.right);
    auto &right = *comp.right;
    switch (comp.type) {
    // Type declarations (x IS INTEGER/BOOLEAN) are now handled in the DECIDE clause,
    // not in SUCH THAT. The COMPARE_EQUAL case here is for actual equality constraints (e.g., SUM(x) = 10)
    case ExpressionType::COMPARE_EQUAL:
    case ExpressionType::COMPARE_LESSTHAN:
    case ExpressionType::COMPARE_GREATERTHAN:
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
    case ExpressionType::COMPARE_NOTEQUAL: {
        switch (left_type) {
            case DecideExpression::VARIABLE: {
                // Multi-variable per-row constraints allowed (e.g., ABS linearization: d >= x - c)
                break;
            }
            case DecideExpression::SUM: {
                if (!ContainsDecideAggregate(*comp.left)) {
                    return BindResult(BinderException::Unsupported(expr, "DECIDE constraint left-hand side must contain SUM(...), AVG(...), MIN(...), or MAX(...)"));
                }
                if (!IsAllowedConstraintRHS(right, variables) || ExpressionContainsDecideVariable(right, variables)) {
                    return BindResult(BinderException::Unsupported(expr, StringUtil::Format("SUM cannot be compared to an expression that is not a scalar or aggregate without DECIDE variables, found '%s'", expr.ToString())));
                }
                break;
            }
            case DecideExpression::INVALID:
                return BindResult(BinderException::Unsupported(expr, error_msg));
            default:
                return BindResult(BinderException::Unsupported(expr, StringUtil::Format("Unsupported DecideExpression '%s'(%s)", comp.left->ToString(), EnumUtil::ToString(left_type))));
        }
        is_top_expression = false;
        return ExpressionBinder::BindExpression(expr_ptr, depth);
    }
    default:
        return BindResult(BinderException::Unsupported(expr, StringUtil::Format("SUCH THAT constraint clause does not support '%s'(ExpressionType::%s)", expr.ToString(), EnumUtil::ToString(comp.type))));
    }
}

BindResult DecideConstraintsBinder::BindOperator(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &op = expr.Cast<OperatorExpression>();
    switch (op.type) {
    case ExpressionType::COMPARE_IN:{
        // Variable-level IN (x IN (1,2,3)) is rewritten before binding.
        // If we reach here, it's an aggregate IN or unrewritten edge case.
        return BindResult(BinderException::Unsupported(expr, StringUtil::Format(
            "SUCH THAT does not support IN on '%s'. Only simple DECIDE variables are allowed as the IN target",
            op.children.front()->ToString())));
    }
    default:
        return BindResult(BinderException::Unsupported(expr, StringUtil::Format("SUCH THAT constraint clause does not support '%s'(ExpressionType::%s)", expr.ToString(), EnumUtil::ToString(op.type))));
    }
}

BindResult DecideConstraintsBinder::BindBetween(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &between = expr.Cast<BetweenExpression>();

    // Transform BETWEEN into (input >= lower) AND (input <= upper)
    auto input_copy = between.input->Copy();
    
    auto lower_comp = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO, std::move(between.input), std::move(between.lower));
    auto upper_comp = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(input_copy), std::move(between.upper));

    auto conjunction = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower_comp), std::move(upper_comp));
    
    // Bind the new conjunction
    // We need to replace the current expression pointer with the new conjunction
    expr_ptr = std::move(conjunction);
    return BindConjunction(expr_ptr, depth);
}

BindResult DecideConstraintsBinder::BindConjunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &conj = expr.Cast<ConjunctionExpression>();
    // first try to bind the children of the case expression
    ErrorData error;
    for (idx_t i = 0; i < conj.children.size(); i++) {
        is_top_expression = true;
        BindChild(conj.children[i], depth, error);
    }
    if (error.HasError()) {
        return BindResult(std::move(error));
    }
    // the children have been successfully resolved
    // cast the input types to boolean (if necessary)
    // and construct the bound conjunction expression
    auto result = make_uniq<BoundConjunctionExpression>(conj.GetExpressionType());
    for (auto &child_expr : conj.children) {
        auto &child = BoundExpression::GetExpression(*child_expr);
        result->children.push_back(BoundCastExpression::AddCastToType(context, std::move(child), LogicalType::BOOLEAN));
    }
    // now create the bound conjunction expression
    return BindResult(std::move(result));
}

BindResult DecideConstraintsBinder::BindWhenConstraint(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &func = expr_ptr->Cast<FunctionExpression>();
    D_ASSERT(func.children.size() == 2);

    // Validate: WHEN condition (child[1]) cannot reference DECIDE variables
    if (ExpressionContainsDecideVariable(*func.children[1], variables)) {
        return BindResult(BinderException::Unsupported(*expr_ptr,
            "WHEN conditions cannot reference DECIDE variables. "
            "The WHEN condition must only reference table columns."));
    }
    if (ContainsWhenOperator(*func.children[0])) {
        return BindResult(BinderException::Unsupported(*expr_ptr,
            "Cannot combine expression-level WHEN with aggregate-local WHEN in the same DECIDE constraint. "
            "Move the shared condition into each aggregate-local WHEN, or keep a single expression-level WHEN."));
    }

    // Bind the constraint (child[0]) through normal DECIDE constraint dispatch
    is_top_expression = true;
    ErrorData constraint_error;
    BindChild(func.children[0], depth, constraint_error);
    if (constraint_error.HasError()) {
        return BindResult(std::move(constraint_error));
    }

    // Bind the condition (child[1]) using the base ExpressionBinder (not DECIDE-specific)
    // RAII guard ensures flag is reset even if BindChild throws
    is_top_expression = false;
    binding_when_condition = true;
    ErrorData condition_error;
    try {
        BindChild(func.children[1], depth, condition_error);
    } catch (...) {
        binding_when_condition = false;
        throw;
    }
    binding_when_condition = false;
    if (condition_error.HasError()) {
        return BindResult(std::move(condition_error));
    }

    // Construct bound result: tagged BoundConjunctionExpression
    // child[0] = bound constraint, child[1] = bound condition (cast to BOOLEAN)
    auto &bound_constraint = BoundExpression::GetExpression(*func.children[0]);
    auto &bound_condition = BoundExpression::GetExpression(*func.children[1]);

    auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
    result->children.push_back(std::move(bound_constraint));
    result->children.push_back(BoundCastExpression::AddCastToType(context, std::move(bound_condition), LogicalType::BOOLEAN));
    result->alias = WHEN_CONSTRAINT_TAG;
    return BindResult(std::move(result));
}

//! Check if a parsed constraint expression is aggregate (SUM-based).
//! Unwraps optional WHEN wrapper to inspect the inner comparison.
//! Handles both direct aggregates (SUM(...) <= K) and aggregate-local WHEN
//! (SUM(...) WHEN cond <= K) on the comparison LHS.
static bool IsAggregateConstraint(const ParsedExpression &expr) {
    const ParsedExpression *inner = &expr;
    // Unwrap expression-level WHEN wrapper if present
    if (inner->GetExpressionClass() == ExpressionClass::FUNCTION) {
        auto &func = inner->Cast<FunctionExpression>();
        if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG && !func.children.empty()) {
            inner = func.children[0].get();
        }
    }
    // Check if the comparison's LHS contains a DECIDE aggregate
    // (either directly or wrapped in aggregate-local WHEN)
    if (inner->GetExpressionClass() == ExpressionClass::COMPARISON) {
        auto &comp = inner->Cast<ComparisonExpression>();
        if (ContainsDecideAggregate(*comp.left)) {
            return true;
        }
    }
    return false;
}

BindResult DecideConstraintsBinder::BindPerConstraint(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &func = expr_ptr->Cast<FunctionExpression>();
    D_ASSERT(func.children.size() >= 2);

    auto &constraint_child = func.children[0];  // constraint (possibly WHEN-wrapped)

    // Validate each PER column (children[1..N])
    for (idx_t i = 1; i < func.children.size(); i++) {
        auto &column_child = func.children[i];

        // Validate: PER column must not reference a DECIDE variable
        if (ExpressionContainsDecideVariable(*column_child, variables)) {
            return BindResult(BinderException::Unsupported(*expr_ptr,
                "PER column cannot be a DECIDE variable. "
                "PER must group by a table column."));
        }

        // Validate: PER column must be a simple column reference
        if (column_child->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
            return BindResult(BinderException::Unsupported(*expr_ptr,
                "PER columns must be simple column references "
                "(e.g., PER empID or PER (empID, dept)). Expressions are not supported."));
        }
    }

    // Validate: constraint must be aggregate (SUM-based)
    if (!IsAggregateConstraint(*constraint_child)) {
        return BindResult(BinderException::Unsupported(*expr_ptr,
            "PER can only be applied to aggregate (SUM) constraints. "
            "Per-row constraints (e.g., 'x <= 5 PER col') are not supported "
            "because each row already has its own constraint."));
    }

    // Bind the constraint child through normal dispatch (handles WHEN recursively)
    is_top_expression = true;
    ErrorData constraint_error;
    BindChild(func.children[0], depth, constraint_error);
    if (constraint_error.HasError()) {
        return BindResult(std::move(constraint_error));
    }

    // Bind each PER column using the base ExpressionBinder
    // (reuse binding_when_condition flag to bypass DECIDE-specific dispatch)
    is_top_expression = false;
    binding_when_condition = true;
    for (idx_t i = 1; i < func.children.size(); i++) {
        ErrorData column_error;
        try {
            BindChild(func.children[i], depth, column_error);
        } catch (...) {
            binding_when_condition = false;
            throw;
        }
        if (column_error.HasError()) {
            binding_when_condition = false;
            return BindResult(std::move(column_error));
        }
    }
    binding_when_condition = false;

    // Construct tagged bound result:
    // child[0] = bound constraint (possibly WHEN-wrapped)
    // children[1..N] = bound PER columns (BoundColumnRefExpression)
    auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
    result->children.push_back(std::move(BoundExpression::GetExpression(*func.children[0])));
    for (idx_t i = 1; i < func.children.size(); i++) {
        result->children.push_back(std::move(BoundExpression::GetExpression(*func.children[i])));
    }
    result->alias = func.function_name;  // preserves PER_CONSTRAINT_TAG
    return BindResult(std::move(result));
}

BindResult DecideConstraintsBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	if (binding_when_condition) {
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	if (depth > 0) {
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	auto &expr = *expr_ptr;
	switch (expr.GetExpressionClass()) {
    case ExpressionClass::COLUMN_REF: {
        if (!is_top_expression) {
            return ExpressionBinder::BindExpression(expr_ptr, depth);
        }
        // A bare column at the top level of SUCH THAT is never a valid
        // constraint. Top-level commas are rejected by the parser, but keep
        // the PER hint here for malformed single-column inputs and older
        // prepared parse trees that surface a bare grouping key.
        return BindResult(BinderException::Unsupported(
            expr, StringUtil::Format(
                      "'%s' is not a valid SUCH THAT constraint on its own. "
                      "To group a constraint by multiple columns, parenthesize them: PER (col1, col2).",
                      expr.ToString())));
    }
    case ExpressionClass::CONSTANT: {
        if (!is_top_expression) {
            return ExpressionBinder::BindExpression(expr_ptr, depth);
        }
        break;
    }
    case ExpressionClass::FUNCTION: {
        auto &func = expr.Cast<FunctionExpression>();
        // DecidB: PER constraint wrapper (outermost, wraps optional WHEN)
        if (func.is_operator && IsPerConstraintTag(func.function_name)) {
            return BindPerConstraint(expr_ptr, depth);
        }
        // DecidB: top-level WHEN wraps a whole constraint. Nested WHEN is the
        // aggregate-local form and binds through DecideBinder::BindFunction.
        if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG) {
            if (!is_top_expression) {
                return BindFunction(expr_ptr, depth);
            }
            return BindWhenConstraint(expr_ptr, depth);
        }
        if (!is_top_expression) {
            return BindFunction(expr_ptr, depth);
        }
        break;
	}
    case ExpressionClass::COMPARISON:
        return BindComparison(expr_ptr, depth);
    case ExpressionClass::OPERATOR: {
        if (!is_top_expression) {
            return ExpressionBinder::BindExpression(expr_ptr, depth);
        }
        return BindOperator(expr_ptr, depth);
    }
	case ExpressionClass::BETWEEN:
        return BindBetween(expr_ptr, depth);
    case ExpressionClass::CONJUNCTION:
        return BindConjunction(expr_ptr, depth);
    case ExpressionClass::SUBQUERY: {
        return DecideBinder::BindExpression(expr_ptr, depth, root_expression);
    }
	default:
        break;
	}
    return BindResult(BinderException::Unsupported(expr, StringUtil::Format("SUCH THAT clause does not support '%s'(ExpressionClass::%s)", expr.ToString(), EnumUtil::ToString(expr.GetExpressionClass()))));
}

DecideExpression DecideConstraintsBinder::GetExpressionType(ParsedExpression &expr_ptr, string& error_msg){
    // A relation qualifier changes which tuples a reducer sums over, not what shape it
    // is, so classification looks straight through it.
    auto &expr = const_cast<ParsedExpression &>(UnwrapQualifiedReducer(expr_ptr));
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::COLUMN_REF: {
        if (!IsVariableExpression(expr, variables)) {
            error_msg = StringUtil::Format("SUCH THAT clause: Column '%s' must be one of the DECIDE variables", expr.ToString());
            return DecideExpression::INVALID;
        }
        return DecideExpression::VARIABLE;
    }
    case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		auto fname = StringUtil::Lower(func.function_name);
		if (fname == "sum" || fname == "avg" || fname == "min" || fname == "max") {
            auto scalar_name = FindScalarDecideVariable(*func.children.front());
            if (!scalar_name.empty()) {
                error_msg = StringUtil::Format(
                    "'%s' is a query-wide decision, so %s(%s) has nothing to aggregate over; "
                    "use %s on its own",
                    scalar_name, StringUtil::Upper(fname), scalar_name, scalar_name);
                return DecideExpression::INVALID;
            }
            if (!ValidateSumArgument(*func.children.front(), variables, error_msg, /*allow_quadratic=*/true, /*allow_bilinear=*/true)) {
                error_msg += ", found '" + expr.ToString() + "'";
                return DecideExpression::INVALID;
            }
            return DecideExpression::SUM;
		} else if (ContainsDecideAggregate(expr)) {
            return DecideExpression::SUM;
		} else if (ExpressionContainsDecideVariable(expr, variables)) {
            // Operator/function expressions containing DECIDE variables
            // are treated as per-row multi-variable constraints
            // (e.g., z_1 + z_2 + z_3 from IN rewrite, or d - x from ABS linearization)
            return DecideExpression::VARIABLE;
        } else {
            error_msg = StringUtil::Format("SUCH THAT clause does not support left-hand side function '%s', only SUM, AVG, MIN, or MAX is allowed.", func.function_name);
            return DecideExpression::INVALID;
        }
    }
    default: {
        error_msg = StringUtil::Format("The left-hand side of a SUCH THAT constraint must be a DECIDE variable or a SUM expression over a DECIDE variable (e.g., SUM(x * a) / SUM(x)). Found '%s' instead.", expr.ToString());
    	return DecideExpression::INVALID;
    }
    }
}

} // namespace duckdb
