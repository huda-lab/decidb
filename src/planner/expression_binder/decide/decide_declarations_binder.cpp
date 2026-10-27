#include "duckdb/planner/expression_binder/decide/decide_declarations_binder.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/decide/decide_removed_spellings.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_constraints_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression_binder/decide/decide_objective_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"

namespace duckdb {

// ABS, MIN/MAX and `<>` bind here as ordinary expressions -- a BoundFunctionExpression,
// a BoundAggregateExpression, a BoundComparisonExpression. Choosing a mathematical
// formulation for them is layer 5's job (`src/optimizer/decide/`), which is what lets
// this layer resolve names and types without also deciding how a construct is encoded.

//! A decision is referenced by its bare name. `D.open`, where D is a relation of the FROM
//! clause with no column `open` and `open` is a decision, is the removed table-qualified
//! reference and is refused with the bare name; in a `when` condition, a `per` key or a
//! `by` key it is a decision where only data may stand. A qualified name that binds to a
//! column, a struct field or a USING column is left to the SQL binder. Subquery bodies are
//! not entered: their names are their own.
static void RefuseQualifiedDecisionReferences(const ParsedExpression &expr, BindContext &bind_context,
                                              const case_insensitive_map_t<idx_t> &variables, bool data_position) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		if (!colref.IsQualified() || !variables.count(colref.GetColumnName())) {
			return;
		}
		ErrorData error;
		auto binding = bind_context.GetBinding(colref.GetTableName(), error);
		if (!binding || binding->HasMatchingBinding(colref.GetColumnName())) {
			return;
		}
		if (data_position) {
			throw BinderException(expr, "DECIDE: %s is a decision; when, per and by may only use data",
			                      colref.ToString());
		}
		throw BinderException(expr, "DECIDE: write %s, not %s; a decision is referenced by its bare name",
		                      colref.GetColumnName(), colref.ToString());
	}
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<FunctionExpression>();
		// The children after the body of a scope or `by` marker are its condition or key.
		bool scope = func.is_operator && (func.function_name == SCOPE_WHEN_TAG || func.function_name == SCOPE_PER_TAG ||
		                                  func.function_name == REDUCER_BY_TAG);
		for (idx_t i = 0; i < func.children.size(); i++) {
			RefuseQualifiedDecisionReferences(*func.children[i], bind_context, variables,
			                                  data_position || (scope && i > 0));
		}
		if (func.filter) {
			RefuseQualifiedDecisionReferences(*func.filter, bind_context, variables, data_position);
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		RefuseQualifiedDecisionReferences(child, bind_context, variables, data_position);
	});
}

//! One declaration `[per K:] name(TYPE)` as the parser leaves it: the comparison
//! `name = 'type_marker'`, its name wrapped in the `SCOPE_PER_TAG` marker when a `per`
//! was written.
struct ParsedDeclaration {
	const ColumnRefExpression *name = nullptr;
	//! The `per` marker, or null when none was written or it was `per row`, the default
	//! written out. One child means `per ()`.
	const FunctionExpression *per = nullptr;
	string type_marker;
};

//! A volatile function (`random()`) would take a different value each time a clause is read, and the solver reads a
//! clause more than once (its scope checks, its rows, a bound-side reducer), so it is refused where it is written.
//! A value computed once per row in a subquery or CTE is ordinary data.
static void RefuseVolatile(const Expression &expr, const char *where) {
	// The function's own stability: IsVolatile() is true for every expression above a volatile one too.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &function = expr.Cast<BoundFunctionExpression>();
		if (function.function.stability == FunctionStability::VOLATILE) {
			throw BinderException("DECIDE: %s() is not supported in %s", function.function.name, where);
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { RefuseVolatile(child, where); });
}

