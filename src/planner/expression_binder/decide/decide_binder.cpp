#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/table_binding.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include <unordered_set>
#include <algorithm>
#include <cmath>

#include "duckdb/main/client_context.hpp"

namespace duckdb {

bool IsVariableExpression(const ParsedExpression &expr, const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	const auto &colref = expr.Cast<const ColumnRefExpression>();
	if (colref.IsQualified()) {
		string qualified = colref.GetTableName() + "." + colref.GetColumnName();
		return variables.count(qualified) > 0;
	}
	return variables.count(colref.GetColumnName()) > 0;
}

static idx_t CountDecideVariableOccurrencesInternal(const ParsedExpression &expr,
                                                    const case_insensitive_map_t<idx_t> &variables) {
	idx_t count = 0;
	if (IsVariableExpression(expr, variables)) {
		count++;
	}
	// Descend into subquery QueryNode bodies (SELECT, WHERE, HAVING, etc.)
	// EnumerateChildren only visits SubqueryExpression.child, not the query body.
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery_expr = expr.Cast<const SubqueryExpression>();
		if (subquery_expr.subquery && subquery_expr.subquery->node) {
			// const_cast: EnumerateQueryNodeChildren requires non-const but we only read
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    const_cast<QueryNode &>(*subquery_expr.subquery->node),
			    [&](unique_ptr<ParsedExpression> &child) {
				    count += CountDecideVariableOccurrencesInternal(*child, variables);
			    },
			    [&](TableRef &ref) {});
		}
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

	// ParsedExpressionIterator::EnumerateChildren does not enter the QueryNode of
	// a scalar subquery. Walk it explicitly so a correlated decision reference
	// cannot hide an explicit cast inside the subquery body.
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery_expr = expr.Cast<const SubqueryExpression>();
		if (subquery_expr.subquery && subquery_expr.subquery->node) {
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    const_cast<QueryNode &>(*subquery_expr.subquery->node),
			    [&](unique_ptr<ParsedExpression> &child) {
				    ValidateDecideNoExplicitDecisionCasts(*child, variables);
			    },
			    [&](TableRef &ref) {});
		}
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		ValidateDecideNoExplicitDecisionCasts(child, variables);
	});
}

bool IsDecideAggregateName(const string &name) {
	auto lname = StringUtil::Lower(name);
	return lname == "sum" || lname == "avg" || lname == "min" || lname == "max";
}

idx_t FindOrCreateEntityScope(BindContext &bind_context, const string &table_name,
                              vector<EntityScopeInfo> &entity_scopes,
                              case_insensitive_map_t<idx_t> &table_scope_map) {
	return FindOrCreateEntityScope(bind_context, vector<string> {table_name}, entity_scopes, table_scope_map);
}

idx_t FindOrCreateEntityScope(BindContext &bind_context, const vector<string> &table_names,
                              vector<EntityScopeInfo> &entity_scopes,
                              case_insensitive_map_t<idx_t> &table_scope_map) {
	D_ASSERT(!table_names.empty());
	// Canonicalize order so sum(D,T: ...) and sum(T,D: ...) share one scope: the
	// composite identity is a set, not a sequence.
	auto sorted_names = table_names;
	std::sort(sorted_names.begin(), sorted_names.end(), [](const string &a, const string &b) {
		return StringUtil::Lower(a) < StringUtil::Lower(b);
	});
	string cache_key = StringUtil::Join(sorted_names, ",");
	auto scope_it = table_scope_map.find(cache_key);
	if (scope_it != table_scope_map.end()) {
		return scope_it->second;
	}
	EntityScopeInfo scope_info;
	scope_info.table_alias = cache_key;
	for (auto &table_name : sorted_names) {
		ErrorData error;
		auto binding = bind_context.GetBinding(table_name, error);
		D_ASSERT(binding); // callers resolve the name first so they can word their own error
		scope_info.source_table_indices.push_back(binding->index);
		// Register every source-table column via GetColumnBinding. This forces the
		// columns into the scan's column_ids and returns the correct ColumnBinding
		// (table_index, position-in-col_ids).
		if (binding->binding_type == BindingType::TABLE) {
			auto &tbl_binding = binding->Cast<TableBinding>();
			for (idx_t col_idx = 0; col_idx < binding->names.size(); col_idx++) {
				scope_info.entity_key_column_types.push_back(binding->types[col_idx]);
				scope_info.entity_key_bindings.push_back(tbl_binding.GetColumnBinding(col_idx));
			}
		} else {
			// Non-base table (e.g., subquery): fall back to raw bindings.
			for (idx_t col_idx = 0; col_idx < binding->names.size(); col_idx++) {
				scope_info.entity_key_column_types.push_back(binding->types[col_idx]);
				scope_info.entity_key_bindings.push_back(ColumnBinding(binding->index, col_idx));
			}
		}
	}
	idx_t scope_idx = entity_scopes.size();
	table_scope_map.emplace(cache_key, scope_idx);
	entity_scopes.push_back(std::move(scope_info));
	return scope_idx;
}

