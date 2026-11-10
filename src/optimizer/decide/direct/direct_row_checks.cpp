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
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

namespace {

//! One check's plan over its own read of the source: an aggregate that comes to one row, whose one column is the
//! message the check raises, NULL when it holds.
struct CheckPlan {
	unique_ptr<LogicalOperator> plan;
	ColumnBinding message;
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


	//! A value check: the least message any row raises.
	CheckPlan ValueCheck(const DirectRowCheck &check) {
		vector<unique_ptr<Expression>> aggregates;
		aggregates.push_back(Aggregate("min", {Over(*check.invalid)}));
		auto aggregate = DirectAggregateOver(optimizer, std::move(read.plan), {}, std::move(aggregates));
		auto message = ColumnBinding(aggregate->aggregate_index, 0);
		return {std::move(aggregate), message};
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
	vector<unique_ptr<Expression>> raised;
	for (auto &check : checks) {
		auto built = builder.Build(check);
		raised.push_back(DirectRaiseMessage(optimizer, calc, calc.Col(LogicalType::VARCHAR, built.message)));
		verdicts =
		    verdicts ? LogicalCrossProduct::Create(std::move(verdicts), std::move(built.plan)) : std::move(built.plan);
	}
	// One row: true, or the error of the first check that fails, in the order given.
	vector<unique_ptr<Expression>> verdict_columns;
	verdict_columns.push_back(DirectAllHold(calc, std::move(raised)));
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
	// Each read of a reducer shows the reducer's source spelling, rendered once the reducers before it are named, so an
	// outer reducer shows named inner results.
	const auto first_fragment = decide.source_fragments.size();
	for (auto &retained : decide.retained_bound_aggregates) {
		auto display = retained.aggregate.function->Copy();
		for (auto &argument : retained.aggregate.arguments) {
			display->Cast<BoundAggregateExpression>().children.push_back(argument->Copy());
		}
		decide.NameRetainedRecordReads(*display, first_fragment);
		decide.source_fragments.push_back(
		    RenderDecideSource(*display, decide.source_fragments, decide.keyed_scopes, &decide.keys));
	}
	decide.EnumerateExpressions(
	    [&](unique_ptr<Expression> *expression) { decide.NameRetainedRecordReads(**expression, first_fragment); });
	decide.bound_aggregates.clear();
}

} // namespace duckdb
