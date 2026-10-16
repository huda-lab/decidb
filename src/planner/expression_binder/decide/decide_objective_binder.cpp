#include "duckdb/planner/expression_binder/decide/decide_objective_binder.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"

namespace duckdb {

DecideObjectiveBinder::DecideObjectiveBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                                             const case_insensitive_set_t &scalar_variables,
                                             optional_ptr<DecideQualifierContext> qualifier_context)
    : DecideBinder(binder, context, variables, scalar_variables, qualifier_context) {
}

BindResult DecideObjectiveBinder::BindScopedObjective(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	// `[WHEN c] [PER ()]: objective`. An objective is produced once, so `PER ()` is its
	// default and adds nothing; the parser refuses any other key.
	auto *body = &expr_ptr;
	if (IsDecideMarker(**body, SCOPE_PER_TAG)) {
		body = &(*body)->Cast<FunctionExpression>().children[0];
	}
	optional_ptr<FunctionExpression> when;
	if (IsDecideMarker(**body, SCOPE_WHEN_TAG)) {
		when = &(*body)->Cast<FunctionExpression>();
		body = &when->children[0];
	}
	ErrorData body_error;
	is_top_expression = true;
	binding_scoped_objective = true;
	try {
		BindChild(*body, depth, body_error);
	} catch (...) {
		binding_scoped_objective = false;
		throw;
	}
	binding_scoped_objective = false;
	if (body_error.HasError()) {
		return BindResult(std::move(body_error));
	}
	auto bound = std::move(BoundExpression::GetExpression(**body));
	if (!when) {
		return BindResult(std::move(bound));
	}
	ErrorData condition_error;
	is_top_expression = false;
	binding_when_condition = true;
	try {
		BindChild(when->children[1], depth, condition_error);
	} catch (...) {
		binding_when_condition = false;
		throw;
	}
	binding_when_condition = false;
	if (condition_error.HasError()) {
		return BindResult(std::move(condition_error));
	}
	auto wrap = [&](unique_ptr<Expression> objective) -> unique_ptr<Expression> {
		auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		wrapper->children.push_back(std::move(objective));
		wrapper->children.push_back(BoundCastExpression::AddCastToType(
		    context, std::move(BoundExpression::GetExpression(*when->children[1])), LogicalType::BOOLEAN));
		wrapper->alias = WHEN_CONSTRAINT_TAG;
		AddDecideTag(wrapper->alias, SCOPED_CLAUSE_TAG);
		return std::move(wrapper);
	};
	// The two-level form's PER wrapper stays outermost, as the nested objective's rewrite
	// reads it: PER{WHEN{objective, c}, k}.
	if (bound->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION && IsPerConstraintTag(bound->alias)) {
		auto &per = bound->Cast<BoundConjunctionExpression>();
		per.children[0] = wrap(std::move(per.children[0]));
		return BindResult(std::move(bound));
	}
	return BindResult(wrap(std::move(bound)));
}