const ParsedExpression &UnwrapQualifiedReducer(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return expr;
	}
	auto &func = expr.Cast<const FunctionExpression>();
	if (func.is_operator && func.function_name == QUALIFIED_REDUCER_TAG && !func.children.empty()) {
		return *func.children[0];
	}
	return expr;
}

bool ContainsDecideAggregate(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (!func.is_operator && (IsDecideAggregateName(func.function_name) || StringUtil::CIEquals(func.function_name, "norm"))) {
			return true;
		}
		if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG && !func.children.empty()) {
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

bool ContainsWhenOperator(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<const FunctionExpression>();
		if (func.is_operator && func.function_name == WHEN_CONSTRAINT_TAG) {
			return true;
		}
		for (auto &child : func.children) {
			if (ContainsWhenOperator(*child)) {
				return true;
			}
		}
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::OPERATOR) {
		auto &op = expr.Cast<const OperatorExpression>();
		for (auto &child : op.children) {
			if (ContainsWhenOperator(*child)) {
				return true;
			}
		}
		return false;
	}
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		auto &cast = expr.Cast<const CastExpression>();
		return ContainsWhenOperator(*cast.child);
	}
	if (expr.GetExpressionClass() == ExpressionClass::COMPARISON) {
		auto &cmp = expr.Cast<const ComparisonExpression>();
		return ContainsWhenOperator(*cmp.left) || ContainsWhenOperator(*cmp.right);
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
//! user spelled it, or empty if there is none. Walks scalar-subquery bodies for the
//! same reason ValidateDecideNoExplicitDecisionCasts does: a correlated decision
//! reference can sit inside one, and EnumerateChildren does not enter a QueryNode.
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
		string key = colref.IsQualified()
		    ? (colref.GetTableName() + "." + colref.GetColumnName())
		    : colref.GetColumnName();
		auto it = variables.find(key);
		if (it != variables.end() && it->second < variable_types.size()) {
			const auto &type = variable_types[it->second];
			if (type == LogicalType::DOUBLE || type == LogicalType::FLOAT) {
				return colref.GetColumnName();
			}
		}
	}

	string found;
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery_expr = expr.Cast<const SubqueryExpression>();
		if (subquery_expr.subquery && subquery_expr.subquery->node) {
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    const_cast<QueryNode &>(*subquery_expr.subquery->node),
			    [&](unique_ptr<ParsedExpression> &child) {
				    if (found.empty()) {
					    found = FindRealDecideVariable(*child, variables, variable_types);
				    }
			    },
			    [&](TableRef &ref) {});
		}
	}
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
	// nested inside an operand is a boolean value, not a constraint: in the misparse
	// `(SUM(x) WHEN w > 1) + 3 <= 10` the `> 1` is added to 3, and the type error
	// that names is a better diagnosis than anything said about strictness. So this
	// descends the way DecideConstraintsBinder does — through conjunctions and
	// through the constraint child of a WHEN / PER wrapper — and no further.
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
		if (func.is_operator && !func.children.empty() &&
		    (func.function_name == WHEN_CONSTRAINT_TAG || func.function_name == PER_CONSTRAINT_TAG)) {
			// children[0] is the constraint; the rest are the condition / PER columns.
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

//! Is this bound node an AVG aggregate?
static bool IsBoundAvgAggregate(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	auto &agg = expr.Cast<const BoundAggregateExpression>();
	return StringUtil::CIEquals(agg.function.name, "avg");
}

//! Is `expr` provably whole-numbered from declared types alone?
//!
//! `allow_avg_hoist` is set only for `<>`, whose rewrite multiplies both sides by the
//! group size and so compares `SUM(e)` against `K*n` — integral whenever `e` is. A
//! strict comparison has no such hoist and keeps AVG's fractional 1/n coefficients.
static bool DecideSideIsWholeNumbered(const Expression &expr, bool allow_avg_hoist,
                                      idx_t decide_index) {
	if (IsBoundL0NormMarker(expr)) {
		return true;
	}
	if (allow_avg_hoist && IsBoundAvgAggregate(expr)) {
		auto &agg = expr.Cast<const BoundAggregateExpression>();
		return agg.children.size() == 1 &&
		       DecideSideIsWholeNumbered(*agg.children[0], false, decide_index);
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CAST: {
		// A cast to a whole-numbered type makes the value one; a widening cast of an
		// already-whole operand (the binder inserts many, e.g. INTEGER -> DECIMAL(18,0))
		// keeps it one. Either direction suffices.
		auto &cast = expr.Cast<const BoundCastExpression>();
		return DecideTypeIsWholeNumbered(cast.return_type) ||
		       DecideSideIsWholeNumbered(*cast.child, allow_avg_hoist, decide_index);
	}
	case ExpressionClass::BOUND_AGGREGATE: {
		// SUM / MIN / MAX of whole numbers are whole. AVG is handled above; anything
		// else falls through to its declared return type.
		auto &agg = expr.Cast<const BoundAggregateExpression>();
		const auto &name = agg.function.name;
		if ((StringUtil::CIEquals(name, "sum") || StringUtil::CIEquals(name, "min") ||
		     StringUtil::CIEquals(name, "max")) &&
		    agg.children.size() == 1) {
			return DecideSideIsWholeNumbered(*agg.children[0], false, decide_index);
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
				if (!SideReferencesDecision(*child, decide_index)) {
					continue;
				}
				if (!DecideSideIsWholeNumbered(*child, allow_avg_hoist, decide_index)) {
					return false;
				}
			}
			return true;
		}
		if (name == "*") {
			// Every factor matters here, decision-bearing or not: a fractional
			// multiplier rescales the decision off the lattice (`0.5 * x`).
			for (const auto &child : func.children) {
				if (!DecideSideIsWholeNumbered(*child, allow_avg_hoist, decide_index)) {
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
				return DecideSideIsWholeNumbered(*func.children[0], allow_avg_hoist, decide_index);
			}
			return false;
		}
		return DecideTypeIsWholeNumbered(expr.return_type);
	}
	default:
		return DecideTypeIsWholeNumbered(expr.return_type);
	}
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
static bool FindFractionalOperand(const Expression &expr, bool allow_avg_hoist,
                                  idx_t decide_index, FractionalOperand &out) {
	if (DecideSideIsWholeNumbered(expr, allow_avg_hoist, decide_index)) {
		return false;
	}
	// Prefer a named leaf over the composite node above it: `l_quantity` is actionable,
	// `sum((x * l_quantity))` is not.
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = FindFractionalOperand(child, allow_avg_hoist, decide_index, out);
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
	// No fractional leaf underneath: the node itself is what produces fractions.
	out.name = expr.GetName();
	out.type = expr.return_type;
	out.is_castable_column = false;
	return true;
}

void ValidateDecideIntegralComparisonOperands(const Expression &expr, idx_t decide_index) {
	// Constraint position only, and by the same reasoning as the parsed-tree validator:
	// a comparison nested inside an operand is a boolean value, not a model row.
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<const BoundConjunctionExpression>();
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) || IsPerConstraintTag(conj.alias)) {
			if (!conj.children.empty()) {
				ValidateDecideIntegralComparisonOperands(*conj.children[0], decide_index);
			}
			return;
		}
		for (const auto &child : conj.children) {
			ValidateDecideIntegralComparisonOperands(*child, decide_index);
		}
		return;
	}
	case ExpressionClass::BOUND_COMPARISON: {
		auto type = expr.GetExpressionType();
		bool is_strict =
		    type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_GREATERTHAN;
		bool is_not_equal = type == ExpressionType::COMPARE_NOTEQUAL;
		if (!is_strict && !is_not_equal) {
			return;
		}
		auto &comp = expr.Cast<const BoundComparisonExpression>();
		// Only a side that carries decisions is a compared quantity; the other is the
		// bound, and a fractional bound is a tautology rather than an error.
		bool left_is_compared = SideReferencesDecision(*comp.left, decide_index);
		bool right_is_compared = SideReferencesDecision(*comp.right, decide_index);
		FractionalOperand bad;
		if (!(left_is_compared && FindFractionalOperand(*comp.left, is_not_equal, decide_index, bad)) &&
		    !(right_is_compared && FindFractionalOperand(*comp.right, is_not_equal, decide_index, bad))) {
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
	}
	default:
		return;
	}
}