static ParsedDeclaration SplitDeclaration(const ParsedExpression &declaration) {
	if (declaration.GetExpressionClass() != ExpressionClass::COMPARISON) {
		throw BinderException(declaration, "Invalid DECIDE variable declaration.");
	}
	auto &comp = declaration.Cast<ComparisonExpression>();
	ParsedDeclaration result;
	auto *name = comp.left.get();
	if (name->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &marker = name->Cast<FunctionExpression>();
		if (marker.is_operator && marker.function_name == SCOPE_PER_TAG && !marker.children.empty()) {
			result.per = &marker;
			name = marker.children[0].get();
		} else if (marker.is_operator && marker.function_name == SCOPE_PER_ROW_TAG && !marker.children.empty()) {
			name = marker.children[0].get();
		}
	}
	if (name->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		throw BinderException(declaration, "Invalid DECIDE variable declaration: expected variable name on left side.");
	}
	result.name = &name->Cast<ColumnRefExpression>();
	if (comp.right->GetExpressionClass() == ExpressionClass::CONSTANT) {
		auto &type = comp.right->Cast<ConstantExpression>();
		if (type.value.type() == LogicalType::VARCHAR) {
			result.type_marker = type.value.ToString();
		}
	}
	return result;
}

DecideDeclarationsBinder::DecideDeclarationsBinder(Binder &binder, ClientContext &context)
    : binder(binder), context(context) {
}