BindResult DecideObjectiveBinder::BindTwoLevelObjective(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	// `outer(PER k: inner(e) BY (k)) [BY ()]`.
	auto *outer_slot = &expr_ptr;
	if (IsDecideMarker(**outer_slot, REDUCER_BY_TAG)) {
		outer_slot = &(*outer_slot)->Cast<FunctionExpression>().children[0];
	}
	auto &outer = (*outer_slot)->Cast<FunctionExpression>();
	auto &per = outer.children[0]->Cast<FunctionExpression>();
	if (IsDecideMarker(*per.children[0], SCOPE_WHEN_TAG)) {
		return BindResult(BinderException::Unsupported(
		    outer, StringUtil::Format("DECIDE: when inside the outer reducer of %s(per %s: ...) is not supported "
		                              "yet; write it in front of the objective",
		                              StringUtil::Lower(outer.function_name), DecideKeyToString(per))));
	}
	auto &inner_by = per.children[0]->Cast<FunctionExpression>();
	auto &inner = inner_by.children[0]->Cast<FunctionExpression>();
	auto form = StringUtil::Format("%s(per %s: %s(...) by (%s))", StringUtil::Lower(outer.function_name),
	                               DecideKeyToString(per), StringUtil::Lower(inner.function_name),
	                               DecideKeyToString(inner_by));
	if (!inner.children.empty() && IsDecideMarker(*inner.children[0], SCOPE_WHEN_TAG)) {
		return BindResult(BinderException::Unsupported(
		    inner, StringUtil::Format("DECIDE: when inside the inner reducer of %s is not supported yet; write it "
		                              "in front of the objective",
		                              form)));
	}
	if (!inner.children.empty() && IsDecideMarker(*inner.children[0], SCOPE_PER_TAG)) {
		return BindResult(BinderException::Unsupported(
		    inner, StringUtil::Format("DECIDE: per inside the inner reducer of %s is not supported yet", form)));
	}
	for (idx_t i = 1; i < inner_by.children.size(); i++) {
		if (inner_by.children[i]->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			return BindResult(BinderException::Unsupported(
			    inner_by, StringUtil::Format("DECIDE: in %s the by key must be the per key; any other is not "
			                                 "supported yet",
			                                 form)));
		}
	}
	case_insensitive_set_t decision_names;
	for (auto &variable : variables) {
		decision_names.insert(variable.first);
	}
	auto key = BindKeyColumns(binder.bind_context, per, decision_names);
	auto by_key = BindKeyColumns(binder.bind_context, inner_by, decision_names);
	bool same_key = key.size() == by_key.size();
	for (idx_t i = 0; same_key && i < key.size(); i++) {
		same_key = key[i]->Cast<BoundColumnRefExpression>().binding ==
		           by_key[i]->Cast<BoundColumnRefExpression>().binding;
	}
	if (!same_key) {
		return BindResult(BinderException::Unsupported(
		    inner_by, StringUtil::Format("DECIDE: in %s the by key must be the per key; any other is not supported "
		                                 "yet",
		                                 form)));
	}
	// The nested objective the old spelling binds to: `outer(inner(e))` under a PER
	// wrapper whose columns are the key.
	outer.children[0] = std::move(inner_by.children[0]);
	auto result = BindExpression(*outer_slot, depth);
	if (result.HasError()) {
		return result;
	}
	auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	wrapper->children.push_back(std::move(result.expression));
	for (auto &column : key) {
		wrapper->children.push_back(std::move(column));
	}
	wrapper->alias = PER_CONSTRAINT_TAG;
	AddDecideTag(wrapper->alias, SCOPED_CLAUSE_TAG);
	return BindResult(std::move(wrapper));
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
        // single solver column, so there is nothing for a reducer to collapse. Under a
        // scope written in front, any decision is: one instance is a scope check.
        if (!is_top_expression || IsScalarDecideVariable(expr) ||
            (binding_scoped_objective && IsVariableExpression(expr, variables))) {
	        return ExpressionBinder::BindExpression(expr_ptr, depth);
	    }
	    break;
	}
	case ExpressionClass::FUNCTION: {
	    auto &func = expr.Cast<FunctionExpression>();
	    if (is_top_expression && func.is_operator &&
	        (func.function_name == SCOPE_PER_TAG || func.function_name == SCOPE_WHEN_TAG)) {
	        return BindScopedObjective(expr_ptr, depth);
	    }
	    if (is_top_expression && IsTwoLevelObjective(expr)) {
	        return BindTwoLevelObjective(expr_ptr, depth);
	    }
	    // DecidB: Handle PER on objective — preserve PER columns for per-group objectives
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

	        return BindPerWrapper(func, depth);
	    }
	    // DecidB: Handle WHEN on objective: MAXIMIZE SUM(...) WHEN condition.
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
    return BindResult(BinderException::Unsupported(expr, StringUtil::Format("[MAXIMIZE|MINIMIZE] clause does not support '%s'(ExpressionClass::%s)", expr.ToString(), EnumUtil::ToString(expr.GetExpressionClass()))));
}

DecideExpression DecideObjectiveBinder::GetExpressionType(ParsedExpression &expr_ptr, string& error_msg){
    // A relation qualifier changes which tuples a reducer sums over, not what shape it
    // is, so classification looks straight through it.
    auto &expr = const_cast<ParsedExpression &>(UnwrapReducerMarkers(expr_ptr));
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		auto fname = StringUtil::Lower(func.function_name);
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
        if (is_additive_or_scalar &&
            (ContainsDecideAggregate(expr) ||
             (binding_scoped_objective && ExpressionContainsDecideVariable(expr, variables)))) {
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
