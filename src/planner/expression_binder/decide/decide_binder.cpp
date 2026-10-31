#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/decide_frame_expression.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/table_binding.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include <unordered_set>
#include <algorithm>
#include <cmath>

#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"

namespace duckdb {

bool IsVariableExpression(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	const auto &colref = expr.Cast<const ColumnRefExpression>();
	// A decision is referenced by its bare name; the declarations binder refuses `D.x`.
	return !colref.IsQualified() && variables.count(colref.GetColumnName()) > 0;
}

//! ParsedExpressionIterator::EnumerateChildren, plus the body (SELECT list, WHERE,
//! FROM, ...) of a subquery, which EnumerateChildren does not enter. For the scope markers,
//! which the clause's keywords lex inside a subquery too; a name in a subquery's body is the
//! subquery's own, so nothing that looks for decisions enters one.
static void EnumerateChildrenAndSubqueries(const ParsedExpression &expr,
                                           const std::function<void(const ParsedExpression &child)> &callback) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery_expr = expr.Cast<const SubqueryExpression>();
		if (subquery_expr.subquery && subquery_expr.subquery->node) {
			// const_cast: EnumerateQueryNodeChildren requires non-const but we only read
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    const_cast<QueryNode &>(*subquery_expr.subquery->node),
			    [&](unique_ptr<ParsedExpression> &child) { callback(*child); }, [&](TableRef &ref) {});
		}
	}
	ParsedExpressionIterator::EnumerateChildren(expr, callback);
}

static idx_t CountDecideVariableOccurrencesInternal(const ParsedExpression &expr,
                                                    const case_insensitive_map_t<idx_t> &variables) {
	idx_t count = 0;
	if (IsVariableExpression(expr, variables)) {
		count++;
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		count += CountDecideVariableOccurrencesInternal(child, variables);
	});
	return count;
}

bool ExpressionContainsDecideVariable(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	return CountDecideVariableOccurrencesInternal(expr, variables) > 0;
}

static bool IsPowerFunction(const FunctionExpression &func);

void ValidateDecideNoExplicitDecisionCasts(const ParsedExpression &expr,
                                           const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		auto &cast = expr.Cast<const CastExpression>();
		// Transformer::TransformTypeCast stamps CAST, TRY_CAST and :: with their
		// source location. CastExpression nodes synthesized by the parser itself
		// (for example for a boolean test) have no location and are not user casts.
		if (cast.GetQueryLocation().IsValid() && ExpressionContainsDecideVariable(*cast.child, variables)) {
			throw BinderException(
			    cast,
			    "DECIDE does not allow casts over decision expressions: '%s'. "
			    "Decision values are modeled directly as DOUBLE. Remove the cast; "
			    "casts over data-only expressions are allowed.",
			    cast.ToString());
		}
	}

	// Not into a subquery's body: a decision cannot be read there at all (DecideBinder).
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		ValidateDecideNoExplicitDecisionCasts(child, variables);
	});
}

void ValidateDecideReferences(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::POSITIONAL_REFERENCE) {
		throw BinderException(expr,
		                      "DECIDE: %s names a column by its position, which a DECIDE clause does not support; "
		                      "write the column's name",
		                      expr.ToString());
	}
	if (expr.GetExpressionClass() == ExpressionClass::LAMBDA) {
		// `->` is also the JSON extract operator, whose left side is no parameter list.
		string not_a_lambda;
		auto parameters = const_cast<LambdaExpression &>(expr.Cast<LambdaExpression>())
		                      .ExtractColumnRefExpressions(not_a_lambda);
		for (auto &parameter : not_a_lambda.empty() ? parameters : vector<reference<ParsedExpression>>()) {
			auto &name = parameter.get().Cast<ColumnRefExpression>().GetColumnName();
			if (variables.count(name)) {
				throw BinderException(expr, "DECIDE: the lambda parameter %s has the name of a decision; rename it",
				                      name);
			}
		}
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { ValidateDecideReferences(child, variables); });
}

//===--------------------------------------------------------------------===//
// Where when, per and by may stand
//===--------------------------------------------------------------------===//

//! Refuses every `when`, `per` and `by` inside `expr`, subquery bodies included, as not bound
//! there: a `when` condition, a key, a `by` key and an ordinary SQL subquery written in
//! the clause (which is lexed with the clause's keywords) take data only.
static void ValidateNoScopeSpelling(const ParsedExpression &expr) {
	static const struct {
		const char *tag;
		const char *spelling;
	} scopes[] = {{SCOPE_WHEN_TAG, "WHEN condition:"},
	              {SCOPE_PER_TAG, "PER key:"},
	              {SCOPE_PER_ROW_TAG, "PER ROW:"},
	              {REDUCER_BY_TAG, "BY (key)"}};
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		for (auto &scope : scopes) {
			if (func.is_operator && func.function_name == scope.tag) {
				bool global = func.function_name == SCOPE_PER_TAG && func.children.size() == 1;
				throw BinderException(expr, "DECIDE: '%s' is not supported yet", global ? "PER ():" : scope.spelling);
			}
		}
		if (IsDecideMarker(expr, QUALIFIED_REDUCER_TAG)) {
			throw BinderException(expr, "DECIDE: the colon form %s is not supported inside a subquery yet",
			                      expr.ToString());
		}
	}
	EnumerateChildrenAndSubqueries(expr, [&](const ParsedExpression &child) { ValidateNoScopeSpelling(child); });
}

//! The first decision `expr` names, as written, or empty: `when`, `per` and `by` take data.
//! The bare name, before binding, so the refusal comes ahead of the rules about what a
//! decision may be under; any other spelling is refused as it binds (DecideBinder).
static string FindDecisionName(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	if (IsVariableExpression(expr, variables)) {
		return expr.ToString();
	}
	string found;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (found.empty()) {
			found = FindDecisionName(child, variables);
		}
	});
	return found;
}

//! The first node of class `expression_class` in `expr` outside its subqueries, or null.
static optional_ptr<const ParsedExpression> FindExpressionOfClass(const ParsedExpression &expr,
                                                                  ExpressionClass expression_class) {
	if (expr.GetExpressionClass() == expression_class) {
		return &expr;
	}
	optional_ptr<const ParsedExpression> found;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (!found) {
			found = FindExpressionOfClass(child, expression_class);
		}
	});
	return found;
}

//! A frame selector reads rows by a timeline the scope itself chooses, so none stands in
//! a `when`, a key or an order key (D74).
static void ValidateNoFrame(const ParsedExpression &expr) {
	auto frame = FindExpressionOfClass(expr, ExpressionClass::DECIDE_FRAME);
	if (frame) {
		throw BinderException(*frame,
		                      "DECIDE: a frame selector is not allowed in when, per, by, within or an order key");
	}
}

const char *DecideDecisionInDataFormat(DecideDataPosition position) {
	D_ASSERT(position != DecideDataPosition::NONE);
	return position == DecideDataPosition::FRAME_KEY
	           ? "DECIDE: %s is a decision; a frame's order key and within may only use data"
	           : "DECIDE: %s is a decision; when, per and by may only use data";
}

//! A `when` condition, a key element or a `by` key: data only, no `when`/`per`/`by`, and
//! no window function or frame selector, which read rows the scope has not chosen yet.
static void ValidateScopeData(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	auto decision = FindDecisionName(expr, variables);
	if (!decision.empty()) {
		throw BinderException(expr, DecideDecisionInDataFormat(DecideDataPosition::SCOPE), decision);
	}
	auto window = FindExpressionOfClass(expr, ExpressionClass::WINDOW);
	if (window) {
		throw BinderException(*window, "DECIDE: a window function such as '%s' is not supported in when, per or by",
		                      window->ToString());
	}
	ValidateNoFrame(expr);
	ValidateNoScopeSpelling(expr);
}

//! A power that is no first power: `power(e, k)`, `pow`, `e ** k`, `e ^ k` with `k` other
//! than the constant 1.
static bool IsNonlinearPower(const FunctionExpression &func) {
	auto name = StringUtil::Lower(func.function_name);
	bool power = (func.is_operator && (name == "**" || name == "^")) ||
	             (!func.is_operator && (name == "power" || name == "pow"));
	if (!power || func.children.size() != 2) {
		return false;
	}
	auto &exponent = *func.children[1];
	if (exponent.GetExpressionClass() == ExpressionClass::CONSTANT) {
		auto &value = exponent.Cast<ConstantExpression>().value;
		return value.IsNull() || !value.type().IsNumeric() || value.GetValue<double>() != 1.0;
	}
	return true;
}

//! Whether `expr` squares or multiplies decisions: a product with a decision in two of
//! its factors, or a power of a decision other than the first.
static bool MultipliesDecisions(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<FunctionExpression>();
		if (IsNonlinearPower(func) && ExpressionContainsDecideVariable(*func.children[0], variables)) {
			return true;
		}
		if (func.is_operator && func.function_name == "*") {
			idx_t factors = 0;
			for (auto &child : func.children) {
				factors += ExpressionContainsDecideVariable(*child, variables) ? 1 : 0;
			}
			if (factors > 1) {
				return true;
			}
		}
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { found = found || MultipliesDecisions(child, variables); });
	return found;
}

//! How a message names a frame: `at(...)`, `sum(from ...)`.
static string FrameHead(const DecideFrameExpression &frame) {
	return frame.IsRange() ? frame.reducer + "(from ...)" : "at(...)";
}

//! A frame's order key or `within` key: data only, with no frame or window inside.
static void ValidateFrameKey(const ParsedExpression &key, const case_insensitive_map_t<idx_t> &variables) {
	auto decision = FindDecisionName(key, variables);
	if (!decision.empty()) {
		throw BinderException(key, DecideDecisionInDataFormat(DecideDataPosition::FRAME_KEY), decision);
	}
	ValidateNoFrame(key);
	auto window = FindExpressionOfClass(key, ExpressionClass::WINDOW);
	if (window) {
		throw BinderException(*window, "DECIDE: a window function such as '%s' is not supported in a frame's order key",
		                      window->ToString());
	}
	std::function<void(const ParsedExpression &)> refuse_reducer = [&](const ParsedExpression &node) {
		if (TakesReducerBy(UnwrapReducerMarkers(node))) {
			throw BinderException(node, "DECIDE: a reducer such as '%s' is not allowed in a frame's order key",
			                      node.ToString());
		}
		ParsedExpressionIterator::EnumerateChildren(node, refuse_reducer);
	};
	refuse_reducer(key);
	ValidateNoScopeSpelling(key);
}

//! A frame's payload and fallback are read at one row, so neither holds a reducer or
//! another frame (D74).
static void ValidateFramePayload(const ParsedExpression &expr, const DecideFrameExpression &frame) {
	if (expr.GetExpressionClass() == ExpressionClass::DECIDE_FRAME) {
		throw BinderException(expr, "DECIDE: a frame selector inside %s is not supported", FrameHead(frame));
	}
	if (TakesReducerBy(UnwrapReducerMarkers(expr))) {
		throw BinderException(expr, "DECIDE: a reducer inside %s is not supported; reduce outside the frame",
		                      FrameHead(frame));
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { ValidateFramePayload(child, frame); });
}

//! The first node of `expr` that holds a decision and reads it other than linearly: a call
//! other than `+`, `-`, `*` and `/` over one (`abs(x - 3)`), or a decision divisor. A data
//! subtree, whatever it computes, is a coefficient. Null when `expr` is linear.
static optional_ptr<const ParsedExpression> FindNonlinearDecisionUse(const ParsedExpression &expr,
                                                                    const case_insensitive_map_t<idx_t> &variables) {
	if (!ExpressionContainsDecideVariable(expr, variables) ||
	    expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		return nullptr;
	}
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return &expr;
	}
	auto &func = expr.Cast<FunctionExpression>();
	auto name = func.function_name;
	if (!func.is_operator || (name != "+" && name != "-" && name != "*" && name != "/")) {
		return &expr;
	}
	if (name == "/" && func.children.size() == 2 && ExpressionContainsDecideVariable(*func.children[1], variables)) {
		return &expr;
	}
	for (auto &child : func.children) {
		auto found = FindNonlinearDecisionUse(*child, variables);
		if (found) {
			return found;
		}
	}
	return nullptr;
}

static void ValidateFrame(const DecideFrameExpression &frame, const case_insensitive_map_t<idx_t> &variables) {
	ValidateFrameKey(*frame.order, variables);
	for (auto &key : frame.within) {
		ValidateFrameKey(*key, variables);
	}
	for (auto *value : {frame.payload.get(), frame.fallback.get()}) {
		if (value) {
			ValidateFramePayload(*value, frame);
			ValidateNoScopeSpelling(*value);
			// A frame's term is linear: its payload is read at another row, and a square
			// there is a product across rows once the frame is written out (D74).
			if (MultipliesDecisions(*value, variables)) {
				throw BinderException(*value,
				                      "DECIDE: '%s' squares or multiplies decisions inside %s, which is not supported "
				                      "yet",
				                      value->ToString(), FrameHead(frame));
			}
			// So is anything else over a decision a rewrite would lower into rows of its own:
			// those rows stand at the anchor, while the frame reads its payload elsewhere.
			auto nonlinear = FindNonlinearDecisionUse(*value, variables);
			if (nonlinear) {
				throw BinderException(*nonlinear,
				                      "DECIDE: '%s' inside %s is not linear in the decisions, which is not supported "
				                      "yet; write it outside the frame",
				                      nonlinear->ToString(), FrameHead(frame));
			}
		}
	}
	// A frame reads a decision through its payload: the layers below see a frame's payload,
	// not its fallback, so one whose fallback alone reads a decision is not formulated yet.
	if (frame.fallback && ExpressionContainsDecideVariable(*frame.fallback, variables) &&
	    !ExpressionContainsDecideVariable(*frame.payload, variables)) {
		throw BinderException(*frame.fallback,
		                      "DECIDE: in %s, else %s reads a decision while the payload reads none, which is not "
		                      "supported yet",
		                      frame.ToString(), frame.fallback->ToString());
	}
}