const char *DecideCaseUnsupportedMessage() {
	return "CASE expressions are not supported inside DECIDE constraints or "
	       "objectives. Use postfix WHEN to gate on a row predicate "
	       "(e.g. `SUM(x) >= 1 WHEN category = 'A'`), PER to partition by "
	       "a column, or a CTE/subquery to pre-compute conditional values "
	       "before the DECIDE clause.";
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
				// Nested aggregates (e.g., SUM(MAX(expr)) for PER objectives) are allowed.
				// The optimizer will detect and rewrite them.
				if (func.children.size() == 1 && ExpressionContainsDecideVariable(*func.children[0], variables)) {
					has_decide_variable = true;
					return true;
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
        auto &subquery = expr.Cast<SubqueryExpression>();
        if (subquery.subquery_type != SubqueryType::SCALAR) {
            error_msg = "Subquery in DECIDE SUM expression must be scalar";
            return false;
        }
        if (ExpressionContainsDecideVariable(expr, variables)) {
            error_msg = "Subquery in DECIDE SUM expression cannot contain DECIDE variables";
            return false;
        }
        return true;
    }
    case ExpressionClass::CASE:
        error_msg = DecideCaseUnsupportedMessage();
        return false;
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
	// A scalar is never table-qualified (the grammar rejects that spelling), so
	// only the bare name can name one.
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
		if (func.children.empty() ||
		    !ValidateSumArgument(*func.children.front(), variables, error_msg, /*allow_quadratic=*/true)) {
			error_msg += ", found '" + func.ToString() + "'";
			result = DecideExpression::INVALID;
		} else {
			result = DecideExpression::SUM;
		}
		return true;
	}
	if (fname == "sum" || fname == "avg" || fname == "min" || fname == "max") {
		if (!func.children.empty() && IsRowInvariantExpression(*func.children.front())) {
			auto body_text = func.children.front()->ToString();
			error_msg = StringUtil::Format(
			    "%s is a query-wide decision, so %s(%s) has nothing to aggregate over; "
			    "use %s on its own",
			    body_text, StringUtil::Upper(fname), body_text, body_text);
			result = DecideExpression::INVALID;
		} else if (!ValidateSumArgument(*func.children.front(), variables, error_msg, /*allow_quadratic=*/true)) {
			error_msg += ", found '" + func.ToString() + "'";
			result = DecideExpression::INVALID;
		} else {
			result = DecideExpression::SUM;
		}
		return true;
	}
	return false;
}

