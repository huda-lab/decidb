#include "duckdb/planner/binder.hpp"
#include "duckdb/decidb/decide_cast_policy.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/common/enums/decide.hpp"
#include <functional>

namespace duckdb {

unique_ptr<LogicalOperator> Binder::PlanFilter(unique_ptr<Expression> condition, unique_ptr<LogicalOperator> root) {
	PlanSubqueries(condition, root);
	auto filter = make_uniq<LogicalFilter>(std::move(condition));
	filter->AddChild(std::move(root));
	return std::move(filter);
}

unique_ptr<LogicalOperator> Binder::CreatePlan(BoundSelectNode &statement) {
	unique_ptr<LogicalOperator> root;
	D_ASSERT(statement.from_table);
	root = CreatePlan(*statement.from_table);
	D_ASSERT(root);

	// plan the sample clause
	if (statement.sample_options) {
		root = make_uniq<LogicalSample>(std::move(statement.sample_options), std::move(root));
	}
	if (statement.where_clause) {
		root = PlanFilter(std::move(statement.where_clause), std::move(root));
	}

    if (statement.HasDecideClause()) {
        // Which column refs will be ONE value for the whole query once flattening is
        // done. Canonicalization needs this to tell a legal factor on a reducer
        // (`(SELECT k FROM p) * SUM(x)`) from an illegal one (`weight * SUM(x)`, which
        // has no answer to "which row's weight?"). After PlanSubqueries all three of
        // `weight`, an uncorrelated subquery and a CORRELATED subquery are plain column
        // refs — the flattened subquery is even named "SUBQUERY" — so only correlation
        // information captured HERE separates them, and it exists only before
        // flattening. Hold the owning slots: PlanSubqueries replaces the node in place,
        // so the slot is what survives to be read afterward.
        struct SourceSubquerySlot {
            unique_ptr<Expression> *slot;
            idx_t source_fragment_id = DConstants::INVALID_INDEX;
        };
        vector<SourceSubquerySlot> uncorrelated_subquery_slots;
        vector<SourceSubquerySlot> correlated_subquery_slots;
        {
            std::function<void(unique_ptr<Expression> &)> collect_subqueries =
                [&](unique_ptr<Expression> &slot) {
                    if (slot->GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
                        auto &subq = slot->Cast<BoundSubqueryExpression>();
                        if (subq.subquery_type == SubqueryType::SCALAR) {
                            // Correlated ones are recorded only so a rejection can call
                            // them a subquery; they stay out of the query-wide set and
                            // are refused by the fail-safe default either way.
                            idx_t source_fragment_id = DConstants::INVALID_INDEX;
                            TryParseSourceFragmentTag(slot->GetAlias(), source_fragment_id);
                            (subq.IsCorrelated() ? correlated_subquery_slots
                                                 : uncorrelated_subquery_slots)
                                .push_back({&slot, source_fragment_id});
                        }
                        return;
                    }
                    ExpressionIterator::EnumerateChildren(*slot, collect_subqueries);
                };
            if (statement.decide_constraints) {
                collect_subqueries(statement.decide_constraints);
            }
            // The objective passes through the same canonicalization boundary, so it
            // needs the same correlation evidence. Without this an UNCORRELATED
            // subquery scaling a reducer (`(SELECT k) * MAX(x)`) is indistinguishable
            // from a row-varying column after flattening, hits the fail-safe default,
            // and is rejected for varying per row when it does not.
            if (statement.decide_objective) {
                collect_subqueries(statement.decide_objective);
            }
        }

        // Flatten now, even when this select node is itself being planned inside an
        // outer subquery's flattening. Ordinary clauses tolerate PlanSubqueries'
        // `is_outside_flattened` deferral because nothing reads their expressions
        // again until RecursiveDependentJoinPlanner has peeled the outer subquery.
        // A DECIDE clause does: canonicalization runs a few lines below, in this same
        // CreatePlan, and copies the constraint tree -- and copying a
        // BoundSubqueryExpression throws. Deferring here therefore fails any subquery
        // written inside a nested DECIDE, whether that subquery is itself a DECIDE or
        // a plain SELECT. Layer 4's input contract is a subquery-free bound tree, so
        // the flattening is not optional at this point.
        const bool outer_flatten_state = is_outside_flattened;
        is_outside_flattened = true;
        PlanSubqueries(statement.decide_constraints, root);
        PlanSubqueries(statement.decide_objective, root);
        is_outside_flattened = outer_flatten_state;

        // Read back what every scalar subquery became. The table-index sets give the
        // strict canonicalizer its planning-time evidence. The expression tags carry
        // the same semantic fact with the flattened value so it survives copies and
        // optimizer-generated re-canonicalization.
        auto collect_flattened_indexes = [](const vector<SourceSubquerySlot> &slots,
                                            unordered_set<idx_t> &out) {
            for (auto &entry : slots) {
                auto *slot = entry.slot;
                if (!slot || !*slot) {
                    continue;
                }
                // Identity, not value: a flattened subquery is the same column whatever
                // wraps it, so this peels past any cast (see StripCastsForIdentity).
                const Expression *flat = StripCastsForIdentity(**slot);
                if (flat->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    out.insert(flat->Cast<BoundColumnRefExpression>().binding.table_index);
                }
            }
        };
        unordered_set<idx_t> query_wide_table_indexes;
        unordered_set<idx_t> correlated_subquery_table_indexes;
        collect_flattened_indexes(uncorrelated_subquery_slots, query_wide_table_indexes);
        collect_flattened_indexes(correlated_subquery_slots, correlated_subquery_table_indexes);

        auto tag_flattened_values = [](const vector<SourceSubquerySlot> &slots,
                                       const string &tag) {
            for (auto &entry : slots) {
                auto *slot = entry.slot;
                if (!slot || !*slot) {
                    continue;
                }
                auto alias = (*slot)->GetAlias();
                if (!HasDecideTag(alias, tag)) {
                    AddDecideTag(alias, tag);
                }
                if (entry.source_fragment_id != DConstants::INVALID_INDEX) {
                    idx_t ignored;
                    if (!TryParseSourceFragmentTag(alias, ignored)) {
                        AddDecideTag(alias, MakeSourceFragmentTag(entry.source_fragment_id));
                    }
                }
                (*slot)->SetAlias(std::move(alias));
            }
        };
        tag_flattened_values(uncorrelated_subquery_slots, QUERY_WIDE_VALUE_TAG);
        tag_flattened_values(correlated_subquery_slots, ROW_VARYING_SUBQUERY_TAG);
        // Canonicalization: the single point at which the shape of a user-written
        // constraint is decided (decision terms left, data right). Runs here rather
        // than during binding because it needs bound expressions -- a decision
        // variable is exactly a BoundColumnRefExpression on decide_index -- and
        // because it must see the flattened form PlanSubqueries just produced.
        // Everything the optimizer emits later is canonicalized by
        // LogicalDecide::AddConstraint. Those two are the only call sites.
        string written_objective;
        string canonical_objective;
        {
            DecideCanonicalizer canonicalizer(context, statement.decide_index,
                                               statement.variable_scopes,
                                               std::move(query_wide_table_indexes),
                                               std::move(correlated_subquery_table_indexes));
            if (statement.decide_constraints) {
                statement.decide_constraints = canonicalizer.CanonicalizeTree(*statement.decide_constraints);
                canonicalizer.VerifyCanonical(*statement.decide_constraints);
                FinalizeConstraintSourceInfo(*statement.decide_constraints,
                                             statement.decide_constraint_sources,
                                             statement.decide_source_fragments,
                                             statement.entity_scopes);
            }
            // The objective is one side of a comparison, so it belongs to the same
            // boundary. It carries no relation to orient, which is the only part of
            // the constraint contract that does not apply to it.
            if (statement.decide_objective) {
                // The objective's counterpart to InitializeConstraintSourceInfo /
                // FinalizeConstraintSourceInfo: written on the way in, canonical on the
                // way out, captured around the one call that can change it. There is
                // exactly one objective, so these are two strings rather than a registry.
                written_objective = RenderDecideObjective(*statement.decide_objective,
                                                          statement.decide_source_fragments,
                                                          statement.entity_scopes);
                statement.decide_objective = canonicalizer.CanonicalizeObjective(
                    *statement.decide_objective, statement.objective_constant_offset);
                canonicalizer.VerifyCanonicalObjective(*statement.decide_objective);
                canonical_objective = RenderDecideObjective(*statement.decide_objective,
                                                            statement.decide_source_fragments,
                                                            statement.entity_scopes);
                if (canonical_objective == written_objective) {
                    canonical_objective.clear();
                }
            }
        }

        auto decide_op = make_uniq<LogicalDecide>(
            statement.decide_index,
            std::move(statement.decide_variables),
            std::move(statement.decide_constraints),
            statement.decide_sense,
            std::move(statement.decide_objective)
        );

        decide_op->num_auxiliary_vars = statement.num_auxiliary_vars;
        decide_op->objective_constant_offset = statement.objective_constant_offset;
        decide_op->is_boolean_var = std::move(statement.is_boolean_var);
        decide_op->entity_scopes = std::move(statement.entity_scopes);
        decide_op->variable_scopes = std::move(statement.variable_scopes);
        decide_op->constraint_sources = std::move(statement.decide_constraint_sources);
        decide_op->written_objective = std::move(written_objective);
        decide_op->canonical_objective = std::move(canonical_objective);
        decide_op->source_fragments = std::move(statement.decide_source_fragments);
        // Capture the user's column names off the BindContext, which is still live here
        // and is the authority name resolution itself used. Every source kind is covered
        // uniformly: a base table's catalog names, a `t(a, b, c)` alias list over
        // `(VALUES ...)`, a subquery's or a CTE's output names. Taken here rather than at
        // physical-plan time because the binding — and with it the only copy of an alias
        // list — is gone by then, leaving the plan's positional `col0` placeholders.
        for (auto &binding : bind_context.GetBindingsList()) {
            for (idx_t i = 0; i < binding->names.size(); i++) {
                decide_op->source_columns.push_back({ColumnBinding(binding->index, i), binding->names[i]});
            }
        }
        decide_op->entity_key_expressions = std::move(statement.entity_key_expressions);
        // ne_clause_labels, ABS aux vars, and MIN/MAX indicator links +
        // objective types are created by DecideOptimizer (runs after plan creation)
        decide_op->AddChild(std::move(root));
        root = std::move(decide_op);
    }

	if (!statement.aggregates.empty() || !statement.groups.group_expressions.empty()) {
		if (!statement.groups.group_expressions.empty()) {
			// visit the groups
			for (auto &group : statement.groups.group_expressions) {
				PlanSubqueries(group, root);
			}
		}
		// now visit all aggregate expressions
		for (auto &expr : statement.aggregates) {
			PlanSubqueries(expr, root);
		}
		// finally create the aggregate node with the group_index and aggregate_index as obtained from the binder
		auto aggregate = make_uniq<LogicalAggregate>(statement.group_index, statement.aggregate_index,
		                                             std::move(statement.aggregates));
		aggregate->groups = std::move(statement.groups.group_expressions);
		aggregate->groupings_index = statement.groupings_index;
		aggregate->grouping_sets = std::move(statement.groups.grouping_sets);
		aggregate->grouping_functions = std::move(statement.grouping_functions);

		aggregate->AddChild(std::move(root));
		root = std::move(aggregate);
	} else if (!statement.groups.grouping_sets.empty()) {
		// edge case: we have grouping sets but no groups or aggregates
		// this can only happen if we have e.g. select 1 from tbl group by ();
		// just output a dummy scan
		root = make_uniq_base<LogicalOperator, LogicalDummyScan>(statement.group_index);
	}

	if (statement.having) {
		PlanSubqueries(statement.having, root);
		auto having = make_uniq<LogicalFilter>(std::move(statement.having));

		having->AddChild(std::move(root));
		root = std::move(having);
	}

	if (!statement.windows.empty()) {
		auto win = make_uniq<LogicalWindow>(statement.window_index);
		win->expressions = std::move(statement.windows);
		// visit the window expressions
		for (auto &expr : win->expressions) {
			PlanSubqueries(expr, root);
		}
		D_ASSERT(!win->expressions.empty());
		win->AddChild(std::move(root));
		root = std::move(win);
	}

	if (statement.qualify) {
		PlanSubqueries(statement.qualify, root);
		auto qualify = make_uniq<LogicalFilter>(std::move(statement.qualify));

		qualify->AddChild(std::move(root));
		root = std::move(qualify);
	}

	for (idx_t i = statement.unnests.size(); i > 0; i--) {
		auto unnest_level = i - 1;
		auto entry = statement.unnests.find(unnest_level);
		if (entry == statement.unnests.end()) {
			throw InternalException("unnests specified at level %d but none were found", unnest_level);
		}
		auto &unnest_node = entry->second;
		auto unnest = make_uniq<LogicalUnnest>(unnest_node.index);
		unnest->expressions = std::move(unnest_node.expressions);
		// visit the unnest expressions
		for (auto &expr : unnest->expressions) {
			PlanSubqueries(expr, root);
		}
		D_ASSERT(!unnest->expressions.empty());
		unnest->AddChild(std::move(root));
		root = std::move(unnest);
	}

	for (auto &expr : statement.select_list) {
		PlanSubqueries(expr, root);
	}

	auto proj = make_uniq<LogicalProjection>(statement.projection_index, std::move(statement.select_list));
	auto &projection = *proj;
	proj->AddChild(std::move(root));
	root = std::move(proj);

	// finish the plan by handling the elements of the QueryNode
	root = VisitQueryNode(statement, std::move(root));

	// add a prune node if necessary
	if (statement.need_prune) {
		D_ASSERT(root);
		vector<unique_ptr<Expression>> prune_expressions;
		for (idx_t i = 0; i < statement.column_count; i++) {
			prune_expressions.push_back(make_uniq<BoundColumnRefExpression>(
			    projection.expressions[i]->return_type, ColumnBinding(statement.projection_index, i)));
		}
		auto prune = make_uniq<LogicalProjection>(statement.prune_index, std::move(prune_expressions));
		prune->AddChild(std::move(root));
		root = std::move(prune);
	}
	return root;
}

} // namespace duckdb
