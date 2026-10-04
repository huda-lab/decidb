#include "duckdb/planner/expression_binder/decide/decide_objective_binder.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

namespace duckdb {

DecideObjectiveBinder::DecideObjectiveBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                                             const case_insensitive_set_t &scalar_variables,
                                             optional_ptr<DecideQualifierContext> qualifier_context)
    : DecideBinder(binder, context, variables, scalar_variables, qualifier_context) {
}

BindResult DecideObjectiveBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto location = expr_ptr->GetQueryLocation();
	return PreserveQueryLocation(location, BindExpressionInternal(expr_ptr, depth, root_expression));
}

BindResult DecideObjectiveBinder::BindExpressionInternal(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                         bool root_expression) {
	if (binding_when_condition) {
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	if (depth > 0) {
		return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
	}
	auto &expr = *expr_ptr;
    string error_msg;
	switch (expr.GetExpressionClass()) {
    case ExpressionClass::COLUMN_REF:
    case ExpressionClass::CONSTANT: {
        // A bare query-wide decision is a complete objective on its own: it has a
        // single solver column, so there is nothing for a reducer to collapse.
        if (!is_top_expression || IsScalarDecideVariable(expr)) {
	        return ExpressionBinder::BindExpression(expr_ptr, depth);
	    }
	    break;
	}
	case ExpressionClass::FUNCTION: {
	    auto &func = expr.Cast<FunctionExpression>();
	        if (is_top_expression && GetExpressionType(expr, error_msg) == DecideExpression::INVALID) {
	            return BindResult(BinderException::Unsupported(expr, error_msg));
	        }
	        is_top_expression = false;
	        return BindFunction(expr_ptr, depth);
	}
    case ExpressionClass::SUBQUERY:
        return DecideBinder::BindExpression(expr_ptr, depth, root_expression);
	case ExpressionClass::CAST:
		// Explicit decision-bearing casts were rejected from the parsed objective
		// before rewrites. A surviving nested cast is therefore a data computation
		// (or parser-internal representation noise) and binds with normal DuckDB rules.
		if (!is_top_expression) {
			return ExpressionBinder::BindExpression(expr_ptr, depth);
		}
		break;
	case ExpressionClass::OPERATOR:
		// The aggregate classifier already validated this reducer body before
		// BindAggregate descends into it. A nested operator here is therefore a
		// decision-free coefficient such as COALESCE(weight, 0); bind it with
		// DuckDB's ordinary operator binder, matching the constraint binder.
		if (!is_top_expression) {
			return ExpressionBinder::BindExpression(expr_ptr, depth);
		}
		break;
	case ExpressionClass::CASE:
		// Shares the constraint binder's wording; see DecideCaseUnsupportedMessage.
		return BindResult(BinderException::Unsupported(expr, DecideCaseUnsupportedMessage()));
	default:
        break;
	}
    if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF && IsVariableExpression(expr, variables)) {
        // A row- or key-scoped decision on its own: the same rule the generation
        // check states for a term beside a reducer, in the same words.
        return BindResult(BinderException::Unsupported(
            expr, StringUtil::Format("MAXIMIZE/MINIMIZE objective: decision '%s' varies across rows, but an objective "
                                     "is generated once (PER ()); reduce it, e.g. SUM(%s), or declare it PER ().",
                                     expr.ToString(), expr.ToString())));
    }
    if (expr.GetExpressionClass() == ExpressionClass::CONSTANT) {
        return BindResult(BinderException::Unsupported(
            expr, StringUtil::Format("MAXIMIZE/MINIMIZE objective: '%s' reads no decision, so there is nothing to "
                                     "optimize; write SATISFY for any feasible assignment.",
                                     expr.ToString())));
    }
    if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
        return BindResult(BinderException::Unsupported(
            expr, StringUtil::Format("MAXIMIZE/MINIMIZE objective: '%s' is a data column, not a decision; an "
                                     "objective reads decisions, e.g. SUM(x * %s).",
                                     expr.ToString(), expr.ToString())));
    }
    return BindResult(BinderException::Unsupported(
        expr, StringUtil::Format("MAXIMIZE/MINIMIZE objective: '%s' is not an objective; write a reducer over "
                                 "decisions (SUM, AVG, MIN, MAX) or a PER () decision.",
                                 expr.ToString())));
}

DecideExpression DecideObjectiveBinder::GetExpressionType(ParsedExpression &expr_ptr, string& error_msg){
    // A relation qualifier changes which tuples a reducer sums over, not what shape it
    // is, so classification looks straight through it.
    auto &expr = const_cast<ParsedExpression &>(UnwrapQualifiedReducer(expr_ptr));
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		auto fname = StringUtil::Lower(func.function_name);
		// A frame anywhere in the objective, beside a reducer as much as alone
		// (`SUM(x) - 3 * AT(FIRST: x) OVER (t)`): it navigates from an instance's
		// position and an objective has none. Checked before the additive shape is
		// accepted, which would otherwise read the frame as each row's own term.
		if (ParsedExpressionContainsFrame(expr)) {
			error_msg = "A frame expression (AT / FROM .. TO .. OVER) navigates from an instance's position, "
			            "and an objective has none: the position of a frame is not determined there. "
			            "Use it in a constraint.";
			return DecideExpression::INVALID;
		}
		DecideExpression reducer_result;
		if (ClassifyReducerCall(func, reducer_result, error_msg)) {
			return reducer_result;
		}
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
        // Linear arithmetic over query-wide decisions alone (`2 * c + 1`, `c + d`,
        // `-c`): every term is one value for the query, so the objective is
        // generated once without a reducer, exactly like a bare PER () decision.
        if (is_additive_or_scalar && ExpressionContainsDecideVariable(expr, variables) &&
            IsRowInvariantExpression(expr)) {
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
    case ExpressionClass::COLUMN_REF: {
        // A query-wide decision is a single solver column, so it is already a
        // scalar objective — it needs no reducer to collapse it.
        if (IsScalarDecideVariable(expr)) {
            return DecideExpression::VARIABLE;
        }
        if (IsVariableExpression(expr, variables)) {
            error_msg = StringUtil::Format("MAXIMIZE/MINIMIZE objective: decision '%s' varies across rows, but an objective is "
                                           "generated once (PER ()); reduce it, e.g. SUM(%s), or declare it PER ().",
                                           expr.ToString(), expr.ToString());
            return DecideExpression::INVALID;
        }
        error_msg = StringUtil::Format("The objective of the [MAXIMIZE|MINIMIZE] clause must be a SUM expression over a DECIDE variable (e.g., SUM(x * a) / SUM(x)). Found '%s' instead.", expr.ToString());
        return DecideExpression::INVALID;
    }
    default: {
        error_msg = StringUtil::Format("The objective of the [MAXIMIZE|MINIMIZE] clause must be a SUM expression over a DECIDE variable (e.g., SUM(x * a) / SUM(x)). Found '%s' instead.", expr.ToString());
    	return DecideExpression::INVALID;
    }
    }
}

} // namespace duckdb