//! A product, quotient or square that multiplies a frame term by a decision reads a
//! decision at one row and a value at another (D74): refused, data factors aside.
static void ValidateFrameProduct(const FunctionExpression &func, const case_insensitive_map_t<idx_t> &variables) {
	auto name = StringUtil::Lower(func.function_name);
	bool product = func.is_operator && (name == "*" || name == "/");
	bool square = IsNonlinearPower(func);
	if (!product && !square) {
		return;
	}
	for (idx_t i = 0; i < func.children.size(); i++) {
		if (!FindExpressionOfClass(*func.children[i], ExpressionClass::DECIDE_FRAME)) {
			continue;
		}
		bool times_decision = square && i == 0 && ExpressionContainsDecideVariable(*func.children[i], variables);
		for (idx_t j = 0; j < func.children.size(); j++) {
			times_decision =
			    times_decision || (product && j != i && ExpressionContainsDecideVariable(*func.children[j], variables));
		}
		if (times_decision) {
			throw BinderException(func,
			                      "DECIDE: '%s' multiplies a decision by a value from another row, which is not "
			                      "supported yet",
			                      func.ToString());
		}
	}
}

//! `[when c] [per K]:` off a clause or a reducer's argument: the key elements and the
//! condition are validated as data, and the body is returned.
static const ParsedExpression &PeelScopeSpelling(const ParsedExpression &expr,
                                                 const case_insensitive_map_t<idx_t> &variables) {
	const ParsedExpression *body = &expr;
	if (IsDecideMarker(*body, SCOPE_PER_ROW_TAG)) {
		body = body->Cast<const FunctionExpression>().children[0].get();
	} else if (IsDecideMarker(*body, SCOPE_PER_TAG)) {
		auto &per = body->Cast<const FunctionExpression>();
		for (idx_t i = 1; i < per.children.size(); i++) {
			ValidateScopeData(*per.children[i], variables);
		}
		body = per.children[0].get();
	}
	if (IsDecideMarker(*body, SCOPE_WHEN_TAG)) {
		auto &when = body->Cast<const FunctionExpression>();
		ValidateScopeData(*when.children[1], variables);
		body = when.children[0].get();
	}
	return *body;
}

namespace {
//! Where in a clause a node stands, for the placement rules.
struct ScopePlacement {
	const case_insensitive_map_t<idx_t> &variables;
	//! Inside a reducer's argument.
	bool in_reducer = false;
	//! Inside the argument of a reducer over decisions.
	bool in_decision_reducer = false;
	//! Under a `*` or `/` that holds a decision: a reducer over data alone there scales the
	//! decision, a coefficient ValidateDecideNoNonLinearScalar refuses with its remedy.
	bool scales_a_decision = false;
	//! An enclosing reducer chooses rows by its own `when` or `per`, or groups them by a
	//! `by`: a reducer inside reads them by the path rule (syntax_reference.md §5.5).
	bool path_keeps_rows = false;
	//! The reducer about to be met is the first child of a `by (...)` with a key.
	bool under_by = false;
};
} // namespace

static void ValidateScopePlacement(const ParsedExpression &expr, ScopePlacement placement) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		ValidateNoScopeSpelling(expr);
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::DECIDE_FRAME) {
		ValidateFrame(expr.Cast<DecideFrameExpression>(), placement.variables);
		return;
	}
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { ValidateScopePlacement(child, placement); });
		return;
	}
	auto &func = expr.Cast<const FunctionExpression>();
	ValidateFrameProduct(func, placement.variables);
	if (func.is_operator && func.function_name == REDUCER_BY_TAG) {
		if (!TakesReducerBy(UnwrapReducerMarkers(*func.children[0]))) {
			throw BinderException(expr, "DECIDE: by (...) must directly follow SUM, AVG, MIN, MAX, norm or count");
		}
		for (idx_t i = 1; i < func.children.size(); i++) {
			ValidateScopeData(*func.children[i], placement.variables);
		}
		auto reducer_placement = placement;
		reducer_placement.under_by = func.children.size() > 1;
		ValidateScopePlacement(*func.children[0], reducer_placement);
		return;
	}
	if (IsDecideScopeMarker(expr)) {
		// Only in front of a clause or of a reducer's argument, both peeled before here.
		ValidateNoScopeSpelling(expr);
	}
	// The colon form takes SUM, AVG, MIN or MAX only.
	if (IsDecideMarker(expr, QUALIFIED_REDUCER_TAG) && func.children.size() > 1 &&
	    func.children[0]->GetExpressionClass() == ExpressionClass::FUNCTION &&
	    StringUtil::CIEquals(func.children[0]->Cast<const FunctionExpression>().function_name, "norm")) {
		vector<string> relations;
		for (idx_t i = 1; i < func.children.size(); i++) {
			relations.push_back(func.children[i]->ToString());
		}
		throw BinderException(expr, "DECIDE: norm(%s: ...) is not supported yet; a relation qualifier takes SUM, AVG, "
		                            "MIN or MAX",
		                      StringUtil::Join(relations, ", "));
	}
	bool scoped_argument = !func.is_operator && !func.children.empty() && IsDecideScopeMarker(*func.children[0]);
	if (scoped_argument && !IsDecideAggregateName(func.function_name)) {
		throw BinderException(expr, "DECIDE: when and per inside a call need SUM, AVG, MIN or MAX; write the scope "
		                            "in front of the constraint or inside a reducer");
	}
	// Every reducer, `count` among them: one over data alone added inside a reducer over
	// decisions is the refusal below, whatever its kind.
	bool reducer = TakesReducerBy(func);
	const bool reads_decision = ExpressionContainsDecideVariable(expr, placement.variables);
	// What a reducer inside another may not stand under (D74), each rewritten before the
	// value rewrite reads the path, so the inner reducer would lose the enclosing `when`
	// and `by`: an ABS, a norm, and a product with a decision.
	if (!func.is_operator && !func.children.empty() && ContainsDecideAggregate(*func.children[0]) &&
	    ExpressionContainsDecideVariable(*func.children[0], placement.variables)) {
		if (StringUtil::CIEquals(func.function_name, "norm")) {
			throw BinderException(expr, "DECIDE: a reducer inside norm is not supported yet");
		}
		if (StringUtil::CIEquals(func.function_name, "abs") && placement.path_keeps_rows) {
			throw BinderException(expr, "DECIDE: abs over a reducer inside another reducer is not supported yet: %s",
			                      expr.ToString());
		}
	}
	if (placement.in_reducer && func.is_operator && func.function_name == "*") {
		for (idx_t i = 0; i < func.children.size(); i++) {
			// A reducer over decisions: one over data alone scaling a decision is the
			// coefficient ValidateDecideNoNonLinearScalar refuses with its remedy.
			if (!ContainsDecideAggregate(*func.children[i]) ||
			    !ExpressionContainsDecideVariable(*func.children[i], placement.variables)) {
				continue;
			}
			for (idx_t j = 0; j < func.children.size(); j++) {
				if (j != i && ExpressionContainsDecideVariable(*func.children[j], placement.variables)) {
					throw BinderException(expr,
					                      "DECIDE: '%s' multiplies a reducer by a decision; only data can scale a "
					                      "reducer",
					                      expr.ToString());
				}
			}
		}
	}
	// A reducer nests in another to any depth (syntax_reference.md §5.5), with two exceptions
	// (D74): a norm, and a reducer over data alone in the argument of one over decisions --
	// unless it scales a decision there, a coefficient ValidateDecideNoNonLinearScalar
	// refuses with its remedy.
	if (reducer && placement.in_reducer && StringUtil::CIEquals(func.function_name, "norm")) {
		throw BinderException(expr, "DECIDE: norm inside a reducer is not supported yet");
	}
	if (reducer && placement.in_decision_reducer && !reads_decision && !placement.scales_a_decision) {
		throw BinderException(expr,
		                      "DECIDE: %s(...) over data alone inside a reducer over decisions is not supported yet; "
		                      "compute it in a subquery, or reduce it outside",
		                      func.function_name == "count_star" ? string("count")
		                                                         : StringUtil::Lower(func.function_name));
	}
	if (func.is_operator && (func.function_name == "*" || func.function_name == "/") && reads_decision) {
		placement.scales_a_decision = true;
	}
	auto inner = placement;
	if (reducer) {
		inner.in_reducer = true;
		inner.in_decision_reducer = reads_decision;
		inner.scales_a_decision = false;
		inner.path_keeps_rows = placement.path_keeps_rows || placement.under_by || scoped_argument;
	}
	inner.under_by = false;
	// The colon form's relations keep one row of each class, as a `per` does.
	placement.under_by = IsDecideMarker(expr, QUALIFIED_REDUCER_TAG);
	for (idx_t i = 0; i < func.children.size(); i++) {
		auto &child = i == 0 && scoped_argument ? PeelScopeSpelling(*func.children[0], placement.variables)
		                                        : *func.children[i];
		ValidateScopePlacement(child, reducer ? inner : placement);
	}
	if (func.filter) {
		ValidateNoScopeSpelling(*func.filter);
	}
}

static void ValidateScopeClause(const ParsedExpression &clause, const case_insensitive_map_t<idx_t> &variables,
                                bool objective) {
	auto &body = PeelScopeSpelling(clause, variables);
	// Every clause follows the scope rules (D30).
	ValidateScopePlacement(body, ScopePlacement {variables});
}

void ValidateDecideScopeSpellings(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                  bool objective) {
	if (!objective && expr.GetExpressionClass() == ExpressionClass::CONJUNCTION &&
	    expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		// `AND` separates constraints; each is checked on its own.
		for (auto &child : expr.Cast<const ConjunctionExpression>().children) {
			ValidateDecideScopeSpellings(*child, variables, objective);
		}
		return;
	}
	ValidateScopeClause(expr, variables, objective);
}


//! Column `column_index` of `binding` as a key column. A base table's goes through
//! GetColumnBinding, which also forces the column into the scan and returns its
//! position there; any other relation's binding is (table index, column).
static ColumnBinding RegisterKeyColumn(Binding &binding, column_t column_index) {
	if (binding.binding_type == BindingType::TABLE) {
		return binding.Cast<TableBinding>().GetColumnBinding(column_index);
	}
	return ColumnBinding(binding.index, column_index);
}

//! Whether column `column_index` of `binding` stores a value per row: false for `rowid`
//! and for a generated column, which a key would only repeat.
static bool IsStoredColumn(Binding &binding, column_t column_index) {
	if (column_index == COLUMN_IDENTIFIER_ROW_ID) {
		return false;
	}
	auto entry = binding.GetStandardEntry();
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return true;
	}
	return !entry->Cast<TableCatalogEntry>().GetColumn(LogicalIndex(column_index)).Generated();
}

//! One column of a key: where it binds, its type, and its name as a message quotes it --
//! as written for a column element, qualified by its relation (`D.depotID`) for a column
//! a relation element stands for.
struct KeyColumn {
	ColumnBinding binding;
	LogicalType type;
	string name;
};
using KeyColumns = vector<KeyColumn>;

//! Column `column_index` of `binding` as a key column, registered with the scan.
static KeyColumn MakeKeyColumn(Binding &binding, column_t column_index) {
	return KeyColumn {RegisterKeyColumn(binding, column_index), binding.types[column_index],
	                  KeywordHelper::WriteOptionallyQuoted(binding.GetAlias()) + "." +
	                      KeywordHelper::WriteOptionallyQuoted(binding.names[column_index])};
}

//! Adds the stored columns of `relation`: a relation in a key stands for them.
static void AddRelationColumns(Binding &relation, KeyColumns &columns) {
	for (column_t column_index = 0; column_index < relation.names.size(); column_index++) {
		if (IsStoredColumn(relation, column_index)) {
			columns.push_back(MakeKeyColumn(relation, column_index));
		}
	}
}

//! Adds column `column_index` of `binding`, which `element` names, to `columns`. `key_word`
//! names the key in a message: `per`, `within`.
static void AddKeyColumn(const ColumnRefExpression &element, Binding &binding, column_t column_index,
                         KeyColumns &columns, const char *key_word) {
	if (!IsStoredColumn(binding, column_index)) {
		throw BinderException(element,
		                      "DECIDE: %s is not a stored column; a %s key lists columns or relations of the "
		                      "FROM clause",
		                      element.ToString(), key_word);
	}
	auto column = MakeKeyColumn(binding, column_index);
	column.name = element.ToString();
	columns.push_back(std::move(column));
}

//! The relation named by up to three parts: `alias`, `schema.alias`, `catalog.schema.alias`.
static BindingAlias RelationAlias(const vector<string> &parts) {
	switch (parts.size()) {
	case 1:
		return BindingAlias(parts[0]);
	case 2:
		return BindingAlias(parts[0], parts[1]);
	default:
		D_ASSERT(parts.size() == 3);
		return BindingAlias(parts[0], parts[1], parts[2]);
	}
}

