#include "duckdb/optimizer/decide/direct/direct_row_checks.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_tables.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

void DirectAddRowChecks(Optimizer &optimizer, DirectSourceReader &reader, const vector<DirectRowCheck> &checks) {
	if (checks.empty()) {
		return;
	}
	ExpressionBuilder calc(optimizer.context);
	auto read = reader.ReadSource();
	// Each row makes the checks in the solver's order, so a row that fails two raises the first one's error.
	vector<unique_ptr<Expression>> chain;
	for (auto &check : checks) {
		chain.push_back(DirectOverProjection(*check.invalid, reader.SourceBindings(), read.table_index));
	}
	vector<unique_ptr<Expression>> children;
	children.push_back(DirectAllHold(calc, std::move(chain)));
	vector<unique_ptr<Expression>> aggregates;
	aggregates.push_back(optimizer.BindAggregateFunction("bool_and", std::move(children)));
	auto aggregate = DirectAggregateOver(optimizer, std::move(read.plan), {}, std::move(aggregates));
	// One row: true once every row has been checked, or the error a row raised. A source with no row has no value to
	// refuse: the aggregate is NULL there and the checks hold.
	vector<unique_ptr<Expression>> verdict_columns;
	verdict_columns.push_back(calc.Out(calc.Coalesce(
	    {calc.Col(LogicalType::BOOLEAN, ColumnBinding(aggregate->aggregate_index, 0)), calc.Bool(true)})));
	auto verdict = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(verdict_columns));
	verdict->children.push_back(std::move(aggregate));
	reader.Gate(std::move(verdict));
}

void DirectRetainRowChecks(LogicalDecide &decide, const vector<DirectRowCheck> &checks) {
	if (checks.empty() && decide.bound_aggregates.empty()) {
		return;
	}
	// The tree evaluates the checks and the records on the rows it reads (language/guide.md §6.1), so each is kept on
	// the node, and the source stays the original relation, with no aggregate join or check gate in front of it.
	for (auto &check : checks) {
		decide.retained_row_checks.push_back(check.invalid->Copy());
	}
	for (auto &aggregate : decide.bound_aggregates) {
		LogicalDecide::RetainedBoundAggregate retained;
		retained.placeholder = make_uniq<BoundColumnRefExpression>(aggregate.function->return_type, aggregate.column);
		retained.aggregate.column = aggregate.column;
		retained.aggregate.function = aggregate.function->Copy();
		retained.aggregate.reads = aggregate.reads ? aggregate.reads->Copy() : nullptr;
		retained.aggregate.key = aggregate.key;
		retained.aggregate.counts_classes = aggregate.counts_classes;
		retained.aggregate.class_key = aggregate.class_key;
		for (auto &argument : aggregate.arguments) {
			retained.aggregate.arguments.push_back(argument->Copy());
		}
		decide.retained_bound_aggregates.push_back(std::move(retained));
	}
	// A read of a reducer is quoted as the reducer is written: the bound-side rewrite gave its placeholder the
	// reducer's location (DecideReplaces).
	decide.bound_aggregates.clear();
}

} // namespace duckdb