BindResult DecideBinder::BindPerWrapper(FunctionExpression &func, idx_t depth) {
	// Bind the inner constraint/objective (child[0]) through the subclass's own dispatch.
	is_top_expression = true;
	ErrorData inner_error;
	BindChild(func.children[0], depth, inner_error);
	if (inner_error.HasError()) {
		return BindResult(std::move(inner_error));
	}

	// Bind each PER column using the base ExpressionBinder (reuse binding_when_condition
	// to bypass DECIDE-specific dispatch: a PER column is a plain table column).
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
	// child[0] = bound constraint/objective (possibly WHEN-wrapped)
	// children[1..N] = bound PER columns (BoundColumnRefExpression)
	auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	result->children.push_back(std::move(BoundExpression::GetExpression(*func.children[0])));
	for (idx_t i = 1; i < func.children.size(); i++) {
		result->children.push_back(std::move(BoundExpression::GetExpression(*func.children[i])));
	}
	result->alias = func.function_name; // preserves PER_CONSTRAINT_TAG
	return BindResult(std::move(result));
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

BindResult DecideBinder::BindLocalWhenAggregate(FunctionExpression &when_expr, idx_t depth) {
	if (when_expr.children.size() != 2) {
		return BindResult(BinderException::Unsupported(
		    when_expr, "Aggregate-local WHEN expects exactly two arguments: aggregate WHEN condition."));
	}
	if (ExpressionContainsDecideVariable(*when_expr.children[1], variables)) {
		return BindResult(BinderException::Unsupported(
		    when_expr,
		    "Aggregate-local WHEN conditions cannot reference DECIDE variables. "
		    "The WHEN condition must only reference table columns."));
	}

	auto aggregate_result = BindExpression(when_expr.children[0], depth);
	if (aggregate_result.HasError()) {
		return aggregate_result;
	}
	auto aggregate_expr = std::move(aggregate_result.expression);
	auto *aggregate = GetBoundAggregate(*aggregate_expr);
	if (!aggregate) {
		return BindResult(BinderException::Unsupported(
		    when_expr, "Aggregate-local WHEN can only be applied directly to SUM, AVG, MIN, or MAX aggregates."));
	}
	if (aggregate->filter) {
		return BindResult(BinderException::Unsupported(
		    when_expr, "DECIDE aggregate-local WHEN cannot be combined with SQL FILTER on the same aggregate."));
	}

	auto condition_result = ExpressionBinder::BindExpression(when_expr.children[1], depth);
	if (condition_result.HasError()) {
		return condition_result;
	}
	aggregate->filter = BoundCastExpression::AddCastToType(context, std::move(condition_result.expression),
	                                                       LogicalType::BOOLEAN);
	return BindResult(std::move(aggregate_expr));
}

//! True when `expr` holds the same value on every row of the qualified relation: no
//! plain data column and no row- or entity-scoped decision appears anywhere inside it.
//! The bound-tree twin of `DecideBinder::IsRowInvariantExpression`, used once the
//! reducer body is bound and its columns carry scope information.
static bool IsBoundReducerBodyRowInvariant(const Expression &expr, const DecideQualifierContext &ctx) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<const BoundColumnRefExpression>();
		if (colref.binding.table_index != ctx.decide_index) {
			return false; // a plain data column varies per row
		}
		idx_t var_idx = colref.binding.column_index;
		auto &scopes = *ctx.variable_scopes;
		return var_idx < scopes.size() && scopes[var_idx].IsScalar();
	}
	bool invariant = true;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (invariant && !IsBoundReducerBodyRowInvariant(child, ctx)) {
			invariant = false;
		}
	});
	return invariant;
}