//! The column `name` written unqualified, as it binds anywhere else in the query: a USING
//! column means its primary relation's, and after a FULL OUTER JOIN, which has none, the
//! key must say which side it means. Null when no relation has the column.
static optional_ptr<Binding> UnqualifiedColumnBinding(BindContext &bind_context, const ColumnRefExpression &element,
                                                      const char *key_word) {
	auto &name = element.GetColumnName();
	auto using_set = bind_context.GetUsingBinding(name);
	if (!using_set) {
		return bind_context.GetMatchingBinding(name);
	}
	if (!using_set->primary_binding.IsSet()) {
		vector<string> choices;
		for (auto &side : using_set->bindings) {
			choices.push_back(KeywordHelper::WriteOptionallyQuoted(side.GetAlias()) + "." +
			                  KeywordHelper::WriteOptionallyQuoted(name));
		}
		throw BinderException(element, "DECIDE: %s is merged by a FULL OUTER JOIN USING; write %s in the %s key",
		                      element.ToString(), StringUtil::Join(choices, " or "), key_word);
	}
	ErrorData error;
	return bind_context.GetBinding(using_set->primary_binding, error);
}

//! The relation of the FROM clause named `parts` (up to three), or null: the decisions'
//! binding is not one, so a relation of the FROM clause that shares its name is not
//! ambiguous with it.
static optional_ptr<Binding> DataRelation(BindContext &bind_context, const vector<string> &parts,
                                          const DecideKeyDecisions &decisions) {
	ErrorData error;
	auto alias = RelationAlias(parts);
	optional_ptr<Binding> relation;
	idx_t matches = 0;
	for (auto &candidate : bind_context.GetBindings(alias, error)) {
		if (candidate.get().index != decisions.binding_index) {
			relation = &candidate.get();
			matches++;
		}
	}
	// Two relations of the FROM clause by one name: SQL's own message says which.
	return matches > 1 ? bind_context.GetBinding(alias, error) : relation;
}

//! The word a message names a key by: `per` for a `per` key, `within` for a frame's.
static const char *KeyWord(DecideDataPosition position) {
	return position == DecideDataPosition::FRAME_KEY ? "within" : "per";
}

//! Resolves one element of a key at `position` -- a `per` key, or a frame's `within` key --
//! into `columns`, against the FROM clause alone and the way SQL resolves the same name: a
//! column (`c`, `R.c`, `S.R.c`, `C.S.R.c`), else a relation (`R`, `S.R`, `C.S.R`), standing
//! for every column it stores. A decision is no column of it, whether named bare or through
//! its binding (`decide_variables.x`).
static void ResolveKeyElement(BindContext &bind_context, const ColumnRefExpression &element,
                              const DecideKeyDecisions &decisions, KeyColumns &columns, DecideDataPosition position) {
	auto &names = element.column_names;
	auto key_word = KeyWord(position);
	if (names.size() == 1 && decisions.names.count(names[0])) {
		throw BinderException(element, DecideDecisionInDataFormat(position), element.ToString());
	}
	ErrorData error;
	optional_ptr<Binding> binding;
	if (names.size() == 1) {
		binding = UnqualifiedColumnBinding(bind_context, element, key_word);
	} else if (names.size() <= 4) {
		binding = bind_context.GetBinding(RelationAlias(vector<string>(names.begin(), names.end() - 1)), names.back(),
		                                   error);
	}
	if (binding && binding->index == decisions.binding_index) {
		throw BinderException(element, DecideDecisionInDataFormat(position), element.ToString());
	}
	if (binding) {
		AddKeyColumn(element, *binding, binding->GetBindingIndex(names.back()), columns, key_word);
		return;
	}
	auto relation = names.size() <= 3 ? DataRelation(bind_context, names, decisions) : nullptr;
	if (!relation) {
		throw BinderException(element, "DECIDE: %s is neither a column nor a relation of the FROM clause",
		                      element.ToString());
	}
	AddRelationColumns(*relation, columns);
}

//! A key is a set: order and repetition do not change which rows share a class, so its
//! columns are kept sorted by binding, without repeats.
static KeyColumns SortedKey(KeyColumns columns) {
	std::sort(columns.begin(), columns.end(), [](const KeyColumn &a, const KeyColumn &b) {
		return a.binding.table_index != b.binding.table_index ? a.binding.table_index < b.binding.table_index
		                                                      : a.binding.column_index < b.binding.column_index;
	});
	columns.erase(std::unique(columns.begin(), columns.end(),
	                          [](const KeyColumn &a, const KeyColumn &b) { return a.binding == b.binding; }),
	              columns.end());
	return columns;
}

//! The columns of a key listed by `elements[first..]` (none for `()`), each element
//! resolved by ResolveKeyElement.
static KeyColumns ResolveKey(BindContext &bind_context, const vector<unique_ptr<ParsedExpression>> &elements,
                             idx_t first, const DecideKeyDecisions &decisions, DecideDataPosition position) {
	KeyColumns columns;
	for (idx_t i = first; i < elements.size(); i++) {
		auto &element = *elements[i];
		if (element.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			throw InternalException("DECIDE %s key element '%s' is not a name", KeyWord(position), element.ToString());
		}
		ResolveKeyElement(bind_context, element.Cast<ColumnRefExpression>(), decisions, columns, position);
	}
	if (columns.empty() && elements.size() > first) {
		throw InternalException("DECIDE %s key resolved to no column", KeyWord(position));
	}
	return SortedKey(std::move(columns));
}

//! The columns of the key `per` lists: its children[1..].
static KeyColumns ResolveKey(BindContext &bind_context, const FunctionExpression &per,
                             const DecideKeyDecisions &decisions) {
	return ResolveKey(bind_context, per.children, 1, decisions, DecideDataPosition::SCOPE);
}

//! The scope keyed on `columns` (a SortedKey): one per key, whichever spelling names it.
//! `key_text` is the key as first written, kept for display.
static idx_t FindOrCreateExactScope(const KeyColumns &columns, const string &key_text,
                                    vector<EntityScopeInfo> &entity_scopes) {
	vector<ColumnBinding> bindings;
	for (auto &column : columns) {
		bindings.push_back(column.binding);
	}
	for (idx_t scope_idx = 0; scope_idx < entity_scopes.size(); scope_idx++) {
		if (entity_scopes[scope_idx].entity_key_bindings == bindings) {
			return scope_idx;
		}
	}
	EntityScopeInfo scope_info;
	scope_info.table_alias = key_text.empty() ? "()" : key_text;
	scope_info.entity_key_bindings = std::move(bindings);
	for (auto &column : columns) {
		scope_info.entity_key_column_types.push_back(column.type);
		scope_info.key_names.push_back(column.name);
		auto &tables = scope_info.source_table_indices;
		if (std::find(tables.begin(), tables.end(), column.binding.table_index) == tables.end()) {
			tables.push_back(column.binding.table_index);
		}
	}
	entity_scopes.push_back(std::move(scope_info));
	return entity_scopes.size() - 1;
}

idx_t FindOrCreateKeyScope(BindContext &bind_context, const FunctionExpression &per,
                           const DecideKeyDecisions &decisions, const string &key_text,
                           vector<EntityScopeInfo> &entity_scopes) {
	return FindOrCreateExactScope(ResolveKey(bind_context, per, decisions), key_text, entity_scopes);
}

vector<unique_ptr<Expression>> BindKeyColumns(BindContext &bind_context, const FunctionExpression &per,
                                              const DecideKeyDecisions &decisions) {
	vector<unique_ptr<Expression>> result;
	for (auto &column : ResolveKey(bind_context, per, decisions)) {
		result.push_back(make_uniq<BoundColumnRefExpression>(column.name, column.type, column.binding));
	}
	return result;
}

//! The relation `element` names when it names no column of the FROM clause: `by (D)`
//! stands for D's stored columns, as `per D` does. Null for a column or an unknown name;
//! the decisions' binding is no relation of it.
static optional_ptr<Binding> ResolveRelationName(BindContext &bind_context, const ColumnRefExpression &element,
                                                 const DecideKeyDecisions &decisions) {
	auto &names = element.column_names;
	ErrorData error;
	// A column wins, as it does in SQL; an ambiguous one is still a column, left for the
	// ordinary binder to report.
	bool column = false;
	if (names.size() == 1) {
		column = bind_context.GetUsingBinding(names[0]) != nullptr;
		for (auto &binding : bind_context.GetBindingsList()) {
			column = column || binding->HasMatchingBinding(names[0]);
		}
	} else if (names.size() <= 4) {
		column = bind_context.GetBinding(RelationAlias(vector<string>(names.begin(), names.end() - 1)), names.back(),
		                                 error) != nullptr;
	}
	if (column || names.size() > 3) {
		return nullptr;
	}
	auto relation = DataRelation(bind_context, names, decisions);
	if (!relation && bind_context.GetBinding(RelationAlias(names), error)) {
		throw BinderException(element, "DECIDE: %s is neither a column nor a relation of the FROM clause",
		                      element.ToString());
	}
	return relation;
}

//! A relation's stored columns as bound column references, registered with the scan.
static vector<unique_ptr<Expression>> BindRelationColumns(Binding &relation) {
	KeyColumns columns;
	AddRelationColumns(relation, columns);
	vector<unique_ptr<Expression>> result;
	for (auto &column : columns) {
		result.push_back(make_uniq<BoundColumnRefExpression>(column.name, column.type, column.binding));
	}
	return result;
}

bool ContainsDecideAggregate(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (!func.is_operator && (IsDecideAggregateName(func.function_name) || StringUtil::CIEquals(func.function_name, "norm"))) {
			return true;
		}
		// A condition, a key or a `by` key is not part of the value.
		if (func.is_operator && !func.children.empty() &&
		    (func.function_name == REDUCER_BY_TAG || IsDecideScopeMarker(expr))) {
			return ContainsDecideAggregate(*func.children[0]);
		}
		for (auto &child : func.children) {
			if (ContainsDecideAggregate(*child)) {
				return true;
			}
		}
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::OPERATOR) {
		auto &op = expr.Cast<const OperatorExpression>();
		for (auto &child : op.children) {
			if (ContainsDecideAggregate(*child)) {
				return true;
			}
		}
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		auto &cast = expr.Cast<const CastExpression>();
		return ContainsDecideAggregate(*cast.child);
	}
	return false;
}

// Names that can legally wrap a DECIDE variable at bind time. ABS is
// linearized by the optimizer; POWER/POW(expr, 2) feeds the QP objective path;
// SUM/AVG/MIN/MAX/COUNT are aggregates handled in BindAggregate; +, -, *, /,
// **, and unary tags fall through to the normal linear-extraction path.
// Including aggregate names prevents false-flagging the IN-rewrite's "+"
// nodes (synthesized as non-operator FunctionExpression) or nested aggregates.
static bool IsAllowedNameOverDecideVar(const string &name) {
	auto lname = StringUtil::Lower(name);
	if (lname == "abs" || lname == "power" || lname == "pow") return true;
	if (lname == "norm") return true; // bound as an optimizer-owned aggregate marker below
	if (lname == "sum" || lname == "avg" || lname == "min" || lname == "max") return true;
	if (lname == "count" || lname == "count_star") return true;
	if (lname == "+" || lname == "-" || lname == "*" || lname == "/" || lname == "**") return true;
	// DecidB-internal tag operators (__when_constraint__, __per_constraint__, ...)
	// are synthesized as FunctionExpression and handled by dedicated binders.
	if (name.size() >= 4 && name.substr(0, 2) == "__" && name.substr(name.size() - 2) == "__") return true;
	return false;
}

// True when the function name resolves to an AGGREGATE in the catalog. These
// are rejected (with an aggregate-specific error) in BindAggregate, so we must
// not also false-flag them here as "non-linear scalars".
static bool IsAggregateFunctionName(ClientContext &context, const FunctionExpression &func) {
	auto entry = Catalog::GetEntry(context, CatalogType::SCALAR_FUNCTION_ENTRY,
	                                func.catalog, func.schema, func.function_name,
	                                OnEntryNotFound::RETURN_NULL);
	return entry && entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY;
}

// Reject any non-linear scalar function that wraps a DECIDE variable. Mirrors
// the COUNT(decide_var) guard in BindAggregate: catch the mis-use at bind time
// with a semantic-specific message instead of letting it slip through to
// per-row execution, which would silently strip the scalar and produce wrong
// answers. Functions that only wrap table columns are fine — the
// wrapper folds to a per-row constant before the solver sees it.
// True if this function is POWER / POW / **. These share the same validation:
// exponent must be a constant numeric 2. All other exponents are non-linear
// (fractional → radicals, negative → reciprocal, variable → exponential),
// or unsupported higher-degree integer powers (3+ → cubic and above).
static bool IsPowerFunction(const FunctionExpression &func) {
	auto lname = StringUtil::Lower(func.function_name);
	return lname == "power" || lname == "pow" || lname == "**";
}

// Reject POWER(base, exp) when base contains a decide variable and exp is not
// a constant numeric 2. Mirrors ValidateQuadraticPower's error messages so
// test matchers and user-facing wording are consistent across SUM and non-SUM
// contexts. Running at bind time keeps a bad exponent from reaching the
// quadratic extractor, which recognizes only POWER(linear_expr, 2).
static void ValidatePowerExponent(const FunctionExpression &func,
                                  const case_insensitive_map_t<idx_t> &variables) {
	if (func.children.size() != 2) {
		return; // Arity errors are surfaced elsewhere with clearer messages.
	}
	// Only gate POWER whose base transitively references a DECIDE variable.
	// POWER over pure data columns folds to a per-row constant and is fine.
	if (!ExpressionContainsDecideVariable(*func.children[0], variables)) {
		return;
	}
	auto &exponent = *func.children[1];
	if (exponent.GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw BinderException(
		    "POWER exponent in DECIDE expression must be a constant integer "
		    "(only 2 is supported)");
	}
	auto &exp_const = exponent.Cast<const ConstantExpression>();
	double exp_val;
	try {
		exp_val = exp_const.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	} catch (...) {
		throw BinderException("POWER exponent must be numeric");
	}
	if (exp_val != 2.0) {
		throw BinderException(
		    "Only POWER(expr, 2) is supported for quadratic expressions. "
		    "Found exponent %g. Higher powers are not allowed.", exp_val);
	}
}

