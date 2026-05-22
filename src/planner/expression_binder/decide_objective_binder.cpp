#include "duckdb/planner/expression_binder/decide_objective_binder.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

namespace duckdb {

DecideObjectiveBinder::DecideObjectiveBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables)
    : DecideBinder(binder, context, variables){
}

BindResult DecideObjectiveBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	if (binding_when_condition) {
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	if (depth > 0) {
		return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
	}
	auto &expr = *expr_ptr;
    // DebugPrintParsed("BindObjective.input", expr);
    string error_msg;
	switch (expr.GetExpressionClass()) {
    case ExpressionClass::COLUMN_REF:
    case ExpressionClass::CONSTANT: {
        if (!is_top_expression) {
	        return ExpressionBinder::BindExpression(expr_ptr, depth);
	    }
	    break;
	}
	case ExpressionClass::FUNCTION: {
	    auto &func = expr.Cast<FunctionExpression>();
	    // PackDB: Handle PER on objective — preserve PER columns for per-group objectives
	    if (func.is_operator && IsPerConstraintTag(func.function_name)) {
	        D_ASSERT(func.children.size() >= 2);

	        // Validate each PER column
	        for (idx_t i = 1; i < func.children.size(); i++) {
	            if (ExpressionContainsDecideVariable(*func.children[i], variables)) {
	                return BindResult(BinderException::Unsupported(*expr_ptr,
	                    "PER column in MAXIMIZE/MINIMIZE cannot be a DECIDE variable. "
	                    "PER must group by a table column."));
	            }
	            if (func.children[i]->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
	                return BindResult(BinderException::Unsupported(*expr_ptr,
	                    "PER columns in MAXIMIZE/MINIMIZE must be simple column references "
	                    "(e.g., PER empID or PER (empID, dept)). Expressions are not supported."));
	            }
	        }

	        // Bind the inner objective (child[0]) through normal objective binding
	        is_top_expression = true;
	        ErrorData obj_error;
	        BindChild(func.children[0], depth, obj_error);
	        if (obj_error.HasError()) {
	            return BindResult(std::move(obj_error));
	        }

	        // Bind each PER column using base ExpressionBinder
	        is_top_expression = false;
	        binding_when_condition = true;
	        for (idx_t i = 1; i < func.children.size(); i++) {
	            ErrorData col_error;
	            try {
	                BindChild(func.children[i], depth, col_error);
	            } catch (...) {
	                binding_when_condition = false;
	                throw;
	            }
	            if (col_error.HasError()) {
	                binding_when_condition = false;
	                return BindResult(std::move(col_error));
	            }
	        }
	        binding_when_condition = false;

	        // Construct tagged BoundConjunctionExpression:
	        // child[0] = bound objective (possibly WHEN-wrapped)
	        // children[1..N] = bound PER columns
	        auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	        result->children.push_back(std::move(BoundExpression::GetExpression(*func.children[0])));
	        for (idx_t i = 1; i < func.children.size(); i++) {
	            result->children.push_back(std::move(BoundExpression::GetExpression(*func.children[i])));
	        }
	        result->alias = func.function_name;  // preserves PER_CONSTRAINT_TAG
	        return BindResult(std::move(result));
	    }
	    // PackDB: Handle WHEN on objective: MAXIMIZE SUM(...) WHEN condition.
	    // Nested WHEN is the aggregate-local form and binds through
	    // DecideBinder::BindFunction.
	    if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG) {
	        if (!is_top_expression) {
	            return BindFunction(expr_ptr, depth);
	        }
	        D_ASSERT(func.children.size() == 2);
	        // Validate: WHEN condition cannot reference DECIDE variables
	        if (ExpressionContainsDecideVariable(*func.children[1], variables)) {
	            return BindResult(BinderException::Unsupported(*expr_ptr,
	                "WHEN conditions in MAXIMIZE/MINIMIZE cannot reference DECIDE variables. "
	                "The WHEN condition must only reference table columns."));
	        }
	        if (ContainsWhenOperator(*func.children[0])) {
	            return BindResult(BinderException::Unsupported(*expr_ptr,
	                "Cannot combine expression-level WHEN with aggregate-local WHEN in the same DECIDE objective. "
	                "Move the shared condition into each aggregate-local WHEN, or keep a single expression-level WHEN."));
	        }
	        // Bind the objective (child[0]) through normal objective binding
	        is_top_expression = true;
	        ErrorData obj_error;
	        BindChild(func.children[0], depth, obj_error);
	        if (obj_error.HasError()) {
	            return BindResult(std::move(obj_error));
	        }
	        // Bind the condition (child[1]) using base ExpressionBinder
	        is_top_expression = false;
	        binding_when_condition = true;
	        ErrorData cond_error;
	        try {
	            BindChild(func.children[1], depth, cond_error);
	        } catch (...) {
	            binding_when_condition = false;
	            throw;
	        }
	        binding_when_condition = false;
	        if (cond_error.HasError()) {
	            return BindResult(std::move(cond_error));
	        }
	        // Construct tagged BoundConjunctionExpression
	        auto &bound_objective = BoundExpression::GetExpression(*func.children[0]);
	        auto &bound_condition = BoundExpression::GetExpression(*func.children[1]);
	        auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	        result->children.push_back(std::move(bound_objective));
	        result->children.push_back(BoundCastExpression::AddCastToType(context, std::move(bound_condition), LogicalType::BOOLEAN));
	        result->alias = WHEN_CONSTRAINT_TAG;
	        return BindResult(std::move(result));
	    }
	    // DebugPrintParsed("BindObjective.input", expr);
	        if (is_top_expression && GetExpressionType(expr, error_msg) == DecideExpression::INVALID) {
	            return BindResult(BinderException::Unsupported(expr, error_msg));
	        }
	        is_top_expression = false;
	        auto result = BindFunction(expr_ptr, depth);
            if (result.HasError()) {
                return result;
            }
            return result;
	}
    case ExpressionClass::SUBQUERY:
        return DecideBinder::BindExpression(expr_ptr, depth, root_expression);
	default:
        break;
	}
    return BindResult(BinderException::Unsupported(expr, StringUtil::Format("[MAXIMIZE|MINIMIZE] clause does not support '%s'(ExpressionClass::%s)", expr.ToString(), EnumUtil::ToString(expr.GetExpressionClass()))));
}

