#include "duckdb/planner/expression_binder/decide/decide_declarations_binder.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_constraints_binder.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression_binder/decide/decide_objective_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"

namespace duckdb {

// ABS linearization is now fully handled by DecideOptimizer::RewriteAbs (post-binding).
// The binder binds ABS as a normal BoundFunctionExpression; the optimizer detects it,
// creates auxiliary REAL variables, and generates linearization constraints.

// MIN/MAX constraint and objective rewriting is now fully handled by
// DecideOptimizer::RewriteMinMax (post-binding). The binder binds MIN/MAX as normal
// BoundAggregateExpression nodes; the optimizer classifies easy/hard, creates
// indicator variables, and rewrites to SUM.

// Rewrite qualified `Table.var` ColumnRefs into bare `var` ColumnRefs when
// `Table.var` matches a registered table-scoped DECIDE variable. After this
// pass, every reference to a scoped decision variable is unqualified, so the
// regular DuckDB binder (used for SELECT/ORDER/etc.) and the per-row branch
// of DecideConstraintsBinder both resolve it through the generic
// `decide_variables` binding instead of routing `Table` to the real table
// binding (which has no such column). This pre-pass strips the qualifier
// uniformly across aggregate SUM bodies, per-row constraints, the SELECT list
// and the objective, so no later stage has to recognize the qualified spelling.
static void RewriteScopedVarRefs(unique_ptr<ParsedExpression> &expr,
                                  const case_insensitive_map_t<idx_t> &variables) {
	if (!expr) {
		return;
	}
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (colref.IsQualified()) {
			string qualified = colref.GetTableName() + "." + colref.GetColumnName();
			if (variables.count(qualified)) {
				auto alias = colref.GetAlias();
				expr = make_uniq<ColumnRefExpression>(colref.GetColumnName());
				if (!alias.empty()) {
					expr->alias = alias;
				}
				return;
			}
		}
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		RewriteScopedVarRefs(child, variables);
	});
}