// True if `expr` contains an aggregate over table columns only (no DECIDE
// variable anywhere in its argument) — e.g. `AVG(price)`, `MIN(cost)`. Such a
// value needs the whole row set, so it is a scalar the query must pre-compute,
// not a per-row coefficient. Recurses so nested forms (`AVG(p) * 2`) are caught.
static bool ContainsDataOnlyAggregate(ClientContext &context, const ParsedExpression &expr,
                                      const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (IsAggregateFunctionName(context, func) &&
		    !ExpressionContainsDecideVariable(expr, variables)) {
			return true;
		}
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (ContainsDataOnlyAggregate(context, child, variables)) {
			found = true;
		}
	});
	return found;
}

void ValidateDecideNoNonLinearScalar(ClientContext &context,
                                     const ParsedExpression &expr,
                                     const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (IsPowerFunction(func)) {
			ValidatePowerExponent(func, variables);
		} else if (func.is_operator && func.function_name == "*" &&
		           ExpressionContainsDecideVariable(expr, variables) &&
		           ContainsDataOnlyAggregate(context, expr, variables)) {
			// A data-only aggregate multiplied by a decision variable
			// (`SUM(avg(p) * x)`) is not a per-row coefficient: the aggregate
			// spans all rows. Symbolic normalization would silently distribute the
			// variable into it (`avg(p)*x` -> `avg(p*x)`, a different constraint) and
			// return a wrong optimum, so reject it here — before normalization —
			// consistent with the direct `SUM(sum(p) * x)` rejection.
			throw BinderException(
			    "An aggregate over table columns (e.g. AVG(col), MIN(col)) cannot multiply a "
			    "decision variable. Pre-compute it as a scalar in a subquery or CTE and reference "
			    "that value.");
		} else if (func.is_operator && func.function_name == "/") {
			// Division is only linear when the divisor contains no decide
			// variable. x / y (decide vars in divisor) is non-linear; catch
			// it here so it doesn't fall through to per-row extraction
			// (which would silently produce wrong results) or symbolic
			// normalization (which would throw InternalException).
			if (func.children.size() == 2 &&
			    ExpressionContainsDecideVariable(*func.children[1], variables)) {
				throw BinderException(
				    "Division by a DECIDE variable is not supported: "
				    "it would make the model non-linear. The divisor must "
				    "not reference a decision variable.");
			}
			// A data-only aggregate stays on the bound side (S10). Like the multiplier
			// above, it may not share a scaled term with a decision: as the divisor, or
			// inside the dividend (`(sum(x) + sum(cap)) / 2`).
			if (func.children.size() == 2 && ExpressionContainsDecideVariable(expr, variables) &&
			    ContainsDataOnlyAggregate(context, expr, variables)) {
				throw BinderException(
				    "An aggregate over table columns (e.g. AVG(col), MIN(col)) cannot %s a decision "
				    "variable. Pre-compute it as a scalar in a subquery or CTE and reference that value.",
				    ContainsDataOnlyAggregate(context, *func.children[1], variables) ? "divide"
				                                                                    : "be divided together with");
			}
		} else if (!IsAllowedNameOverDecideVar(func.function_name) &&
		           !IsAggregateFunctionName(context, func)) {
			for (auto &child : func.children) {
				if (ExpressionContainsDecideVariable(*child, variables)) {
					throw BinderException(
					    "Scalar function '%s' over a DECIDE variable is not supported: "
					    "it would make the model non-linear. Only ABS() and POWER(..., 2) "
					    "can wrap a decision variable.",
					    func.function_name);
				}
			}
		}
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		ValidateDecideNoNonLinearScalar(context, child, variables);
	});
}

//! Does this node reduce whatever is under it to an integer, whatever that operand's
//! declared type is? Only `norm(e, 0, M)` does: the L0 "norm" counts how many entries
//! are nonzero, so its value is a count and lands on the integer lattice even for a
//! REAL `e`. That is the definition of L0, not a property of how the count is later
//! linearized, so the integer-step rewrites are exact over it.
//!
//! The other orders are not counts and do not qualify: `norm(e, 1)` sums magnitudes,
//! `norm(e, 2)` is a Euclidean length, `norm(e, 'inf')` is a maximum — each is as
//! continuous as `e` is.
static bool IsIntegerValuedReducer(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<const FunctionExpression>();
	if (func.is_operator || !StringUtil::CIEquals(func.function_name, "norm")) {
		return false;
	}
	// Shape is validated properly when the marker is bound; here an unparseable order
	// simply is not the L0 case, and the real diagnosis follows from the binder.
	if (func.children.size() < 2 || func.children[1]->GetExpressionClass() != ExpressionClass::CONSTANT) {
		return false;
	}
	auto &order_value = func.children[1]->Cast<const ConstantExpression>().value;
	if (!order_value.type().IsNumeric()) {
		return false; // 'inf'
	}
	try {
		return order_value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>() == 0.0;
	} catch (...) {
		return false;
	}
}

//! Name of the first REAL-declared decision variable referenced by `expr`, as the
//! user spelled it, or empty if there is none. Not inside a subquery's body, where a
//! decision cannot be read (DecideBinder).
//!
//! Stops at an integer-valued reducer: what a comparison compares is the reducer's
//! value, so a REAL decision counted by `norm(..., 0, M)` is not the compared
//! quantity and must not be reported as one.
static string FindRealDecideVariable(const ParsedExpression &expr,
                                     const case_insensitive_map_t<idx_t> &variables,
                                     const vector<LogicalType> &variable_types) {
	if (IsIntegerValuedReducer(expr)) {
		return "";
	}
	if (IsVariableExpression(expr, variables)) {
		const auto &colref = expr.Cast<const ColumnRefExpression>();
		auto it = variables.find(colref.GetColumnName());
		if (it != variables.end() && it->second < variable_types.size()) {
			const auto &type = variable_types[it->second];
			if (type == LogicalType::DOUBLE || type == LogicalType::FLOAT) {
				return colref.GetColumnName();
			}
		}
	}

	string found;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (found.empty()) {
			found = FindRealDecideVariable(child, variables, variable_types);
		}
	});
	return found;
}

void ValidateDecideNoIntegerStepComparisonOnReal(const ParsedExpression &expr,
                                                 const case_insensitive_map_t<idx_t> &variables,
                                                 const vector<LogicalType> &variable_types) {
	// Only comparisons in constraint position are checked — the ones that become
	// model rows and would therefore be encoded by stepping the bound. A comparison
	// nested inside an operand is a boolean value, not a constraint: in
	// `(sum(x) > 1) + 3 <= 10` the `sum(x) > 1` is added to 3, and the type error
	// that names is a better diagnosis than anything said about strictness. So this
	// descends the way DecideConstraintsBinder does — through conjunctions and
	// through the constraint child of a `when` / `per` wrapper — and no further.
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::CONJUNCTION: {
		auto &conj = expr.Cast<const ConjunctionExpression>();
		for (const auto &child : conj.children) {
			ValidateDecideNoIntegerStepComparisonOnReal(*child, variables, variable_types);
		}
		return;
	}
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<const FunctionExpression>();
		if (IsDecideScopeMarker(expr) && !func.children.empty()) {
			// children[0] is the constraint; the rest are the condition / key.
			ValidateDecideNoIntegerStepComparisonOnReal(*func.children[0], variables, variable_types);
		}
		return;
	}
	case ExpressionClass::COMPARISON: {
		auto type = expr.GetExpressionType();
		bool is_strict =
		    type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_GREATERTHAN;
		bool is_not_equal = type == ExpressionType::COMPARE_NOTEQUAL;
		if (is_strict || is_not_equal) {
			auto &comp = expr.Cast<const ComparisonExpression>();
			// Both sides, because canonicalization has not run yet: the user may have
			// written `5 > x` as readily as `x < 5`, and reading a side is not moving one.
			string real_var = FindRealDecideVariable(*comp.left, variables, variable_types);
			if (real_var.empty()) {
				real_var = FindRealDecideVariable(*comp.right, variables, variable_types);
			}
			if (real_var.empty()) {
				return;
			}
			if (is_not_equal) {
				// No repair by stepping exists here, unlike the strict case: excluding a
				// single point from a continuous range removes nothing a solver can act
				// on. So the advice is to change the declared type or to name the range
				// the user actually wants excluded.
				//
				// Spelled by hand rather than through comp.ToString(), which renders
				// COMPARE_NOTEQUAL as `!=`. The clause is quoted back to the user, so it
				// has to read the way they wrote it.
				string clause = "(" + comp.left->ToString() + " <> " + comp.right->ToString() + ")";
				throw BinderException(
				    comp,
				    "Inequality '<>' is not supported over the REAL decision '%s': '%s'. "
				    "A REAL decision can take values arbitrarily close to the compared value "
				    "on either side, so excluding that single point rules out nothing. "
				    "Declare '%s' as INT if the quantity is a whole number, or use '<=' / '>=' "
				    "to exclude the range you actually mean.",
				    real_var, clause, real_var);
			}
			bool is_less = type == ExpressionType::COMPARE_LESSTHAN;
			throw BinderException(
			    comp,
			    "Strict inequality '%s' is not supported over the REAL decision '%s': '%s'. "
			    "A REAL decision takes any value up to the bound, so there is no next value "
			    "to stop at. Use '%s' instead, and move the bound by one step if the compared "
			    "quantity is a count or other whole number.",
			    is_less ? "<" : ">", real_var, comp.ToString(), is_less ? "<=" : ">=");
		}
		return;
	}
	default:
		return;
	}
}

//===--------------------------------------------------------------------===//
// Integrality of a compared side, from declared types
//===--------------------------------------------------------------------===//

//! Does this side reference a decision variable? Only such a side is a *compared
//! quantity*; a side made of constants and data columns is the bound `K`.
//!
//! `K` is deliberately exempt from the integrality refusal, because a fractional bound is
//! not an error: no whole number equals 2.5, so `SUM(x) <> 2.5` is a tautology and the
//! rewrite drops it, and an infinite bound reads the same way. Refusing here would turn
//! two well-defined outcomes into errors. This mirrors the value-based guards this
//! validator replaced, which read `variable_indices` and `row_coefficients` and never
//! looked at `rhs_values`.
static bool SideReferencesDecision(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<const BoundColumnRefExpression>();
		if (colref.binding.table_index == decide_index) {
			return true;
		}
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = SideReferencesDecision(child, decide_index);
		}
	});
	return found;
}

//! Does this type guarantee whole-numbered values?
//!
//! `DECIMAL` carries its scale in the type, so `DECIMAL(15, 0)` qualifies and
//! `DECIMAL(15, 2)` does not — the latter regardless of what its rows actually hold.
//! No floating type qualifies: `DOUBLE` can hold `3.0`, but nothing in the type says it
//! must, and a rule that depended on the stored values would make a query's validity
//! change when a row is inserted.
static bool DecideTypeIsWholeNumbered(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
		return true;
	case LogicalTypeId::DECIMAL:
		return DecimalType::GetScale(type) == 0;
	default:
		return false;
	}
}

//! Is this bound node the inert `norm(e, 0, M)` marker? The L0 "norm" counts nonzeros,
//! so its value is a count whatever `e` is typed as. `NORM_MARKER_TAG_PREFIX` is followed
//! by the order, so an L0 marker's tag begins `__decide_norm_0`.
static bool IsBoundL0NormMarker(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	string payload;
	if (!ExtractDecideTagPayload(expr.alias, NORM_MARKER_TAG_PREFIX, payload)) {
		return false;
	}
	return StringUtil::StartsWith(payload, "0");
}

//! Is this bound node an AVG aggregate -- a reducer, not a range frame's marker? A range
//! is a value of the row (syntax_reference.md §5.6), which no rewrite multiplies through.
static bool IsBoundAvgAggregate(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	auto &agg = expr.Cast<const BoundAggregateExpression>();
	idx_t frame;
	return StringUtil::CIEquals(agg.function.name, "avg") && !TryParseFrameTag(agg.GetAlias(), frame);
}