void DecideDeclarationsBinder::BindDeclarations(SelectNode &statement, BoundSelectNode &result) {
	auto &bind_context = binder.bind_context;

        case_insensitive_map_t<idx_t> decide_variable_names;
        vector<string> var_names;
        vector<LogicalType> var_types;
        vector<bool> is_boolean_var;  // Track which variables are BOOLEAN for generating bounds
        vector<string> decide_source_fragments;

        // Key scopes: one per distinct key, shared by the declarations and reducers on it.
        vector<EntityScopeInfo> entity_scopes;
        vector<DecideVarScopeInfo> variable_scopes; // per variable: row / keyed / query-wide

        // A parse tree stored by an earlier version (a view, a macro) may still carry a
        // removed spelling (D54, language/errors.md §3); a fresh query never does, the
        // transformer refuses them.
        RefuseRemovedDecideSpellings(statement);

        // Every declared name, checked and collected before any key resolves, so a key
        // element can be told from a decision declared before or after it.
        case_insensitive_set_t declared_names;
        for (const auto &expr_ptr : statement.decide_variables) {
            auto &name = SplitDeclaration(*expr_ptr).name->GetColumnName();
		    // GetMatchingBinding skips a USING column (its binding is the join's merged column), so a
		    // decision named like one has to be asked for separately.
		    if (bind_context.GetMatchingBinding(name) || bind_context.GetUsingBinding(name)) {
			    throw BinderException(*expr_ptr, "DECIDE variable '%s' conflicts with an existing column name.", name);
		    }
		    if (!declared_names.insert(name).second) {
                throw BinderException(*expr_ptr, "Duplicate DECIDE variable name '%s'.", name);
            }
        }

        for (const auto &expr_ptr : statement.decide_variables) {
            auto declaration = SplitDeclaration(*expr_ptr);
            string name = declaration.name->GetColumnName();
            string type_marker = declaration.type_marker;
            idx_t var_idx = var_names.size();
            decide_variable_names.emplace(name, var_idx);

            // `per (): x` is one decision for the query and carries no key scope; `per K: x`
            // is one per class of K; no `per` is one per result row.
            if (declaration.per && declaration.per->children.size() == 1) {
                variable_scopes.push_back(DecideVarScopeInfo::Scalar());
            } else if (declaration.per) {
                auto key_text = DecideKeyToString(*declaration.per);
                auto scope_idx =
                    FindOrCreateKeyScope(bind_context, *declaration.per, declared_names, key_text, entity_scopes);
                entity_scopes[scope_idx].scoped_variable_indices.push_back(var_idx);
                variable_scopes.push_back(DecideVarScopeInfo::Keyed(scope_idx, key_text));
            } else {
                variable_scopes.push_back(DecideVarScopeInfo::Row());
            }
            var_names.push_back(name);
            // An `INT` decision is BIGINT, not INTEGER. Nothing bounds a decision to
            // int32: the solver works in doubles, and an optimum driven by an aggregate
            // row (`SUM(x) <= 5000000000`) has no per-variable bound to inspect here.
            // A narrower column would truncate that answer on readback and hand back a
            // number violating the query's own constraints. BIGINT also matches what
            // DuckDB itself returns for generated integers (`range()`). `BOOL` stays
            // INTEGER — its domain is 0/1, so it cannot overflow, and widening it would
            // change an output column type for no gain.
            var_types.push_back(type_marker == "real_variable"   ? LogicalType::DOUBLE
                                : type_marker == "bool_variable" ? LogicalType::INTEGER
                                                                 : LogicalType::BIGINT);
            is_boolean_var.push_back(type_marker == "bool_variable");
        }

        // A variable's `0/1` domain (declared BOOL, or a boolean-valued auxiliary
        // like an IN-domain/L0 indicator that must stay INTEGER-typed to
        // participate in arithmetic — see is_boolean_var's uses below) is never
        // synthesized as a SUCH THAT constraint. `is_boolean_var` alone carries the
        // domain from here through LogicalDecide to PhysicalDecide, which applies it
        // directly to the solver column's bounds (PhysicalDecide::Finalize) — the
        // same mechanism already used for optimizer-created BOOLEAN-typed
        // auxiliaries (MIN/MAX indicators, NE indicators). This avoids the round
        // trip through the constraint tree: no redundant rows for downstream
        // rewrites to carry or accidentally reshape, and no expression-shape
        // pattern-matching needed to recover the domain at model-build time.

        // Capture user var count BEFORE any rewrites that add auxiliary variables
        idx_t num_user_vars = var_names.size();

        // A decision is referenced by its bare name, wherever the query names it.
        auto refuse_qualified = [&](const ParsedExpression &expr) {
            RefuseQualifiedDecisionReferences(expr, bind_context, decide_variable_names, false);
        };
        if (statement.decide_constraints) {
            refuse_qualified(*statement.decide_constraints);
        }
        if (statement.decide_objective) {
            refuse_qualified(*statement.decide_objective);
        }
        for (auto &sel_expr : statement.select_list) {
            refuse_qualified(*sel_expr);
        }
        for (auto &modifier : statement.modifiers) {
            if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
                for (auto &order : modifier->Cast<OrderModifier>().orders) {
                    refuse_qualified(*order.expression);
                }
            }
        }

        // Where `when`, `per` and `by` may stand, before anything reads the clauses: every
        // decision name is known now, so a decision in a condition or a key is named the
        // way the user spelled it.
        if (statement.decide_constraints) {
            ValidateDecideScopeSpellings(*statement.decide_constraints, decide_variable_names, false);
        }
        if (statement.decide_objective) {
            ValidateDecideScopeSpellings(*statement.decide_objective, decide_variable_names, true);
        }

        // This is the only point where cast authorship is still observable. Reject
        // explicit casts over decisions before DECIDE generates any parsed nodes and
        // before DuckDB binding adds its own type-reconciliation casts. SELECT-list
        // expressions are intentionally outside this solver-algebra policy.
        if (statement.decide_constraints) {
            ValidateDecideNoExplicitDecisionCasts(*statement.decide_constraints,
                                                  decide_variable_names);
        }
        if (statement.decide_objective) {
            ValidateDecideNoExplicitDecisionCasts(*statement.decide_objective,
                                                  decide_variable_names);
        }

        // NORM and IN bind as explicit markers. Their mathematical formulation
        // (indicators, linking rows, and aggregate rewrites) belongs to
        // DecideOptimizer, after types/scopes/casts are known.

        // NOT-EQUAL (<>) indicator variables are now created by DecideOptimizer
        // (runs after binding, creates BOOLEAN indicators directly on LogicalDecide)

        // MIN/MAX, NE, and IN/L0 indicator bounds are all handled the same way now:
        // `is_boolean_var[i]` marks the domain and PhysicalDecide::Finalize applies
        // `[0,1]` directly to the solver column, regardless of whether the indicator's
        // DuckDB-facing type is BOOLEAN (MIN/MAX, NE) or INTEGER (IN/L0, which need
        // INTEGER to participate in the parsed arithmetic that links them). No
        // constraint-tree bounds to generate here.
        idx_t num_auxiliary_vars = var_names.size() - num_user_vars;

        if (statement.decide_constraints) {
            // Reject scalar functions like sqrt(x), exp(x), floor(x) wrapping a
            // DECIDE variable, before anything downstream has to interpret them.
            ValidateDecideNoNonLinearScalar(context, *statement.decide_constraints, decide_variable_names);
            // A `<`, `>` or `<>` over a REAL decision has no exact encoding — all three
            // are encoded by stepping the bound one integer unit — and the declared type
            // says so without reading a row. Rejecting here names the variable and quotes
            // the clause; the model builder keeps only the value half of the refusal (a
            // data column yielding a fractional coefficient).
            ValidateDecideNoIntegerStepComparisonOnReal(*statement.decide_constraints, decide_variable_names,
                                                        var_types);
            // Source-only casts and scalar subqueries lose their written spelling
            // during binding/flattening. Give those atoms stable fragment ids now;
            // the DECIDE binder carries the tags onto the bound nodes.
            TagDecideSourceFragments(*statement.decide_constraints, decide_source_fragments);
        }
        if (statement.decide_objective) {
            ValidateDecideNoNonLinearScalar(context, *statement.decide_objective, decide_variable_names);
            // The objective quotes its own SQL in a message (a `when` that keeps no row, a
            // term that reads no decision), so its casts and subqueries keep their written
            // text, as every constraint's do.
            TagDecideSourceFragments(*statement.decide_objective, decide_source_fragments);
        }

        bind_context.AddGenericBinding(result.decide_index, "decide_variables", var_names, var_types);
        // Query-wide decisions (`per (): x`), so the constraint and objective binders can
        // tell a query-wide decision from a per-row one.
        case_insensitive_set_t scalar_variable_names;
        for (idx_t v = 0; v < variable_scopes.size() && v < var_names.size(); v++) {
            if (variable_scopes[v].IsScalar()) {
                scalar_variable_names.insert(var_names[v]);
            }
        }
        // What a relation-qualified reducer needs to resolve `sum(D: ...)`. The binders
        // may append a key-only entity scope to `entity_scopes`, which is why this runs
        // before the vector is moved onto the operator below.
        DecideQualifierContext qualifier_context;
        qualifier_context.decide_index = result.decide_index;
        qualifier_context.entity_scopes = &entity_scopes;
        vector<DecideGroupKey> group_keys;
        qualifier_context.group_keys = &group_keys;
        qualifier_context.source_fragments = &decide_source_fragments;
        // Isolate with brackets to avoid multiple active binders.
        {
            DecideConstraintsBinder decide_constraints_binder (binder, context, decide_variable_names, scalar_variable_names, &qualifier_context);
            unique_ptr<ParsedExpression> constraints = std::move(statement.decide_constraints);
            result.decide_constraints = decide_constraints_binder.Bind(constraints);
            if (result.decide_constraints) {
                RefuseVolatile(*result.decide_constraints, "a constraint");
                // The other half of the integer-step gate. The parsed-tree validator above
                // read the decision's declared type off the DECIDE clause; every other
                // operand's type is only known now that binding has resolved it. Together
                // they are the whole refusal, so the model builder no longer raises one.
                auto relations = DecideRelationNames(bind_context, result.decide_index);
                DecideSourceNames names {decide_source_fragments, entity_scopes, group_keys, relations};
                ValidateDecideIntegralComparisonOperands(*result.decide_constraints, result.decide_index, &names);
                // Degree has one owner, and this is it. Running on the bound tree makes the
                // rule total: a reducer argument and a bare per-row constraint are judged
                // the same way, which the parsed-tree gate could not do because it was only
                // ever called for SUM/AVG/MIN/MAX arguments.
                ValidateDecideConstraintDegree(*result.decide_constraints, result.decide_index);
                result.decide_constraint_sources =
                    InitializeConstraintSourceInfo(*result.decide_constraints, decide_source_fragments, entity_scopes,
                                                   result.decide_index, &group_keys);
            }
            // Types are now determined from the DECIDE clause, not from constraint binding
        }
        if (statement.decide_objective) {
            DecideObjectiveBinder decide_objective_binder (binder, context, decide_variable_names, scalar_variable_names, &qualifier_context);
            decide_objective_binder.decide_sense = statement.decide_sense;
            unique_ptr<ParsedExpression> objective = std::move(statement.decide_objective);
            result.decide_objective = decide_objective_binder.Bind(objective);
            if (result.decide_objective) {
                RefuseVolatile(*result.decide_objective, "the objective");
                ValidateDecideObjectiveDegree(*result.decide_objective, result.decide_index);
            }
        }
        for (auto &key : group_keys) {
            for (auto &element : key.expressions) {
                RefuseVolatile(*element, "a by key");
            }
        }
        result.decide_source_fragments = decide_source_fragments;
        result.decide_sense = statement.decide_sense;
        // Update types in bind context to reflect the determined types from DECIDE clause
        bind_context.GetBindingsList().back()->types = var_types;
        for (idx_t i = 0; i < var_names.size(); i++) {
            auto bound_col_ref = make_uniq<BoundColumnRefExpression>(
                var_names[i],
                var_types[i],
                ColumnBinding(result.decide_index, i)
            );
            result.decide_variables.push_back(std::move(bound_col_ref));
        }
        result.num_auxiliary_vars = num_auxiliary_vars;
        result.is_boolean_var = is_boolean_var;

        // Create BoundColumnRefExpressions for entity-key columns.
        // These are stored on LogicalDecide so that DuckDB's binder column-reference
        // tracking (used by the initial Get column_id selection AND by the column
        // pruner's rebinding pass) treats entity-key columns as live. Without this,
        // entity-key columns that don't also appear in SELECT/WHERE/constraints/
        // objective get pruned by the Binder, leaving the VarIndexer with a
        // degenerate key set that silently collapses distinct entities.
        for (auto &scope : entity_scopes) {
            for (idx_t k = 0; k < scope.entity_key_bindings.size(); k++) {
                result.entity_key_expressions.push_back(
                    make_uniq<BoundColumnRefExpression>(
                        "entity_key_" + scope.table_alias,
                        scope.entity_key_column_types[k],
                        scope.entity_key_bindings[k]));
            }
        }

        result.decide_group_keys = std::move(group_keys);

        result.entity_scopes = std::move(entity_scopes);
        result.variable_scopes = variable_scopes;

        // ne_clause_labels is now populated by DecideOptimizer (post-binding)
        // minmax_clause_labels, flat_objective_agg/is_easy, per_inner/outer_agg/is_easy
        // are now populated by DecideOptimizer::RewriteMinMax (post-binding)

        // Hide auxiliary vars from SELECT * by truncating the bind context binding.
        // The decide_variables vector still has ALL vars (user + aux) for the execution layer,
        // but the bind context only exposes user vars for star expansion.
        if (num_auxiliary_vars > 0) {
            auto &binding = *bind_context.GetBindingsList().back();
            for (idx_t i = num_user_vars; i < var_names.size(); i++) {
                binding.name_map.erase(var_names[i]);
            }
            binding.names.resize(num_user_vars);
            binding.types.resize(num_user_vars);
        }
}

} // namespace duckdb
