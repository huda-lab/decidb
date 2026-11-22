#include "duckdb/planner/expression_binder/decide/decide_declarations_binder.hpp"
#include "duckdb/planner/decide/decide_tree_nodes.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/function_entry.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_constraints_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_objective_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"

namespace duckdb {

// ABS, MIN/MAX and `<>` bind here as ordinary expressions -- a BoundFunctionExpression,
// a BoundAggregateExpression, a BoundComparisonExpression. Choosing a mathematical
// formulation for them is layer 5's job (`src/optimizer/decide/`), which is what lets
// this layer resolve names and types without also deciding how a construct is encoded.

//! In a `when` condition, a `per` key or a `by` key only data may stand: `D.open`, where D is
//! a relation of the FROM clause with no column `open` and `open` is a decision, is refused
//! there. Anywhere else, and for a qualified name that binds to a column, a struct field or
//! a USING column, the SQL binder reads the name. Subquery bodies are not entered: their
//! names are their own.
static void RefuseQualifiedDecisionReferences(const ParsedExpression &expr, BindContext &bind_context,
                                              const case_insensitive_map_t<idx_t> &variables,
                                              const DecideSourceText *text, bool data_position);

static void RefuseQualifiedDecisionReferences(const DecideScope &scope, BindContext &bind_context,
                                              const case_insensitive_map_t<idx_t> &variables,
                                              const DecideSourceText *text) {
	if (scope.when) {
		RefuseQualifiedDecisionReferences(*scope.when, bind_context, variables, text, true);
	}
	for (auto &element : scope.key) {
		RefuseQualifiedDecisionReferences(*element, bind_context, variables, text, true);
	}
}

static void RefuseQualifiedDecisionReferences(const ParsedExpression &expr, BindContext &bind_context,
                                              const case_insensitive_map_t<idx_t> &variables,
                                              const DecideSourceText *text, bool data_position) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		// `[catalog.][schema.]D.open` is at most four names; the SQL binder reads the names after the fourth as
		// struct fields, and a decision has none.
		if (!data_position || !colref.IsQualified() || colref.column_names.size() > 4 ||
		    !variables.count(colref.GetColumnName())) {
			return;
		}
		ErrorData error;
		auto binding = bind_context.GetBinding(colref.GetTableName(), error);
		if (!binding) {
			return;
		}
		throw BinderException(expr, "%s", DecisionInDataMessage(DecideWrittenSql(expr, text), "when, per and by"));
	}
	// A scope's condition and key and a `by` key are data positions.
	if (expr.GetExpressionClass() == ExpressionClass::DECIDE_REDUCER) {
		auto &reducer = expr.Cast<DecideReducerExpression>();
		RefuseQualifiedDecisionReferences(*reducer.call, bind_context, variables, text, data_position);
		RefuseQualifiedDecisionReferences(reducer.scope, bind_context, variables, text);
		for (auto &element : reducer.by) {
			RefuseQualifiedDecisionReferences(*element, bind_context, variables, text, true);
		}
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<FunctionExpression>();
		for (auto &child : func.children) {
			RefuseQualifiedDecisionReferences(*child, bind_context, variables, text, data_position);
		}
		if (func.filter) {
			RefuseQualifiedDecisionReferences(*func.filter, bind_context, variables, text, data_position);
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		RefuseQualifiedDecisionReferences(child, bind_context, variables, text, data_position);
	});
}

//! One spelling per function. DuckDB registers some functions under several names (`pow`, `power` and `^` for `**`;
//! `abs` for `@`; `add`, `subtract` and `multiply` for `+`, `-` and `*`; `mean` for `avg`), each with its primary's
//! implementation. Every call of the clause's algebra whose name resolves, as the binder resolves it, to such an
//! alias is renamed to its primary, so every check from here on matches one name per function. The call keeps the
//! name it was written under (SetDecideWrittenName), which plans and messages read. A `when` condition and a `by` key
//! are respelled too; a subquery is left as written.
static void RespellDecideAliases(ClientContext &context, ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](ParsedExpression &child) { RespellDecideAliases(context, child); });
		return;
	}
	auto &func = expr.Cast<FunctionExpression>();
	auto entry = Catalog::GetEntry(context, CatalogType::SCALAR_FUNCTION_ENTRY, func.catalog, func.schema,
	                               func.function_name, OnEntryNotFound::RETURN_NULL);
	if (entry &&
	    (entry->type == CatalogType::SCALAR_FUNCTION_ENTRY || entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY)) {
		auto &function = entry->Cast<FunctionEntry>();
		if (!function.primary_name.empty()) {
			SetDecideWrittenName(func, function.name);
			func.function_name = function.primary_name;
		}
	}
	for (auto &child : func.children) {
		RespellDecideAliases(context, *child);
	}
}