//! Is `expr` provably whole-numbered from declared types alone?
//!
//! `allow_avg_hoist` is set only for `<>`, whose rewrite multiplies both sides by the
//! group size and so compares `SUM(e)` against `K*n` — integral whenever `e` is. A
//! strict comparison has no such hoist and keeps AVG's fractional 1/n coefficients.
//!
//! `addends_count` is set where a decision-free addend is part of the value rather than
//! of the bound: inside a MIN or MAX, whose strict bound is stepped on the extremum itself
//! (stage 06), and under anything that is not linear in it -- a power, a product of
//! decisions, a norm of order 1, 2 or 'inf' (`|x + 0.5|`, `(x + 0.25)^2`, `(x + 0.5) * y`
//! are not whole).
static bool DecideSideIsWholeNumbered(const Expression &expr, bool allow_avg_hoist, idx_t decide_index,
                                      bool addends_count = false) {
	if (IsBoundL0NormMarker(expr)) {
		return true;
	}
	if (allow_avg_hoist && IsBoundAvgAggregate(expr)) {
		auto &agg = expr.Cast<const BoundAggregateExpression>();
		return agg.children.size() == 1 &&
		       DecideSideIsWholeNumbered(*agg.children[0], false, decide_index, addends_count);
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CAST: {
		// A cast to a whole-numbered type makes the value one; a widening cast of an
		// already-whole operand (the binder inserts many, e.g. INTEGER -> DECIMAL(18,0))
		// keeps it one. Either direction suffices.
		auto &cast = expr.Cast<const BoundCastExpression>();
		return DecideTypeIsWholeNumbered(cast.return_type) ||
		       DecideSideIsWholeNumbered(*cast.child, allow_avg_hoist, decide_index, addends_count);
	}
	case ExpressionClass::BOUND_AGGREGATE: {
		// SUM / MIN / MAX of whole numbers are whole, and so is a norm of order 1, 2 or
		// 'inf' (a sum of absolute values or squares, a largest absolute value) of a whole
		// argument. AVG is handled above; anything else falls through to its declared
		// return type.
		auto &agg = expr.Cast<const BoundAggregateExpression>();
		const auto &name = agg.function.name;
		// A frame is a value of the row, every addend and its `else` part of it: whole when
		// the type its payload and `else` share is (BindDecideFrame casts both to it), and a
		// range's SUM, MIN or MAX of them; an AVG range divides.
		idx_t frame;
		if (TryParseFrameTag(agg.GetAlias(), frame)) {
			const bool averages = StringUtil::CIEquals(name, "avg");
			return !averages && agg.children.size() == 1 && DecideTypeIsWholeNumbered(agg.children[0]->return_type);
		}
		bool extremum = StringUtil::CIEquals(name, "min") || StringUtil::CIEquals(name, "max");
		bool norm = HasDecideTag(agg.alias, NORM_MARKER_TAG_PREFIX);
		if ((StringUtil::CIEquals(name, "sum") || extremum) && agg.children.size() == 1) {
			return DecideSideIsWholeNumbered(*agg.children[0], false, decide_index,
			                                 addends_count || norm || extremum);
		}
		return DecideTypeIsWholeNumbered(expr.return_type);
	}
	case ExpressionClass::BOUND_FUNCTION: {
		// Sums, differences and products of whole numbers are whole. Division is not,
		// and neither is anything else, so both fall through to the return type.
		auto &func = expr.Cast<const BoundFunctionExpression>();
		const auto &name = func.function.name;
		if (name == "+" || name == "-") {
			// An addend that references no decision is part of the bound, not of the
			// compared quantity: `x + 1000003.50 < K` is exactly `x < K - 1000003.50`,
			// and stepping that is still exact because `x` is still on the lattice. A
			// fractional offset moves the bound; it does not move the lattice. (The
			// value-based guard this replaces skipped constant terms for the same
			// reason — it only ever read coefficients carrying a variable index.)
			for (const auto &child : func.children) {
				if (!addends_count && !SideReferencesDecision(*child, decide_index)) {
					continue;
				}
				if (!DecideSideIsWholeNumbered(*child, allow_avg_hoist, decide_index, addends_count)) {
					return false;
				}
			}
			return true;
		}
		if (name == "*") {
			// Every factor matters here, decision-bearing or not: a fractional
			// multiplier rescales the decision off the lattice (`0.5 * x`). A product of
			// decisions is not linear in either factor's addends.
			idx_t decision_factors = 0;
			for (const auto &child : func.children) {
				decision_factors += SideReferencesDecision(*child, decide_index);
			}
			for (const auto &child : func.children) {
				if (!DecideSideIsWholeNumbered(*child, allow_avg_hoist, decide_index,
				                               addends_count || decision_factors > 1)) {
					return false;
				}
			}
			return true;
		}
		// A whole number raised to a whole, non-negative power is whole, so `POWER(x, 2)`
		// stays on the lattice even though the function's return type is DOUBLE. The
		// declared return type is the wrong thing to read here for the same reason it is
		// wrong for an L0 count. A fractional or negative exponent is not a polynomial
		// degree at all and `ValidatePowerExponent` rejects it, so it need not be
		// answered here.
		if (name == "pow" || name == "power" || name == "**" || name == "^") {
			double exp_val;
			if (func.children.size() == 2 && TryGetDecideConstantExponent(*func.children[1], exp_val) &&
			    exp_val >= 0.0 && exp_val == std::floor(exp_val)) {
				return DecideSideIsWholeNumbered(*func.children[0], allow_avg_hoist, decide_index,
				                                 true);
			}
			return false;
		}
		return DecideTypeIsWholeNumbered(expr.return_type);
	}
	default:
		return DecideTypeIsWholeNumbered(expr.return_type);
	}
}

bool DecideValueIsWholeNumbered(const Expression &expr, idx_t decide_index) {
	return DecideSideIsWholeNumbered(expr, false, decide_index, /*addends_count=*/true);
}

//! The operand a refusal should talk about.
struct FractionalOperand {
	string name;
	LogicalType type;
	//! A column can be cast to a whole-numbered type, and the message says so. An
	//! intrinsically fractional expression (`AVG(x)`, a division) cannot — casting it
	//! would truncate the value rather than state an assumption about it — so that
	//! message offers the non-strict operator instead.
	bool is_castable_column = false;
};

//! First operand that is not whole-numbered, for the message. Descends the same way the
//! test above does, so what it reports is what was refused.
static bool FindFractionalOperand(const Expression &expr, bool allow_avg_hoist, idx_t decide_index,
                                  optional_ptr<const DecideSourceNames> names, FractionalOperand &out) {
	if (DecideSideIsWholeNumbered(expr, allow_avg_hoist, decide_index)) {
		return false;
	}
	// Prefer a named leaf over the composite node above it: `l_quantity` is actionable,
	// `sum((x * l_quantity))` is not.
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = FindFractionalOperand(child, allow_avg_hoist, decide_index, names, out);
		}
	});
	if (found) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		out.name = expr.Cast<const BoundColumnRefExpression>().GetName();
		out.type = expr.return_type;
		out.is_castable_column = true;
		return true;
	}
	// No fractional leaf underneath: the node itself is what produces fractions, named as
	// written (its reducer's alias carries the source tags).
	out.name = names ? RenderDecideSource(expr, *names) : expr.GetName();
	out.type = expr.return_type;
	out.is_castable_column = false;
	return true;
}

void ValidateDecideIntegralComparisonOperands(const Expression &expr, idx_t decide_index,
                                              optional_ptr<const DecideSourceNames> names) {
	// Constraint position only, and by the same reasoning as the parsed-tree validator:
	// a comparison nested inside an operand is a boolean value, not a model row.
	// ForEachConstraintLeaf owns that descent; this is only the check.
	ForEachConstraintLeaf(expr, [&](const Expression &node) {
		if (node.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return;
		}
		auto type = node.GetExpressionType();
		bool is_strict =
		    type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_GREATERTHAN;
		bool is_not_equal = type == ExpressionType::COMPARE_NOTEQUAL;
		if (!is_strict && !is_not_equal) {
			return;
		}
		auto &comp = node.Cast<const BoundComparisonExpression>();
		// Only a side that carries decisions is a compared quantity; the other is the
		// bound, and a fractional bound is a tautology rather than an error.
		bool left_is_compared = SideReferencesDecision(*comp.left, decide_index);
		bool right_is_compared = SideReferencesDecision(*comp.right, decide_index);
		FractionalOperand bad;
		if (!(left_is_compared && FindFractionalOperand(*comp.left, is_not_equal, decide_index, names, bad)) &&
		    !(right_is_compared && FindFractionalOperand(*comp.right, is_not_equal, decide_index, names, bad))) {
			return;
		}
		string op = is_not_equal ? "<>" : (type == ExpressionType::COMPARE_LESSTHAN ? "<" : ">");
		string alternative = is_not_equal ? "'<=' or '>='"
		                                  : (type == ExpressionType::COMPARE_LESSTHAN ? "'<='" : "'>='");
		string why = is_not_equal
		                 ? "the rewrite (x <> K becomes x <= K-1 OR x >= K+1) is exact only on "
		                   "whole numbers"
		                 : "stepping the bound is exact only on whole numbers";
		// Pass the comparison so the message carries a caret. The bound tree only started
		// carrying source locations when DecideBinder::BindExpression began stamping them;
		// before that these two throws named no node because there was nothing to name.
		if (bad.is_castable_column) {
			throw BinderException(
			    comp,
			    "Comparison '%s' is not supported here: column '%s' has type %s, which allows "
			    "fractional values, and %s. If '%s' holds whole numbers, cast it "
			    "(%s::BIGINT); otherwise compare with %s.",
			    op, bad.name, bad.type.ToString(), why, bad.name, bad.name, alternative);
		}
		throw BinderException(
		    comp,
		    "Comparison '%s' is not supported here: '%s' has type %s and can take fractional "
		    "values, and %s. Compare with %s instead.",
		    op, bad.name, bad.type.ToString(), why, alternative);
	});
}

const char *DecideCaseUnsupportedMessage() {
	return "CASE expressions are not supported inside DECIDE constraints or "
	       "objectives. Write the condition as a scope in front of the clause "
	       "(when category = 'A': sum(x) >= 1), split rows into classes with per, "
	       "or pre-compute conditional values in a CTE or subquery before the "
	       "DECIDE clause.";
}

// Forward declaration — needed because ValidateQuadraticPower calls ValidateSumArgumentInternal.
static bool ValidateSumArgumentInternal(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                        bool &has_decide_variable, string &error_msg, bool allow_quadratic = false);

// Shared validation for POWER(expr, 2) and expr ** 2 patterns.
// Returns true if the pattern is a valid quadratic form; sets has_decide_variable.
// The base expression is validated as strictly linear (allow_quadratic=false) to prevent nesting.
static bool ValidateQuadraticPower(vector<unique_ptr<ParsedExpression>> &children,
                                   const case_insensitive_map_t<idx_t> &variables,
                                   bool &has_decide_variable, string &error_msg, const string &label) {
	if (children.size() != 2) {
		error_msg = label + " requires exactly two arguments";
		return false;
	}
	auto &exponent = *children[1];
	if (exponent.GetExpressionClass() != ExpressionClass::CONSTANT) {
		error_msg = "POWER exponent in DECIDE expression must be a constant integer (only 2 is supported)";
		return false;
	}
	auto &exp_const = exponent.Cast<ConstantExpression>();
	double exp_val;
	try {
		exp_val = exp_const.value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	} catch (...) {
		error_msg = "POWER exponent must be numeric";
		return false;
	}
	if (exp_val != 2.0) {
		error_msg = StringUtil::Format(
		    "Only POWER(expr, 2) is supported for quadratic expressions. "
		    "Found exponent %g. Higher powers are not allowed.", exp_val);
		return false;
	}
	// Validate the base is a LINEAR expression (allow_quadratic=false prevents nesting like POWER(POWER(x,2),2))
	bool base_has_var = false;
	if (!ValidateSumArgumentInternal(*children[0], variables, base_has_var, error_msg, /*allow_quadratic=*/false)) {
		error_msg = "Inside " + label + ": " + error_msg;
		return false;
	}
	if (!base_has_var) {
		error_msg = label + " in DECIDE expression must reference at least one DECIDE variable";
		return false;
	}
	has_decide_variable = true;
	return true;
}