DecideExpression DecideObjectiveBinder::GetExpressionType(ParsedExpression &expr, string& error_msg){
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		auto fname = StringUtil::Lower(func.function_name);
		if (fname == "sum" || fname == "avg" || fname == "min" || fname == "max") {
            if (!ValidateSumArgument(*func.children.front(), variables, error_msg, /*allow_quadratic=*/true)) {
                error_msg += ", found '" + expr.ToString() + "'";
                return DecideExpression::INVALID;
            }
            return DecideExpression::SUM;
		} else {
            // Non-aggregate outer function. Only additive/scalar composition of
            // aggregates (e.g. `SUM(x) + SUM(y)`, `-SUM(x)`, `c * SUM(x)`,
            // `SUM(x) / K`) is allowed; wrapping an aggregate in a non-additive
            // function (e.g. `POWER(AVG(x), 2)`, `SQRT(SUM(x))`, `LOG(...)`) is
            // not a linearly-composable objective. Supported quadratic shape is
            // SUM(POWER(_, 2)), not POWER(AGG(_), _).
            bool is_additive_or_scalar = (fname == "+" || fname == "-" || fname == "*");
            // Division is scalar only when the divisor does not contain a
            // decide aggregate (otherwise the result is genuinely non-linear).
            if (fname == "/" && func.children.size() == 2 &&
                !ContainsDecideAggregate(*func.children[1])) {
                is_additive_or_scalar = true;
            }
            if (is_additive_or_scalar && ContainsDecideAggregate(expr)) {
                return DecideExpression::SUM;
            }
            if (ContainsDecideAggregate(expr)) {
                error_msg = StringUtil::Format(
                    "[MAXIMIZE|MINIMIZE] does not support wrapping an aggregate in '%s'. "
                    "The aggregate must be the outermost function. "
                    "For quadratic objectives use SUM(POWER(expr, 2)), not %s(AGG(expr), ...).",
                    func.function_name, StringUtil::Upper(func.function_name));
                return DecideExpression::INVALID;
            }
            error_msg = StringUtil::Format("[MAXIMIZE|MINIMIZE] clause does not support function '%s', only SUM, AVG, MIN, or MAX is allowed.", func.function_name);
            return DecideExpression::INVALID;
        }
    }
    default: {
        error_msg = StringUtil::Format("The objective of the [MAXIMIZE|MINIMIZE] clause must be a SUM expression over a DECIDE variable (e.g., SUM(x * a) / SUM(x)). Found '%s' instead.", expr.ToString());
    	return DecideExpression::INVALID;
    }
    }
}

} // namespace duckdb