//! A volatile function (`random()`) would take a different value each time a clause is read, and the solver reads a
//! clause more than once (its scope checks, its rows, a bound-side reducer), so it is refused where it is written.
//! A value computed once per row in a subquery or CTE is ordinary data.
static void RefuseVolatile(const Expression &expr) {
	// The function's own stability: IsVolatile() is true for every expression above a volatile one too.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &function = expr.Cast<BoundFunctionExpression>();
		if (function.function.stability == FunctionStability::VOLATILE) {
			throw BinderException(expr, "a volatile function is not allowed in a DECIDE clause");
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { RefuseVolatile(child); });
}

DecideDeclarationsBinder::DecideDeclarationsBinder(Binder &binder, ClientContext &context)
    : binder(binder), context(context) {
}

void DecideDeclarationsBinder::BindDeclarations(SelectNode &statement, BoundSelectNode &result) {
	auto &bind_context = binder.bind_context;
	auto &decide = *statement.decide;

	case_insensitive_map_t<idx_t> decide_variable_names;
	vector<string> var_names;
	vector<LogicalType> var_types;
	// A BOOL's 0/1 domain is carried by is_boolean_var alone: PhysicalDecide::Finalize applies it to the solver
	// column's bounds, so no SUCH THAT row is synthesized for it.
	vector<bool> is_boolean_var;

	// Key scopes: one per distinct key, shared by the declarations and reducers on it.
	vector<DecideKey> keys;
	vector<KeyedScopeInfo> keyed_scopes;
	vector<DecideVarScopeInfo> variable_scopes; // per variable: row / keyed / query-wide

	// Every declared name, checked and collected before any key resolves, so a key
	// element can be told from a decision declared before or after it.
	case_insensitive_set_t declared_names;
	for (auto &declaration : decide.variables) {
		auto &name = declaration.name;
		// GetMatchingBinding skips a USING column (its binding is the join's merged column), so a
		// decision named like one has to be asked for separately.
		if (bind_context.GetMatchingBinding(name) || bind_context.GetUsingBinding(name)) {
			throw BinderException(declaration.location, "decision '%s' has the name of a column", name);
		}
		RefuseDecideTagInName(declaration.location, name);
		if (!declared_names.insert(name).second) {
			throw BinderException(declaration.location, "decision '%s' is declared twice", name);
		}
	}

	for (auto &declaration : decide.variables) {
		auto &name = declaration.name;
		idx_t var_idx = var_names.size();
		decide_variable_names.emplace(name, var_idx);

		// `per (): x` is one decision for the query and carries no key scope; `per K: x`
		// is one per class of K; no `per`, or `per row`, is one per result row.
		if (declaration.scope.per == DecidePer::ALL) {
			variable_scopes.push_back(DecideVarScopeInfo::Global());
		} else if (declaration.scope.per == DecidePer::KEY) {
			auto key_text = DecideKeyToString(declaration.scope.key);
			auto scope_idx = FindOrCreateKeyScope(context, binder, declaration.scope.key, declared_names, key_text,
			                                      &decide.text, keys, keyed_scopes);
			keyed_scopes[scope_idx].scoped_variable_indices.push_back(var_idx);
			variable_scopes.push_back(DecideVarScopeInfo::Keyed(scope_idx, key_text));
		} else {
			variable_scopes.push_back(DecideVarScopeInfo::Row());
		}
		variable_scopes.back().declaration = declaration.location;
		var_names.push_back(name);
		// An `INT` decision is BIGINT, not INTEGER. Nothing bounds a decision to
		// int32: the solver works in doubles, and an optimum driven by an aggregate
		// row (`SUM(x) <= 5000000000`) has no per-variable bound to inspect here.
		// A narrower column would truncate that answer on readback and hand back a
		// number violating the query's own constraints. BIGINT also matches what
		// DuckDB itself returns for generated integers (`range()`). `BOOL` stays
		// INTEGER — its domain is 0/1, so it cannot overflow, and widening it would
		// change an output column type for no gain.
		var_types.push_back(declaration.type == DecideVariableType::REAL   ? LogicalType::DOUBLE
		                    : declaration.type == DecideVariableType::BOOL ? LogicalType::INTEGER
		                                                                   : LogicalType::BIGINT);
		is_boolean_var.push_back(declaration.type == DecideVariableType::BOOL);
	}

	// One spelling per function, before any check below reads a name.
	decide.EnumerateExpressions([&](unique_ptr<ParsedExpression> &expr) { RespellDecideAliases(context, *expr); });

	// The constraints, then the objective.
	vector<reference<DecideClause>> clauses(decide.constraints.begin(), decide.constraints.end());
	if (decide.objective.body) {
		clauses.push_back(decide.objective);
	}
	// A relation-qualified decision in a `when`, `per` or `by` is refused.
	for (auto &clause : clauses) {
		RefuseQualifiedDecisionReferences(*clause.get().body, bind_context, decide_variable_names, &decide.text, false);
		RefuseQualifiedDecisionReferences(clause.get().scope, bind_context, decide_variable_names, &decide.text);
	}
	// Where `when`, `per` and `by` may stand, before anything reads the clauses: every
	// decision name is known now, so a decision in a condition or a key is named the
	// way the user spelled it.
	for (auto &clause : clauses) {
		ValidateDecideScopeSpellings(clause.get(), decide_variable_names, &decide.text);
	}
	// This is the only point where cast authorship is still observable. Reject
	// explicit casts over decisions before DECIDE generates any parsed nodes and
	// before DuckDB binding adds its own type-reconciliation casts. SELECT-list
	// expressions are intentionally outside this solver-algebra policy.
	decide.EnumerateExpressions([&](unique_ptr<ParsedExpression> &expr) {
		ValidateDecideNoExplicitDecisionCasts(*expr, decide_variable_names, &decide.text);
	});

	bind_context.AddGenericBinding(result.decide_index, "decide_variables", var_names, var_types);
	// Query-wide decisions (`per (): x`), so the constraint and objective binders can
	// tell a query-wide decision from a per-row one.
	case_insensitive_set_t scalar_variable_names;
	for (idx_t v = 0; v < variable_scopes.size(); v++) {
		if (variable_scopes[v].IsGlobal()) {
			scalar_variable_names.insert(var_names[v]);
		}
	}
	// What a relation-qualified reducer needs to resolve `sum(per D: ...)`. The binders
	// may append a key-only keyed scope to `keyed_scopes`, which is why this runs
	// before the vector is moved onto the operator below.
	DecideQualifierContext qualifier_context;
	qualifier_context.decide_index = result.decide_index;
	qualifier_context.keyed_scopes = &keyed_scopes;
	qualifier_context.keys = &keys;
	qualifier_context.source_text = &decide.text;
	vector<DecideFrameSpec> frames;
	qualifier_context.frames = &frames;
	// Isolate with brackets to avoid multiple active binders.
	{
		DecideConstraintsBinder decide_constraints_binder(binder, context, decide_variable_names, scalar_variable_names,
		                                                  &qualifier_context);
		auto constraints = std::move(decide.constraints);
		result.decide.clauses = decide_constraints_binder.BindConstraints(constraints);
		for (auto &clause : result.decide.clauses) {
			clause.EnumerateExpressions([](unique_ptr<Expression> &expression) { RefuseVolatile(*expression); });
		}
		result.decide.constraint_sources = InitializeConstraintSourceInfo(
		    result.decide.clauses, decide.text, keyed_scopes, result.decide_index, &keys, frames);
	}
	if (decide.objective.body) {
		DecideObjectiveBinder decide_objective_binder(binder, context, decide_variable_names, scalar_variable_names,
		                                              &qualifier_context);
		decide_objective_binder.decide_sense = decide.sense;
		auto objective = std::move(decide.objective);
		result.decide.objective = decide_objective_binder.BindObjective(objective);
		result.decide.objective.EnumerateExpressions(
		    [](unique_ptr<Expression> &expression) { RefuseVolatile(*expression); });
	}
	for (auto &key : keys) {
		for (auto &element : key.elements) {
			RefuseVolatile(*element.expression);
		}
	}
	result.decide.source_text = decide.text;
	result.decide.sense = decide.sense;
	// Update types in bind context to reflect the determined types from DECIDE clause
	bind_context.GetBindingsList().back()->types = var_types;
	for (idx_t i = 0; i < var_names.size(); i++) {
		auto bound_col_ref =
		    make_uniq<BoundColumnRefExpression>(var_names[i], var_types[i], ColumnBinding(result.decide_index, i));
		result.decide.variables.push_back(std::move(bound_col_ref));
	}
	result.decide.is_boolean_var = is_boolean_var;

	result.decide.keys = std::move(keys);
	result.decide.frames = std::move(frames);

	result.decide.keyed_scopes = std::move(keyed_scopes);
	result.decide.variable_scopes = variable_scopes;
}

} // namespace duckdb