static bool ValidateSumArgumentInternal(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables,
                                        bool &has_decide_variable, string &error_msg,
                                        bool allow_quadratic) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
        if (IsVariableExpression(expr, variables)) {
            has_decide_variable = true;
        }
        return true;
    }
    case ExpressionClass::CONSTANT:
        return true;
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		string func_name_lower = StringUtil::Lower(func.function_name);
		if (!func.is_operator) {
			if (func_name_lower == "abs") {
				// ABS is allowed inside SUM — will be linearized by the optimizer.
				// Treat ABS as opaque: just verify it references a decide variable.
				if (func.children.size() != 1) {
					error_msg = "ABS requires exactly one argument";
					return false;
				}
				if (ExpressionContainsDecideVariable(*func.children[0], variables)) {
					has_decide_variable = true;
				}
				return true;
			}
			if (func_name_lower == "power" || func_name_lower == "pow") {
				if (!ExpressionContainsDecideVariable(expr, variables)) {
					// POWER over data columns only — a per-row constant, handled by the
					// same rule as any other data-only scalar function below. The
					// quadratic gate exists for POWER over a DECIDE variable; applying it
					// here rejects `SUM(x * POWER(qty, 2))`, which is linear in x.
					return true;
				}
				if (!allow_quadratic) {
					error_msg = "SUM expression must remain linear in DECIDE variables — "
					            "POWER(expr, 2) is only allowed in objectives, not constraints";
					return false;
				}
				return ValidateQuadraticPower(func.children, variables, has_decide_variable, error_msg, "POWER(..., 2)");
			}
			if (func_name_lower == "min" || func_name_lower == "max" || func_name_lower == "sum" || func_name_lower == "avg") {
				// A nested reducer (syntax_reference.md §5.5): its body is held to the rule of any reducer's.
				if (func.children.size() == 1 && ExpressionContainsDecideVariable(*func.children[0], variables)) {
					auto &body = const_cast<ParsedExpression &>(ReducerArgumentBody(*func.children[0]));
					return ValidateSumArgumentInternal(body, variables, has_decide_variable, error_msg,
					                                   allow_quadratic);
				}
				error_msg = StringUtil::Format("Nested %s() inside DECIDE expression must reference a DECIDE variable",
				                               StringUtil::Upper(func_name_lower));
				return false;
			}
			if (!ExpressionContainsDecideVariable(expr, variables)) {
				// Data-only named scalar function on table columns (e.g. mod(id, 97),
				// floor(price)). It folds into a per-row coefficient during
				// symbolization and contributes no DECIDE variable. Data-only
				// aggregates (SUM/AVG/MIN/MAX of columns only) were already rejected
				// above — they require a DECIDE variable — so they never reach here.
				return true;
			}
			error_msg = StringUtil::Format("Unsupported function '%s' inside DECIDE SUM expression", func.function_name);
			return false;
		}
		if (func_name_lower == "**") {
			if (!ExpressionContainsDecideVariable(expr, variables)) {
				return true;
			}
			if (!allow_quadratic) {
				error_msg = "SUM expression must remain linear in DECIDE variables — "
				            "expr ** 2 is only allowed in objectives, not constraints";
				return false;
			}
			return ValidateQuadraticPower(func.children, variables, has_decide_variable, error_msg, "expr ** 2");
		}
		if (func.function_name == REDUCER_BY_TAG || func.function_name == QUALIFIED_REDUCER_TAG) {
			// A nested reducer's `by (G)` or relation qualifier: the reducer is the first child.
			return ValidateSumArgumentInternal(*func.children[0], variables, has_decide_variable, error_msg,
			                                   allow_quadratic);
		}
		if (func_name_lower == "-") {
			for (auto &child : func.children) {
				if (!ValidateSumArgumentInternal(*child, variables, has_decide_variable, error_msg, allow_quadratic)) {
					return false;
				}
			}
			return true;
		}
		if (func_name_lower == "/") {
			// Division is linear iff the divisor contains no decide variable
			// (numerator can reference decide vars; dividing by a data
			// expression is just a coefficient scale). x / y where both
			// sides are decide vars is non-linear and rejected here.
			if (func.children.size() != 2) {
				error_msg = "Division requires exactly two arguments";
				return false;
			}
			if (ExpressionContainsDecideVariable(*func.children[1], variables)) {
				error_msg = "Division by a DECIDE variable is not supported "
				            "(would make the model non-linear)";
				return false;
			}
			return ValidateSumArgumentInternal(*func.children[0], variables,
			                                   has_decide_variable, error_msg,
			                                   allow_quadratic);
		}
		if (func_name_lower == "*" || func_name_lower == "+") {
			for (auto &child : func.children) {
				if (!ValidateSumArgumentInternal(*child, variables, has_decide_variable, error_msg, allow_quadratic)) {
					return false;
				}
			}
			// Degree is deliberately not judged here. It is one concept with one owner —
			// `ValidateDecideConstraintDegree` / `ValidateDecideObjectiveDegree`, which run on
			// the bound tree once binding is complete — so that a per-row constraint and a
			// reducer argument are held to the same rule. This branch used to re-derive it,
			// and because its name scoped it to SUM arguments nothing called it for
			// `POWER(x*y, 2) <= 5`, which then reached term extraction at layer 5.
			return true;
		}
		// An operator the model doesn't understand (`%`, bitwise ops, …) is still
		// fine if this subexpression references no DECIDE variable: it's a per-row
		// constant coefficient: canonicalization keeps it whole as one atom and the
		// physical layer evaluates it as ordinary data.
		// Only genuinely variable-bearing uses of an unsupported operator are rejected.
		if (!ExpressionContainsDecideVariable(expr, variables)) {
			return true;
		}
		error_msg = StringUtil::Format("Unsupported operator '%s' inside DECIDE SUM expression", func.function_name);
		return false;
	}
	case ExpressionClass::OPERATOR: {
		// COALESCE/IFNULL and other parsed operator nodes are ordinary per-row
		// coefficients when every child is data-only. Keep the same boundary as
		// unsupported FunctionExpression operators above: DuckDB evaluates the
		// opaque data expression, while DECIDE only models expressions that touch a
		// decision variable.
		if (!ExpressionContainsDecideVariable(expr, variables)) {
			return true;
		}
		error_msg = StringUtil::Format(
		    "Unsupported operator expression over a DECIDE variable inside DECIDE SUM expression: %s",
		    expr.ToString());
		return false;
	}
	case ExpressionClass::CAST: {
		auto &cast = expr.Cast<CastExpression>();
		return ValidateSumArgumentInternal(*cast.child, variables, has_decide_variable, error_msg, allow_quadratic);
	}
    case ExpressionClass::SUBQUERY: {
        // Data, unless it reads a decision from inside, which is refused as it binds.
        auto &subquery = expr.Cast<SubqueryExpression>();
        if (subquery.subquery_type != SubqueryType::SCALAR) {
            error_msg = "Subquery in DECIDE SUM expression must be scalar";
            return false;
        }
        return true;
    }
    case ExpressionClass::CASE:
        error_msg = DecideCaseUnsupportedMessage();
        return false;
    case ExpressionClass::DECIDE_FRAME: {
        // A frame term is its payload read at another row, held to the rule of the operand it
        // stands in, and its fallback is read at the anchor row.
        auto &frame = expr.Cast<DecideFrameExpression>();
        for (auto *value : {frame.payload.get(), frame.fallback.get()}) {
            if (value &&
                !ValidateSumArgumentInternal(*value, variables, has_decide_variable, error_msg, allow_quadratic)) {
                return false;
            }
        }
        return true;
    }
    default:
        error_msg = StringUtil::Format("Unsupported expression of type ExpressionClass::%s inside DECIDE SUM expression",
                                       EnumUtil::ToString(expr.GetExpressionClass()));
        return false;
    }
}

bool ValidateSumArgument(ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables, string &error_msg,
                         bool allow_quadratic) {
	bool has_decide_variable = false;
	if (!ValidateSumArgumentInternal(expr, variables, has_decide_variable, error_msg, allow_quadratic)) {
		return false;
	}
	if (!has_decide_variable) {
		error_msg = "SUM expression must reference at least one DECIDE variable";
		return false;
	}
	return true;
}

DecideBinder::DecideBinder(Binder &binder, ClientContext &context, const case_insensitive_map_t<idx_t> &variables,
                           const case_insensitive_set_t &scalar_variables,
                           optional_ptr<DecideQualifierContext> qualifier_context)
    : ExpressionBinder(binder, context), variables(variables), scalar_variables(scalar_variables),
      qualifier_context(qualifier_context) {
    is_top_expression = true;
}

BindResult DecideBinder::PreserveQueryLocation(optional_idx location, BindResult result) {
	// Only fill an empty location: a nested bind that already recorded a more precise
	// node should keep it rather than be widened to its parent's span.
	if (!result.HasError() && result.expression && !result.expression->GetQueryLocation().IsValid()) {
		result.expression->SetQueryLocation(location);
	}
	return result;
}

bool DecideBinder::IsScalarDecideVariable(const ParsedExpression &expr) const {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	const auto &colref = expr.Cast<const ColumnRefExpression>();
	// A decision is referenced by its bare name (the declarations binder refuses `D.x`),
	// so only the bare name can name a query-wide one.
	return !colref.IsQualified() && scalar_variables.count(colref.GetColumnName()) > 0;
}

bool DecideBinder::IsRowInvariantExpression(const ParsedExpression &expr) const {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		if (!IsVariableExpression(expr, variables)) {
			return false; // a plain data column varies per row
		}
		return IsScalarDecideVariable(expr); // row/entity-scoped decisions vary; scalars don't
	}
	bool invariant = true;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (invariant && !IsRowInvariantExpression(child)) {
			invariant = false;
		}
	});
	return invariant;
}

bool DecideBinder::ClassifyReducerCall(FunctionExpression &func, DecideExpression &result, string &error_msg) {
	auto fname = StringUtil::Lower(func.function_name);
	if (fname == "norm") {
		if (func.children.empty()) {
			error_msg = StringUtil::Format("NORM needs an expression and an order, as in norm(e, 1), found '%s'",
			                               func.ToString());
			result = DecideExpression::INVALID;
		} else if (!ValidateSumArgument(*func.children.front(), variables, error_msg, /*allow_quadratic=*/true)) {
			error_msg += ", found '" + func.ToString() + "'";
			result = DecideExpression::INVALID;
		} else {
			result = DecideExpression::SUM;
		}
		return true;
	}
	if (fname == "sum" || fname == "avg" || fname == "min" || fname == "max") {
		// The value reduced is the argument's body; a `when` or `per` in front of it only
		// chooses the rows.
		auto body = func.children.empty()
		                ? nullptr
		                : &const_cast<ParsedExpression &>(ReducerArgumentBody(*func.children.front()));
		if (body && IsRowInvariantExpression(*body)) {
			auto body_text = body->ToString();
			error_msg = StringUtil::Format(
			    "%s is a query-wide decision, so %s(%s) has nothing to aggregate over; "
			    "use %s on its own",
			    body_text, StringUtil::Upper(fname), body_text, body_text);
			result = DecideExpression::INVALID;
		} else if (!body) {
			error_msg = StringUtil::Format("%s needs one argument, found '%s'", StringUtil::Upper(fname),
			                               func.ToString());
			result = DecideExpression::INVALID;
		} else if (!ValidateSumArgument(*body, variables, error_msg, /*allow_quadratic=*/true)) {
			error_msg += ", found '" + func.ToString() + "'";
			result = DecideExpression::INVALID;
		} else {
			result = DecideExpression::SUM;
		}
		return true;
	}
	return false;
}

BindResult DecideBinder::BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth) {
	ErrorData error;

	// DECIDE clauses only accept SUM, AVG, MIN, MAX, and COUNT as aggregates. Reject anything
	// else (STRING_AGG, BIT_AND, MEDIAN, HISTOGRAM, etc.) at the canonical DuckDB hook,
	// mirroring WhereBinder::UnsupportedAggregateMessage. `count_star` is permitted because
	// the symbolic phase synthesizes `count_star()` internally when rewriting SUM(constant).
	if (!IsDecideAggregateName(func.name) && func.name != "count" && func.name != "count_star") {
		return BindResult(BinderException::Unsupported(
		    aggr, StringUtil::Format("DECIDE clause does not support aggregate '%s', only SUM, AVG, MIN, MAX, or COUNT is allowed.",
		                             func.name)));
	}

	// COUNT over a DECIDE variable is degenerate: decision variables are never null, so
	// COUNT(x) is identically the row count and does not constrain x. Reject it with a
	// semantic-specific message rather than silently letting it bind to a no-op constraint.
	if (func.name == "count") {
		for (auto &child : aggr.children) {
			if (ExpressionContainsDecideVariable(*child, variables)) {
				return BindResult(BinderException::Unsupported(
				    aggr, "COUNT over a DECIDE variable is degenerate: decision variables are never null, "
				          "so COUNT(x) always equals the row count. Did you mean SUM(x)?"));
			}
		}
	}

	// No Filter/Distinc allowed for Aggregate
	if (aggr.filter || aggr.distinct) {
        return BindResult(BinderException::Unsupported(aggr, StringUtil::Format("DECIDE clause does not support '%s'", aggr.ToString())));
	}

	// Bind arguments
	vector<unique_ptr<Expression>> children;
	vector<LogicalType> child_types;
	for (auto &child_expr : aggr.children) {
        // Use this->BindExpression to ensure subqueries are handled (executed at bind time)
        auto result = BindExpression(child_expr, depth);
        if (result.HasError()) {
            return result;
        }
        auto &bound_child = result.expression;
		child_types.push_back(bound_child->return_type);
		children.push_back(std::move(bound_child));
	}

	// 2. Bind the aggregate function itself
	FunctionBinder function_binder(binder);
	auto best_function_idx = function_binder.BindFunction(func.name, func.functions, child_types, error);
	if (!best_function_idx.IsValid()) {
		error.AddQueryLocation(aggr);
		return BindResult(std::move(error));
	}
	auto bound_function = func.functions.GetFunctionByOffset(best_function_idx.GetIndex());

	// 3. Create the BoundAggregateExpression itself.
	auto bound_aggregate = function_binder.BindAggregateFunction(bound_function, std::move(children));
	// DECIDE uses aggregate aliases for semantic transport tags (qualified
	// reducers, optimizer markers, provenance). DuckDB's generic binder does not
	// need them here, but dropping one would make the later DECIDE pass blind.
	bound_aggregate->alias = aggr.alias;
	// Note: We are NOT storing this aggregate in a BoundSelectNode. We are returning it directly.
	return BindResult(std::move(bound_aggregate));
}

static BoundAggregateExpression *GetBoundAggregate(Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return &expr.Cast<BoundAggregateExpression>();
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		return GetBoundAggregate(*cast.child);
	}
	return nullptr;
}