//! Whether `table_index` is one of the tables `scope` names.
static bool ScopeContainsTable(const EntityScopeInfo &scope, idx_t table_index) {
	return std::find(scope.source_table_indices.begin(), scope.source_table_indices.end(), table_index) !=
	       scope.source_table_indices.end();
}

//! Whether two scopes name any table in common. Used to check a decision variable's
//! own (always single-relation) declaration scope against a qualifier's — possibly
//! composite — scope.
static bool ScopeTablesIntersect(const EntityScopeInfo &a, const EntityScopeInfo &b) {
	for (auto table_index : a.source_table_indices) {
		if (ScopeContainsTable(b, table_index)) {
			return true;
		}
	}
	return false;
}

//! Enforces the well-formedness rule for a relation-qualified reducer: everything the
//! reducer body reads must come from one of the qualified relations, so that all rows
//! sharing a tuple identity carry the same value and de-duplication can keep any one of
//! them. A query-wide (`scalar`) decision is exempt — it is row-invariant, so it
//! contributes the same value to every tuple regardless of which relation "owns" it; the
//! caller rejects it separately when it is the *only* thing in the body (nothing left to
//! reduce over). Anything else — a column from an unnamed relation, a decision not
//! scoped to any named relation — would make the kept row an arbitrary choice, so it is
//! rejected here rather than resolved silently. Returns "" when the body is well formed,
//! else the message to raise. `relations` names the qualifier for error text, in the
//! order the query wrote them.
static string CheckQualifiedReducerBody(const Expression &expr, const vector<string> &relations,
                                        const string &agg_name, idx_t scope_idx,
                                        const DecideQualifierContext &ctx) {
	string error;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<const BoundColumnRefExpression>();
		auto name = colref.GetName();
		auto relation_list = StringUtil::Join(relations, ", ");
		auto &qualifier_scope = (*ctx.entity_scopes)[scope_idx];
		if (colref.binding.table_index == ctx.decide_index) {
			idx_t var_idx = colref.binding.column_index;
			auto &scopes = *ctx.variable_scopes;
			if (var_idx < scopes.size() && scopes[var_idx].IsEntity() &&
			    ScopeTablesIntersect((*ctx.entity_scopes)[scopes[var_idx].entity_scope_idx], qualifier_scope)) {
				return "";
			}
			if (var_idx < scopes.size() && scopes[var_idx].IsScalar()) {
				return ""; // row-invariant: contributes uniformly, whichever relation it sits beside
			}
			if (relations.size() == 1) {
				return StringUtil::Format(
				    "'%s' is not a decision of %s, so %s(%s: ...) cannot use it; declare it as "
				    "%s.%s(...) or move that term into its own reducer",
				    name, relation_list, StringUtil::Upper(agg_name), relation_list, relation_list, name);
			}
			return StringUtil::Format(
			    "'%s' is not a decision of %s, so %s(%s: ...) cannot use it; declare it on one of "
			    "those relations or move that term into its own reducer",
			    name, relation_list, StringUtil::Upper(agg_name), relation_list);
		}
		if (!ScopeContainsTable(qualifier_scope, colref.binding.table_index)) {
			return StringUtil::Format(
			    "'%s' does not come from %s, so %s(%s: ...) cannot use it; keep only those relations' "
			    "columns inside the qualified reducer and sum the rest separately",
			    name, relation_list, StringUtil::Upper(agg_name), relation_list);
		}
		return "";
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!error.empty()) {
			return;
		}
		error = CheckQualifiedReducerBody(child, relations, agg_name, scope_idx, ctx);
	});
	return error;
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
	for (auto &relation : relations) {
		ErrorData binding_error;
		auto binding = binder.bind_context.GetBinding(relation, binding_error);
		if (!binding || binding->index == ctx.decide_index) {
			return BindResult(BinderException::Unsupported(
			    qualified_expr, StringUtil::Format(
			                        "Relation '%s' is not in the FROM clause, so a reducer cannot be qualified by it.",
			                        relation)));
		}
	}
	// A qualifier is an entity scope with no variable of its own, so it shares the
	// declaration path's key and the one EntityMapping the executor builds per scope.
	// Naming several relations widens the scope's tuple identity to their concatenation.
	idx_t scope_idx =
	    FindOrCreateEntityScope(binder.bind_context, relations, *ctx.entity_scopes, *ctx.table_scope_map);

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
	bool any_row_varying = false;
	for (auto &child : aggregate->children) {
		if (!IsBoundReducerBodyRowInvariant(*child, ctx)) {
			any_row_varying = true;
			break;
		}
	}
	if (!any_row_varying) {
		auto body_text = aggregate->children.empty() ? "" : aggregate->children[0]->ToString();
		return BindResult(BinderException::Unsupported(
		    qualified_expr, StringUtil::Format(
		                        "%s is a query-wide decision, so %s(%s: ...) has nothing to aggregate over; "
		                        "use %s on its own",
		                        body_text, StringUtil::Upper(agg_name), relation_list, body_text)));
	}
	for (auto &child : aggregate->children) {
		auto error = CheckQualifiedReducerBody(*child, relations, agg_name, scope_idx, ctx);
		if (!error.empty()) {
			return BindResult(BinderException::Unsupported(qualified_expr, error));
		}
	}
	aggregate->alias = MakeQualifiedReducerTag(scope_idx);
	return BindResult(std::move(aggregate_expr));
}

