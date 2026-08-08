#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"
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
        // A per-row bound whose RHS is an UNCORRELATED scalar subquery (`x <= (SELECT 5)`)
        // is one shared editable cap, not row data — but PlanSubqueries below flattens it
        // into a cross-joined column ref indistinguishable from a real per-row column. The
        // correlation info only exists NOW, before flattening, so detect those comparisons
        // here (their nodes survive the in-place rewrite) and stamp the rewritten RHS with
        // SHARED_SCALAR_SUBQUERY_TAG afterward so infeasible diagnosis treats them as a
        // single loosenable bound. Correlated subqueries are left untagged (genuinely per-row).
        vector<BoundComparisonExpression *> shared_subquery_comps;
        std::function<void(Expression &)> collect_shared_subquery_rhs = [&](Expression &e) {
            if (e.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
                auto &comp = e.Cast<BoundComparisonExpression>();
                Expression *rhs = comp.right.get();
                while (rhs->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
                    rhs = rhs->Cast<BoundCastExpression>().child.get();
                }
                if (rhs->GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
                    auto &subq = rhs->Cast<BoundSubqueryExpression>();
                    if (subq.subquery_type == SubqueryType::SCALAR && !subq.IsCorrelated()) {
                        shared_subquery_comps.push_back(&comp);
                    }
                }
            }
            ExpressionIterator::EnumerateChildren(e, collect_shared_subquery_rhs);
        };
        if (statement.decide_constraints) {
            collect_shared_subquery_rhs(*statement.decide_constraints);
        }

        PlanSubqueries(statement.decide_constraints, root);
        PlanSubqueries(statement.decide_objective, root);

        // Subquery RHS is now a flattened column ref (PlanSubqueries rewrote it in place,
        // leaving the comparison node — and our pointers to it — valid). Tag it shared.
        for (auto *comp : shared_subquery_comps) {
            comp->right->SetAlias(SHARED_SCALAR_SUBQUERY_TAG);
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
        decide_op->entity_key_expressions = std::move(statement.entity_key_expressions);
        // ne_indicator_indices, ABS aux vars, and MIN/MAX indicator links +
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