BindResult DecideBinder::BindQualifiedReducer(FunctionExpression &qualified_expr, idx_t depth) {
	if (qualified_expr.children.size() < 2) {
		return BindResult(BinderException::Unsupported(
		    qualified_expr, "A qualified reducer expects one or more relations and an expression, as in "
		                    "sum(D: ...) or sum(D, T: ...)."));
	}
	vector<string> relations;
	for (idx_t i = 1; i < qualified_expr.children.size(); i++) {
		auto &qualifier = *qualified_expr.children[i];
		if (qualifier.GetExpressionClass() != ExpressionClass::COLUMN_REF ||
		    qualifier.Cast<ColumnRefExpression>().IsQualified()) {
			return BindResult(BinderException::Unsupported(
			    qualified_expr,
			    "The qualifier of a reducer must be a relation name or alias, as in sum(D: ...)."));
		}
		relations.push_back(qualifier.Cast<ColumnRefExpression>().GetColumnName());
	}
	auto relation_list = StringUtil::Join(relations, ", ");

	if (!qualifier_context) {
		return BindResult(BinderException::Unsupported(
		    qualified_expr, "A relation-qualified reducer is only allowed inside a DECIDE clause."));
	}
	auto &ctx = *qualifier_context;
	KeyColumns key;
	// Resolved as a key's relation is (DataRelation): the decisions' binding is none, and a
	// relation of the FROM clause that shares its name is not ambiguous with it.
	auto decisions = KeyDecisions();
	for (auto &relation : relations) {
		auto binding = DataRelation(binder.bind_context, {relation}, decisions);
		if (!binding) {
			return BindResult(BinderException::Unsupported(
			    qualified_expr, StringUtil::Format(
			                        "Relation '%s' is not in the FROM clause, so a reducer cannot be qualified by it.",
			                        relation)));
		}
		AddRelationColumns(*binding, key);
	}
	// `SUM(D, T: e)` is `sum(per D, T: e)` (syntax_reference.md §5.1): the same exact scope, so the same
	// classes, each split by the body's signature (D65).
	auto scope_idx = FindOrCreateExactScope(SortedKey(std::move(key)), relation_list, *ctx.entity_scopes);

	auto aggregate_result = BindExpression(qualified_expr.children[0], depth);
	if (aggregate_result.HasError()) {
		return aggregate_result;
	}
	auto aggregate_expr = std::move(aggregate_result.expression);
	auto *aggregate = GetBoundAggregate(*aggregate_expr);
	if (!aggregate) {
		return BindResult(BinderException::Unsupported(
		    qualified_expr, StringUtil::Format(
		                        "A relation qualifier is only allowed on SUM, AVG, MIN or MAX; write %s(...) without "
		                        "the '%s:' qualifier.",
		                        StringUtil::Upper(relation_list), relation_list)));
	}
	auto agg_name = StringUtil::Lower(aggregate->function.name);
	if (!IsDecideAggregateName(agg_name)) {
		return BindResult(BinderException::Unsupported(
		    qualified_expr,
		    StringUtil::Format("'%s' cannot be qualified by a relation; only SUM, AVG, MIN and MAX can.",
		                       StringUtil::Upper(agg_name))));
	}
	AddDecideTag(aggregate->alias, MakeQualifiedReducerTag(scope_idx));
	AddDecideTag(aggregate->alias, COLON_FORM_TAG);
	return BindResult(std::move(aggregate_expr));
}

DecideKeyDecisions DecideBinder::KeyDecisions() const {
	DecideKeyDecisions decisions;
	for (auto &variable : variables) {
		decisions.names.insert(variable.first);
	}
	if (qualifier_context) {
		decisions.binding_index = qualifier_context->decide_index;
	}
	return decisions;
}

//! The index of the `by` key equal to `key` as a set -- order and repetition aside --
//! appending it as written when no reducer has used it yet.
static idx_t FindOrCreateGroupKey(vector<unique_ptr<Expression>> key, vector<string> names, const string &text,
                                  vector<DecideGroupKey> &group_keys) {
	vector<unique_ptr<Expression>> unique_key;
	vector<string> unique_names;
	for (idx_t i = 0; i < key.size(); i++) {
		bool seen = false;
		for (auto &kept : unique_key) {
			seen = seen || kept->Equals(*key[i]);
		}
		if (!seen) {
			unique_key.push_back(std::move(key[i]));
			unique_names.push_back(std::move(names[i]));
		}
	}
	auto contains = [](const vector<unique_ptr<Expression>> &set, const Expression &expr) {
		for (auto &member : set) {
			if (member->Equals(expr)) {
				return true;
			}
		}
		return false;
	};
	for (idx_t i = 0; i < group_keys.size(); i++) {
		auto &existing = group_keys[i].expressions;
		bool equal = existing.size() == unique_key.size();
		for (auto &expr : unique_key) {
			equal = equal && contains(existing, *expr);
		}
		// A bound subquery equals nothing, not even itself, so a key holding one is
		// recognised by its text: written the same way in one query, it is the same key.
		if (equal || group_keys[i].text == text) {
			return i;
		}
	}
	DecideGroupKey group_key;
	group_key.expressions = std::move(unique_key);
	group_key.text = text;
	group_key.names = std::move(unique_names);
	group_keys.push_back(std::move(group_key));
	return group_keys.size() - 1;
}

BindResult DecideBinder::BindScopedReducer(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	auto &reducer = expr_ptr->Cast<FunctionExpression>();
	// Peel `[when c] [per K]:` off the argument; the reducer itself reduces the body.
	auto argument = std::move(reducer.children[0]);
	unique_ptr<ParsedExpression> per;
	if (IsDecideMarker(*argument, SCOPE_PER_ROW_TAG)) {
		// `per row`, the default written out: one value per row, as without it.
		auto body = std::move(argument->Cast<FunctionExpression>().children[0]);
		argument = std::move(body);
	} else if (IsDecideMarker(*argument, SCOPE_PER_TAG)) {
		auto body = std::move(argument->Cast<FunctionExpression>().children[0]);
		per = std::move(argument);
		argument = std::move(body);
	}
	unique_ptr<ParsedExpression> condition;
	if (IsDecideMarker(*argument, SCOPE_WHEN_TAG)) {
		auto &when = argument->Cast<FunctionExpression>();
		condition = std::move(when.children[1]);
		auto body = std::move(when.children[0]);
		argument = std::move(body);
	}
	reducer.children[0] = std::move(argument);

	auto result = BindFunction(expr_ptr, depth);
	if (result.HasError()) {
		return result;
	}
	auto *aggregate = GetBoundAggregate(*result.expression);
	if (!aggregate) {
		throw InternalException("DECIDE: a scoped reducer did not bind to an aggregate");
	}
	if (condition) {
		// The condition takes data only, so it binds as plain SQL: a compound condition needs
		// no parentheses.
		auto condition_error = BindData(condition, depth, DecideDataPosition::SCOPE);
		if (condition_error.HasError()) {
			return BindResult(std::move(condition_error));
		}
		aggregate->filter = BoundCastExpression::AddCastToType(
		    context, std::move(BoundExpression::GetExpression(*condition)), LogicalType::BOOLEAN);
	}
	if (per) {
		if (!qualifier_context) {
			throw InternalException("DECIDE: a reducer's per needs the clause's key scopes");
		}
		auto &marker = per->Cast<FunctionExpression>();
		auto scope_idx = FindOrCreateKeyScope(binder.bind_context, marker, KeyDecisions(), DecideKeyToString(marker),
		                                      *qualifier_context->entity_scopes);
		AddDecideTag(aggregate->alias, MakeQualifiedReducerTag(scope_idx));
	}
	return result;
}

BindResult DecideBinder::BindReducerBy(FunctionExpression &by, idx_t depth) {
	// Read before the key elements bind in place.
	auto key_text = DecideKeyToString(by);
	auto result = BindExpression(by.children[0], depth);
	if (result.HasError()) {
		return result;
	}
	auto *aggregate = GetBoundAggregate(*result.expression);
	if (!aggregate) {
		throw InternalException("DECIDE: a reducer with by did not bind to an aggregate");
	}
	if (by.children.size() == 1) {
		return result; // `by ()`: every kept row, the default
	}
	if (!qualifier_context || !qualifier_context->group_keys) {
		throw InternalException("DECIDE: a reducer's by needs the clause's group keys");
	}
	// Each element is a data expression, or a relation standing for its stored columns.
	vector<unique_ptr<Expression>> key;
	vector<string> names;
	for (idx_t i = 1; i < by.children.size(); i++) {
		auto &element = by.children[i];
		if (element->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto relation =
			    ResolveRelationName(binder.bind_context, element->Cast<ColumnRefExpression>(), KeyDecisions());
			if (relation) {
				for (auto &column : BindRelationColumns(*relation)) {
					names.push_back(column->GetName());
					key.push_back(std::move(column));
				}
				continue;
			}
		}
		names.push_back(element->ToString());
		auto key_error = BindData(element, depth, DecideDataPosition::SCOPE);
		if (key_error.HasError()) {
			return BindResult(std::move(key_error));
		}
		key.push_back(std::move(BoundExpression::GetExpression(*element)));
	}
	auto group_key = FindOrCreateGroupKey(std::move(key), std::move(names), key_text, *qualifier_context->group_keys);
	AddDecideTag(aggregate->alias, MakeGroupKeyTag(group_key));
	return result;
}

ErrorData DecideBinder::BindFrameFallback(DecideFrameExpression &frame, const DecideFrameSpec &spec,
                                          const string &fallback_text, idx_t depth, unique_ptr<Expression> &out) {
	if (!frame.fallback) {
		return ErrorData();
	}
	// The fallback stands in for the payload at a missing position: a number, as the
	// payload is a number to the model.
	ErrorData fallback_error;
	BindChild(frame.fallback, depth, fallback_error);
	if (fallback_error.HasError()) {
		return fallback_error;
	}
	auto fallback = std::move(BoundExpression::GetExpression(*frame.fallback));
	auto &type = fallback->return_type;
	if (!type.IsNumeric() && type.id() != LogicalTypeId::BOOLEAN && type.id() != LogicalTypeId::SQLNULL) {
		throw BinderException(frame, "DECIDE: in %s, else %s is not a number", spec.text, fallback_text);
	}
	// A fallback that is NULL before the query runs is the default, `else NULL` (D80): a
	// missing position selects nothing.
	Value value;
	if (fallback->IsFoldable() && ExpressionExecutor::TryEvaluateScalar(context, *fallback, value) &&
	    value.IsNull()) {
		return ErrorData();
	}
	out = std::move(fallback);
	return ErrorData();
}

unique_ptr<Expression> DecideBinder::BindFrameAggregate(const string &name, unique_ptr<Expression> payload,
                                                        const DecideFrameExpression &frame, const string &alias,
                                                        ErrorData &error) {
	auto &entry = Catalog::GetEntry<AggregateFunctionCatalogEntry>(context, SYSTEM_CATALOG, DEFAULT_SCHEMA, name);
	FunctionBinder function_binder(binder);
	auto offset = function_binder.BindFunction(entry.name, entry.functions, {payload->return_type}, error);
	if (!offset.IsValid()) {
		error.AddQueryLocation(frame);
		return nullptr;
	}
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(payload));
	auto result =
	    function_binder.BindAggregateFunction(entry.functions.GetFunctionByOffset(offset.GetIndex()), std::move(children));
	result->SetAlias(alias);
	return std::move(result);
}