BindResult DecideBinder::BindFunction(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
    auto &expr = *expr_ptr;
    auto &function = expr.Cast<FunctionExpression>();
    if (function.is_operator && function.function_name == WHEN_CONSTRAINT_TAG) {
        return BindLocalWhenAggregate(function, depth);
    }
    if (function.is_operator && function.function_name == QUALIFIED_REDUCER_TAG) {
        return BindQualifiedReducer(function, depth);
    }
    // NORM is DECIDE syntax, not a catalog function. Bind it as a deliberately
    // inert SUM aggregate marker; DecideOptimizer owns the mathematical rewrite
    // and replaces this marker before it can reach physical planning. Keeping an
    // aggregate-shaped marker is important: aggregate-local WHEN/PER continue to
    // use their normal bound-tree contracts.
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
    // It's a scalar function, bind it normally
    return ExpressionBinder::BindExpression(expr_ptr, depth);
}

BindResult DecideBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
    // Read the location before dispatching: BindFunction rewrites `expr_ptr` in place
    // for the norm marker, so the node it names afterwards is not the one the user wrote.
    auto location = expr_ptr->GetQueryLocation();
    return PreserveQueryLocation(location, BindExpressionInternal(expr_ptr, depth, root_expression));
}

BindResult DecideBinder::BindExpressionInternal(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                bool root_expression) {
    if (depth > 0) {
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
    auto &expr = *expr_ptr;
    switch (expr.GetExpressionClass()) {
    case ExpressionClass::FUNCTION:
        return BindFunction(expr_ptr, depth);
    case ExpressionClass::SUBQUERY: {
        auto &subquery = expr.Cast<SubqueryExpression>();
        if (subquery.subquery_type != SubqueryType::SCALAR) {
             return BindResult(BinderException::Unsupported(expr, "Only scalar subqueries are supported in DECIDE"));
        }
        if (ExpressionContainsDecideVariable(expr, variables)) {
            return BindResult(BinderException::Unsupported(expr,
                "Subqueries in DECIDE clauses cannot reference DECIDE variables."));
        }
        // Standard binding handles both correlated and uncorrelated scalar subqueries.
        // Uncorrelated: PlanSubqueries evaluates them as cross-joined scalars.
        // Correlated: PlanSubqueries decorrelates them into joins, producing
        //             per-row values that the DECIDE operator evaluates normally.
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
    default:
        return ExpressionBinder::BindExpression(expr_ptr, depth, root_expression);
    }
}



} // namespace duckdb
