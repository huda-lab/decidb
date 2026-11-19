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
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

namespace {

//! One check's plan over its own read of the source: an aggregate that comes to one row, whose one column is true when
//! the check holds on every row (a row it refuses raises where the check is evaluated).
struct CheckPlan {
	unique_ptr<LogicalOperator> plan;
	ColumnBinding holds;
};

//! Builds each check as aggregates over a read of the source, pruned by the optimizer to the columns the check reads.
class RowCheckBuilder {
public:
	RowCheckBuilder(Optimizer &optimizer_p, DirectSourceReader &reader_p)
	    : optimizer(optimizer_p), reader(reader_p), calc(optimizer_p.context) {
	}

	CheckPlan Build(const DirectRowCheck &check) {
		read = reader.ReadSource();
		return ValueCheck(check);
	}

private:
	Optimizer &optimizer;
	DirectSourceReader &reader;
	ExpressionBuilder calc;
	//! The read the check being built aggregates.
	DirectSourceReader::Read read;

	//! An expression over the source, re-pointed at the read.
	ExprPtr Over(const Expression &expression) const {
		return calc.Wrap(DirectOverProjection(expression, reader.SourceBindings(), read.table_index));
	}

	unique_ptr<Expression> Aggregate(const string &name, vector<ExprPtr> arguments) const {
		vector<unique_ptr<Expression>> children;
		for (auto &argument : arguments) {
			children.push_back(calc.Out(argument));
		}
		return optimizer.BindAggregateFunction(name, std::move(children));
	}

	//! A value check: every row evaluates it, and the one that finds a value the model cannot read raises.
	CheckPlan ValueCheck(const DirectRowCheck &check) {
		vector<unique_ptr<Expression>> aggregates;
		aggregates.push_back(Aggregate("bool_and", {Over(*check.invalid)}));
		auto aggregate = DirectAggregateOver(optimizer, std::move(read.plan), {}, std::move(aggregates));
		auto holds = ColumnBinding(aggregate->aggregate_index, 0);
		return {std::move(aggregate), holds};
	}
};

} // namespace

void DirectAddRowChecks(Optimizer &optimizer, DirectSourceReader &reader, const vector<DirectRowCheck> &checks) {
	if (checks.empty()) {
		return;
	}
	ExpressionBuilder calc(optimizer.context);
	RowCheckBuilder builder(optimizer, reader);
	unique_ptr<LogicalOperator> verdicts;
	vector<unique_ptr<Expression>> checked;
	for (auto &check : checks) {
		auto built = builder.Build(check);
		// A source with no row has no value to refuse: the aggregate is NULL there and the check holds.
		checked.push_back(calc.Out(calc.Coalesce({calc.Col(LogicalType::BOOLEAN, built.holds), calc.Bool(true)})));
		verdicts =
		    verdicts ? LogicalCrossProduct::Create(std::move(verdicts), std::move(built.plan)) : std::move(built.plan);
	}
	// One row: true once every check has been evaluated, or the error one of them raised.
	vector<unique_ptr<Expression>> verdict_columns;
	verdict_columns.push_back(DirectAllHold(calc, std::move(checked)));
	auto verdict = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(verdict_columns));
	verdict->children.push_back(std::move(verdicts));
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