// NOTE: RewriteNotEqual has been moved to DecideOptimizer (src/optimizer/decide/decide_optimizer.cpp).
// It now operates on BoundExpressions post-binding instead of ParsedExpressions pre-binding.

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

        // Table-scoped variable tracking
        vector<EntityScopeInfo> entity_scopes;
        vector<DecideVarScopeInfo> variable_scopes;  // per variable: row / entity / query-wide scalar
        // Map table alias → entity_scopes index (to share scope info for multiple vars on same table)
        case_insensitive_map_t<idx_t> table_scope_map;

        for (const auto& expr_ptr : statement.decide_variables) {
            string name;
            string table_name;  // empty for row-scoped variables
            string type_marker;

            // Handle typed variable declarations (ComparisonExpression from "x IS INTEGER")
            if (expr_ptr->GetExpressionClass() == ExpressionClass::COMPARISON) {
                const auto& comp = expr_ptr->Cast<duckdb::ComparisonExpression>();

                // LHS should be the variable name (ColumnRefExpression)
                if (comp.left->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
                    throw BinderException(*expr_ptr, "Invalid DECIDE variable declaration: expected variable name on left side.");
                }
                const auto& colref = comp.left->Cast<duckdb::ColumnRefExpression>();
                name = colref.GetColumnName();
                if (colref.IsQualified()) {
                    table_name = colref.GetTableName();
                }

                // RHS should be the type marker (ConstantExpression with string value)
                if (comp.right->GetExpressionClass() == ExpressionClass::CONSTANT) {
                    const auto& const_expr = comp.right->Cast<duckdb::ConstantExpression>();
                    if (const_expr.value.type() == LogicalType::VARCHAR) {
                        type_marker = const_expr.value.ToString();
                    }
                }
            } else {
                throw BinderException(*expr_ptr, "Invalid DECIDE variable declaration.");
            }

            // The grammar flags a query-wide declaration by prefixing the type
            // marker (see scalar_variable_type in select.y). Strip it so the type
            // comparisons below stay scope-agnostic.
            const string scalar_prefix = "scalar_";
            bool is_scalar = StringUtil::StartsWith(type_marker, scalar_prefix);
            if (is_scalar) {
                type_marker = type_marker.substr(scalar_prefix.size());
            }

            if (bind_context.GetMatchingBinding(name)) {
                throw BinderException(*expr_ptr, "DECIDE variable '%s' conflicts with an existing column name.", name);
            }
            if (decide_variable_names.count(name)) {
                throw BinderException(*expr_ptr, "Duplicate DECIDE variable name '%s'.", name);
            }

            idx_t var_idx = var_names.size();

            // Register under unqualified name (always)
            decide_variable_names.emplace(name, var_idx);

            // Handle table-scoped variable
            idx_t scope_idx = DConstants::INVALID_INDEX;
            if (!table_name.empty()) {
                // Resolve table in the bind context
                ErrorData error;
                auto binding = bind_context.GetBinding(table_name, error);
                if (!binding) {
                    throw BinderException(*expr_ptr,
                        "DECIDE variable '%s.%s': table '%s' not found in FROM clause.",
                        table_name, name, table_name);
                }

                // Also register under qualified name so constraints can use T.x syntax
                string qualified_name = table_name + "." + name;
                decide_variable_names.emplace(qualified_name, var_idx);

                // Reuse the scope if another variable — or a qualified reducer — already
                // keyed on this table; the tuple-identity key is the same either way.
                scope_idx = FindOrCreateEntityScope(bind_context, table_name, entity_scopes, table_scope_map);
                entity_scopes[scope_idx].scoped_variable_indices.push_back(var_idx);
            }

            // A scalar is query-wide, so it never carries an entity scope; the
            // grammar already rejects the table-qualified spelling.
            if (is_scalar) {
                variable_scopes.push_back(DecideVarScopeInfo::Scalar());
            } else if (scope_idx != DConstants::INVALID_INDEX) {
                variable_scopes.push_back(DecideVarScopeInfo::Entity(scope_idx));
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

        // Strip table qualifiers from `Table.var` references that match a
        // registered scoped DECIDE variable. Applied to the SELECT list, the
        // SUCH THAT tree, and the objective tree so that downstream binders
        // (regular DuckDB binder for SELECT, per-row branch of
        // DecideConstraintsBinder) see bare `var` instead of `Table.var`.
        // No-op when no scoped variables are declared.
        RewriteScopedVarRefs(statement.decide_constraints, decide_variable_names);
        RewriteScopedVarRefs(statement.decide_objective, decide_variable_names);
        for (auto &sel_expr : statement.select_list) {
            RewriteScopedVarRefs(sel_expr, decide_variable_names);
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

        // MIN/MAX objective rewrite is handled by DecideOptimizer::RewriteMinMaxObjective (post-binding).
        // MIN/MAX aggregates pass through normalization and binding as normal functions.

        // ABS linearization is now fully handled by DecideOptimizer::RewriteAbs (post-binding).
        // ABS(expr) passes through normalization and binding as a normal function.
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
        }

        bind_context.AddGenericBinding(result.decide_index, "decide_variables", var_names, var_types);
        // Names declared with `scalar`, so the constraint and objective binders can
        // tell a query-wide decision from a row-scoped one.
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
        qualifier_context.table_scope_map = &table_scope_map;
        qualifier_context.variable_scopes = &variable_scopes;
        // Isolate with brackets to avoid multiple active binders.
        {
            DecideConstraintsBinder decide_constraints_binder (binder, context, decide_variable_names, scalar_variable_names, &qualifier_context);
            unique_ptr<ParsedExpression> constraints = std::move(statement.decide_constraints);
            result.decide_constraints = decide_constraints_binder.Bind(constraints);
            if (result.decide_constraints) {
                // The other half of the integer-step gate. The parsed-tree validator above
                // read the decision's declared type off the DECIDE clause; every other
                // operand's type is only known now that binding has resolved it. Together
                // they are the whole refusal, so the model builder no longer raises one.
                ValidateDecideIntegralComparisonOperands(*result.decide_constraints,
                                                         result.decide_index);
                // Degree has one owner, and this is it. Running on the bound tree makes the
                // rule total: a reducer argument and a bare per-row constraint are judged
                // the same way, which the parsed-tree gate could not do because it was only
                // ever called for SUM/AVG/MIN/MAX arguments.
                ValidateDecideConstraintDegree(*result.decide_constraints, result.decide_index);
                result.decide_constraint_sources = InitializeConstraintSourceInfo(
                    *result.decide_constraints, decide_source_fragments, entity_scopes,
                    result.decide_index);
                result.decide_source_fragments = decide_source_fragments;
            }
            // Types are now determined from the DECIDE clause, not from constraint binding
        }
        if (statement.decide_objective) {
            DecideObjectiveBinder decide_objective_binder (binder, context, decide_variable_names, scalar_variable_names, &qualifier_context);
            decide_objective_binder.decide_sense = statement.decide_sense;
            unique_ptr<ParsedExpression> objective = std::move(statement.decide_objective);
            result.decide_objective = decide_objective_binder.Bind(objective);
            if (result.decide_objective) {
                ValidateDecideObjectiveDegree(*result.decide_objective, result.decide_index);
            }
        }
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

        // Refine entity keys: exclude scoped-table columns that appear as coefficient
        // data in constraints/objectives. These are dependent data columns, not identity columns.
        // Without this refinement, CTEs with multiple columns would use ALL columns as
        // entity key, making every row its own entity and defeating table-scoping.
        if (!entity_scopes.empty()) {
            // Collect column bindings from scoped tables that appear in bound expressions
            unordered_set<uint64_t> data_columns;  // encode as (table_index << 32) | col_index
            auto collect_data_cols = [&](Expression &expr) {
                if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    auto &col_ref = expr.Cast<BoundColumnRefExpression>();
                    auto &binding = col_ref.binding;
                    // Check if this column belongs to any scoped table
                    for (auto &scope : entity_scopes) {
                        bool in_scope = false;
                        for (auto scope_table_index : scope.source_table_indices) {
                            if (binding.table_index == scope_table_index) {
                                in_scope = true;
                                break;
                            }
                        }
                        if (in_scope) {
                            data_columns.insert((static_cast<uint64_t>(binding.table_index) << 32) |
                                                static_cast<uint64_t>(binding.column_index));
                        }
                    }
                }
            };
            if (result.decide_constraints) {
                ExpressionIterator::EnumerateExpression(result.decide_constraints, collect_data_cols);
            }
            if (result.decide_objective) {
                ExpressionIterator::EnumerateExpression(result.decide_objective, collect_data_cols);
            }

            // Remove data columns from entity keys (keep at least one key column)
            for (auto &scope : entity_scopes) {
                vector<ColumnBinding> refined_bindings;
                vector<LogicalType> refined_types;
                for (idx_t k = 0; k < scope.entity_key_bindings.size(); k++) {
                    auto &b = scope.entity_key_bindings[k];
                    uint64_t key = (static_cast<uint64_t>(b.table_index) << 32) |
                                   static_cast<uint64_t>(b.column_index);
                    if (data_columns.find(key) == data_columns.end()) {
                        refined_bindings.push_back(b);
                        refined_types.push_back(scope.entity_key_column_types[k]);
                    }
                }
                // Only apply refinement if at least one column survives
                if (!refined_bindings.empty()) {
                    scope.entity_key_bindings = std::move(refined_bindings);
                    scope.entity_key_column_types = std::move(refined_types);
                }
            }
        }

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

        // Store table-scoped variable metadata
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