BindResult DecideBinder::BindDecideFrame(DecideFrameExpression &frame, idx_t depth) {
	if (!qualifier_context || !qualifier_context->frames) {
		throw InternalException("DECIDE: a frame selector needs the clause's frames");
	}
	// Read before the parts bind in place. ValidateDecideScopeSpellings has already held the
	// order key and the within key to data, and the payload and fallback to one row each.
	DecideFrameSpec spec;
	spec.range = frame.IsRange();
	spec.from = frame.from;
	spec.from_k = frame.from_k;
	spec.to = frame.to;
	spec.to_k = frame.to_k;
	spec.every = frame.every;
	spec.all = frame.all;
	spec.descending = frame.descending;
	spec.cyclic = frame.cyclic;
	spec.text = frame.ToString();
	spec.order_name = frame.order->ToString();
	auto fallback_text = frame.fallback ? frame.fallback->ToString() : string();

	// The payload, under the aggregate that reads it at the selected rows: the range's
	// reducer, else `first`, which reads the one row a point selects.
	unique_ptr<Expression> result;
	if (frame.IsRange()) {
		auto name = StringUtil::Lower(frame.reducer);
		if (!IsDecideAggregateName(name) && name != "count") {
			throw BinderException(frame, "DECIDE: a range frame reduces with sum, avg, min, max or count, found %s",
			                      frame.reducer);
		}
		spec.reducer = name;
		vector<unique_ptr<ParsedExpression>> arguments;
		arguments.push_back(std::move(frame.payload));
		unique_ptr<ParsedExpression> call = make_uniq<FunctionExpression>(name, std::move(arguments));
		call->SetQueryLocation(frame.GetQueryLocation());
		auto bound = BindFunction(call, depth);
		if (bound.HasError()) {
			return bound;
		}
		result = std::move(bound.expression);
		// A missing position's `else` is one of the values the reducer reads, so payload and
		// fallback take a type that holds both, as a point's do: `else 0.5` over a
		// whole-numbered payload sums 0.5 there. The reducer is bound again over it.
		unique_ptr<Expression> fallback;
		auto fallback_error = BindFrameFallback(frame, spec, fallback_text, depth, fallback);
		if (fallback_error.HasError()) {
			return BindResult(std::move(fallback_error));
		}
		if (fallback) {
			auto *reducer = GetBoundAggregate(*result);
			if (!reducer || reducer->children.size() != 1) {
				throw InternalException("DECIDE: a range frame did not bind to an aggregate over its payload");
			}
			auto &payload_type = reducer->children[0]->return_type;
			LogicalType common;
			if (!LogicalType::TryGetMaxLogicalType(context, payload_type, fallback->return_type, common)) {
				throw BinderException(frame, "DECIDE: in %s, else %s is not a number", spec.text, fallback_text);
			}
			if (payload_type != common) {
				auto payload = BoundCastExpression::AddCastToType(context, std::move(reducer->children[0]), common);
				ErrorData reducer_error;
				result = BindFrameAggregate(name, std::move(payload), frame, reducer->GetAlias(), reducer_error);
				if (!result) {
					return BindResult(std::move(reducer_error));
				}
			}
			// The type the bound reducer reads, which its overload may have widened the payload
			// to (`sum` of a FLOAT reads a DOUBLE): the fallback is one more value of it.
			auto &argument_type = GetBoundAggregate(*result)->children[0]->return_type;
			spec.fallback = BoundCastExpression::AddCastToType(context, std::move(fallback), argument_type);
		}
	} else {
		ErrorData payload_error;
		BindChild(frame.payload, depth, payload_error);
		if (payload_error.HasError()) {
			return BindResult(std::move(payload_error));
		}
		vector<unique_ptr<Expression>> children;
		children.push_back(std::move(BoundExpression::GetExpression(*frame.payload)));
		// The point reads its payload where it selects a row and its fallback where it does
		// not, so its value takes a type that holds both: `else 0.5` over a whole-numbered
		// payload is 0.5 there. The fallback is bound first for that reason.
		unique_ptr<Expression> fallback;
		auto fallback_error = BindFrameFallback(frame, spec, fallback_text, depth, fallback);
		if (fallback_error.HasError()) {
			return BindResult(std::move(fallback_error));
		}
		if (fallback) {
			auto &payload_type = children[0]->return_type;
			LogicalType common;
			if (!LogicalType::TryGetMaxLogicalType(context, payload_type, fallback->return_type, common)) {
				throw BinderException(frame, "DECIDE: in %s, else %s is not a number", spec.text, fallback_text);
			}
			children[0] = BoundCastExpression::AddCastToType(context, std::move(children[0]), common);
			spec.fallback = BoundCastExpression::AddCastToType(context, std::move(fallback), common);
		}
		ErrorData first_error;
		result = BindFrameAggregate("first", std::move(children[0]), frame, string(), first_error);
		if (!result) {
			return BindResult(std::move(first_error));
		}
	}
	auto *aggregate = GetBoundAggregate(*result);
	if (!aggregate || aggregate->children.size() != 1) {
		throw InternalException("DECIDE: a frame selector did not bind to an aggregate over its payload");
	}

	// The order key binds as plain SQL, as a `when` condition does.
	auto order_error = BindData(frame.order, depth, DecideDataPosition::FRAME_KEY);
	if (order_error.HasError()) {
		return BindResult(std::move(order_error));
	}
	spec.order = std::move(BoundExpression::GetExpression(*frame.order));
	// The within key resolves as a per key does: columns, and relations standing for theirs.
	// Messages name both as written (DecideFrameSpec::timeline_names, order_name).
	for (auto &column :
	     ResolveKey(binder.bind_context, frame.within, 0, KeyDecisions(), DecideDataPosition::FRAME_KEY)) {
		spec.within.push_back(make_uniq<BoundColumnRefExpression>(column.name, column.type, column.binding));
		spec.timeline_names.push_back(column.name);
	}

	// The aggregate prints as the frame written, a source fragment of its own. Tagged here
	// rather than by TagDecideSourceFragments: ExpressionBinder::Bind gives a bound node
	// the alias its parsed node had before binding, which would drop the frame's tag.
	if (qualifier_context->source_fragments) {
		auto &fragments = *qualifier_context->source_fragments;
		AddDecideTag(aggregate->alias, MakeSourceFragmentTag(fragments.size()));
		fragments.push_back(spec.text);
	}
	auto &frames = *qualifier_context->frames;
	AddDecideTag(aggregate->alias, MakeFrameTag(frames.size()));
	frames.push_back(std::move(spec));
	return BindResult(std::move(result));
}

BindResult DecideBinder::BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &function = expr.Cast<FunctionExpression>();
    if (function.is_operator && function.function_name == QUALIFIED_REDUCER_TAG) {
        return BindQualifiedReducer(function, depth);
    }
    // `by (key)` and a scope in front of a reducer's argument. ValidateDecideScopeSpellings
    // has already refused both on anything but SUM, AVG, MIN and MAX.
    if (function.is_operator && function.function_name == REDUCER_BY_TAG) {
        return BindReducerBy(function, depth);
    }
    if (!function.is_operator && !function.children.empty() && IsDecideScopeMarker(*function.children[0])) {
        return BindScopedReducer(expr_ptr, depth);
    }
    // NORM is DECIDE syntax, not a catalog function. Bind it as a deliberately
    // inert SUM aggregate marker; DecideOptimizer owns the mathematical rewrite
    // and replaces this marker before it can reach physical planning. Keeping an
    // aggregate-shaped marker is important: a `by (G)` after it uses the same
    // bound-tree contract as on any other reducer.
    if (!function.is_operator && StringUtil::CIEquals(function.function_name, "norm")) {
        if (function.children.size() < 2 || function.children.size() > 3) {
            return BindResult(BinderException::Unsupported(
                function, "norm(expr, p) takes an expression, an order (1, 2, 0, or 'inf'), and optional M for L0."));
        }
        if (function.children[1]->GetExpressionClass() != ExpressionClass::CONSTANT) {
            return BindResult(BinderException::Unsupported(
                function, "The order p in norm(expr, p) must be a constant (1, 2, 0, or 'inf')."));
        }
        auto &order_value = function.children[1]->Cast<ConstantExpression>().value;
        string payload;
        if (order_value.type().id() == LogicalTypeId::VARCHAR) {
            auto order = StringUtil::Lower(StringValue::Get(order_value));
            if (order != "inf" && order != "infinity" && order != "max") {
                return BindResult(BinderException::Unsupported(function,
                    StringUtil::Format("Unsupported norm order '%s'. Use 0, 1, 2, or 'inf'.", order)));
            }
            if (function.children.size() != 2) {
                return BindResult(BinderException::Unsupported(function, "norm(expr, 'inf') does not take M."));
            }
            payload = "inf";
        } else if (order_value.type().IsNumeric()) {
            double order = order_value.GetValue<double>();
            if (order == 1.0) {
                if (function.children.size() != 2) return BindResult(BinderException::Unsupported(function, "Only norm(expr, 0, M) accepts M."));
                payload = "1";
            } else if (order == 2.0) {
                if (function.children.size() != 2) return BindResult(BinderException::Unsupported(function, "Only norm(expr, 0, M) accepts M."));
                payload = "2";
            } else if (order == 0.0) {
                if (function.children.size() == 2) {
                    payload = "0_auto";
                } else {
                    if (function.children[2]->GetExpressionClass() != ExpressionClass::CONSTANT) {
                        return BindResult(BinderException::Unsupported(function,
                            "norm(expr, 0, M): the bound M must be a constant. Omit it to infer M from the data, or pass a positive literal."));
                    }
                    auto m = function.children[2]->Cast<ConstantExpression>().value.GetValue<double>();
                    if (!(m > 0.0)) {
                        return BindResult(BinderException::Unsupported(function,
                            "The L0 bound M in norm(expr, 0, M) must be a positive constant."));
                    }
                    payload = "0_" + StringUtil::Format("%.17g", m);
                }
            } else {
                return BindResult(BinderException::Unsupported(function,
                    StringUtil::Format("Unsupported norm order %g. Supported: 0, 1, 2, or 'inf'.", order)));
            }
        } else {
            return BindResult(BinderException::Unsupported(function,
                "The order p in norm(expr, p) must be 0, 1, 2, or 'inf'."));
        }
        vector<unique_ptr<ParsedExpression>> marker_children;
        marker_children.push_back(std::move(function.children[0]));
        auto marker = make_uniq<FunctionExpression>("sum", std::move(marker_children));
        marker->alias = function.alias;
        AddDecideTag(marker->alias, string(NORM_MARKER_TAG_PREFIX) + payload + "__");
        expr_ptr = std::move(marker);
        return BindFunction(expr_ptr, depth);
    }
    // Check if this is an aggregate function
    QueryErrorContext error_context(expr_ptr->GetQueryLocation());
    auto &catalog_entry = *GetCatalogEntry(CatalogType::SCALAR_FUNCTION_ENTRY, function.catalog, function.schema, function.function_name, OnEntryNotFound::THROW_EXCEPTION, error_context);
    
    if (catalog_entry.type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
        // It's an aggregate function, bind it using our custom aggregate logic
        return BindAggregate(function, catalog_entry.Cast<AggregateFunctionCatalogEntry>(), depth);
    }
    // It's a scalar function, bind it normally. A NULL argument makes it NULL, and DuckDB
    // folds it to that constant: the decisions it read are gone, so it is refused here,
    // where the SQL is still at hand.
    bool reads_decision = ExpressionContainsDecideVariable(function, variables);
    auto text = reads_decision ? function.ToString() : string();
    auto result = ExpressionBinder::BindExpression(expr_ptr, depth);
    if (reads_decision && !result.HasError() &&
        result.expression->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
        result.expression->Cast<BoundConstantExpression>().value.IsNull()) {
        throw BinderException(function,
                              "DECIDE: '%s' is NULL whatever the decisions are; replace the NULL with a value, as in "
                              "COALESCE(column, 0)",
                              text);
    }
    return result;
}

DecideDecisionRead FindDecisionRead(const Expression &expr, idx_t decide_index) {
	DecideDecisionRead read;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (colref.binding.table_index == decide_index) {
			read.node = &expr;
			read.column = colref.binding.column_index;
		}
		return read;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		// A subquery's body has its own binder, which lists every column of the enclosing
		// query read anywhere inside it as correlated.
		for (auto &column : expr.Cast<BoundSubqueryExpression>().binder->correlated_columns) {
			if (column.binding.table_index == decide_index) {
				read.node = &expr;
				read.column = column.binding.column_index;
				return read;
			}
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!read.node) {
			read = FindDecisionRead(child, decide_index);
		}
	});
	return read;
}

ErrorData DecideBinder::BindData(unique_ptr<ParsedExpression> &expr, idx_t depth, DecideDataPosition position) {
	auto outer = binding_data;
	binding_data = position;
	ErrorData error;
	try {
		BindChild(expr, depth, error);
	} catch (...) {
		binding_data = outer;
		throw;
	}
	binding_data = outer;
	return error;
}

void DecideBinder::RefuseDecisionReference(const ParsedExpression &parsed, const Expression &bound, idx_t depth) const {
	if (!qualifier_context) {
		return;
	}
	auto read = FindDecisionRead(bound, qualifier_context->decide_index);
	if (!read.node) {
		return;
	}
	string name;
	for (auto &variable : variables) {
		if (variable.second == read.column) {
			name = variable.first;
		}
	}
	if (binding_data != DecideDataPosition::NONE) {
		throw BinderException(parsed, DecideDecisionInDataFormat(binding_data), name);
	}
	// From inside a subquery: the column of a subquery is bound past it, at this binder's depth.
	if (depth > 0) {
		throw BinderException(parsed, "DECIDE: %s is a decision; a subquery in a DECIDE clause may only use data",
		                      name);
	}
	if (IsVariableExpression(parsed, variables)) {
		return;
	}
	auto &colref = parsed.Cast<ColumnRefExpression>();
	if (colref.IsQualified() && variables.count(colref.GetColumnName())) {
		throw BinderException(parsed, "DECIDE: write %s, not %s; a decision is referenced by its bare name",
		                      colref.GetColumnName(), colref.ToString());
	}
	// The decisions' binding itself, read as a struct of them.
	throw BinderException(parsed, "DECIDE: %s is neither a column nor a relation of the FROM clause",
	                      colref.ToString());
}

BindResult DecideBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	// Read before dispatching: BindFunction rewrites `expr_ptr` in place for the norm marker,
	// so the node it names afterwards is not the one the user wrote. A column reference binds
	// without replacing its node.
	auto location = expr_ptr->GetQueryLocation();
	// A reference SQL expands binds the references it expands to through here as well --
	// `decide_variables`, a struct of the decisions, binds `decide_variables.x` -- and the
	// rule judges it as written, once.
	bool column_reference = expr_ptr->GetExpressionClass() == ExpressionClass::COLUMN_REF && !binding_reference;
	auto outer_reference = binding_reference;
	binding_reference = binding_reference || column_reference;
	BindResult result;
	try {
		result = binding_data == DecideDataPosition::NONE ? BindNode(expr_ptr, depth, root_expression)
		                                                  : ExpressionBinder::BindExpression(expr_ptr, depth);
	} catch (...) {
		binding_reference = outer_reference;
		throw;
	}
	binding_reference = outer_reference;
	if (!result.HasError() && result.expression) {
		if (column_reference) {
			RefuseDecisionReference(*expr_ptr, *result.expression, depth);
		}
		PreserveDecideSourceFragment(*expr_ptr, *result.expression);
	}
	return PreserveQueryLocation(location, std::move(result));
}

BindResult DecideBinder::BindNode(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	if (depth > 0) {
		return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
	}
	auto &expr = *expr_ptr;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::FUNCTION:
		return BindFunction(expr_ptr, depth);
	case ExpressionClass::DECIDE_FRAME:
		return BindDecideFrame(expr.Cast<DecideFrameExpression>(), depth);
	case ExpressionClass::SUBQUERY: {
		auto &subquery = expr.Cast<SubqueryExpression>();
		if (subquery.subquery_type != SubqueryType::SCALAR) {
			return BindResult(BinderException::Unsupported(expr, "Only scalar subqueries are supported in DECIDE"));
		}
		// Standard binding handles both correlated and uncorrelated scalar subqueries.
		// Uncorrelated: PlanSubqueries evaluates them as cross-joined scalars.
		// Correlated: PlanSubqueries decorrelates them into joins, producing per-row values
		// that the DECIDE operator evaluates normally. One that reads a decision is refused as
		// that column binds, past the subquery, at this binder (BindExpression).
		return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
	}
	default:
		return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
	}
}

} // namespace duckdb
