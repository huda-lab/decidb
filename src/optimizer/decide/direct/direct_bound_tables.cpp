#include "duckdb/optimizer/decide/direct/direct_bound_tables.hpp"

#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"

namespace duckdb {

namespace {

//! Builds the side table of each bound aggregate over its own ungated read of the source.
class BoundTableBuilder {
public:
	BoundTableBuilder(Optimizer &optimizer_p, DirectSourceReader &reader_p, const LogicalDecide &decide_p)
	    : optimizer(optimizer_p), reader(reader_p), decide(decide_p), calc(optimizer_p.context) {
	}

	void Add(const DecideBoundAggregate &record) {
		// The read has the tables added before this one, which the record may read.
		read = reader.ReadSource();
		vector<ExprPtr> keys;
		vector<unique_ptr<Expression>> source_keys;
		for (auto &element : DecideKeyElements(decide.keys, record.key)) {
			keys.push_back(Over(element.Grouping()));
			source_keys.push_back(element.Grouping().Copy());
		}
		auto definition = record.counts_classes ? CountedByClass(record, keys) : Plain(record, keys);
		reader.AddSideTable(std::move(definition), std::move(source_keys), record.column);
	}

private:
	Optimizer &optimizer;
	DirectSourceReader &reader;
	const LogicalDecide &decide;
	ExpressionBuilder calc;
	//! The read the table being built aggregates.
	DirectSourceReader::Read read;

	//! An expression over the source, re-pointed at the read.
	ExprPtr Over(const Expression &expression) const {
		return calc.Wrap(DirectOverProjection(expression, reader.SourceBindings(), read.table_index));
	}

	//! The first value of `value` in a group (ARBITRARY), a NULL included.
	unique_ptr<Expression> Arbitrary(const ExprPtr &value) const {
		vector<unique_ptr<Expression>> children;
		children.push_back(calc.Out(value));
		return optimizer.BindAggregateFunction("arbitrary", std::move(children));
	}

	//! The reducer's function over the rows it reads, in each group of its key.
	unique_ptr<LogicalOperator> Plain(const DecideBoundAggregate &record, const vector<ExprPtr> &keys) {
		ExprPtr reads = record.reads ? Over(*record.reads) : nullptr;
		ExprPtr argument = record.argument ? DirectOnKeptRows(calc, reads, Over(*record.argument)) : nullptr;
		vector<unique_ptr<Expression>> aggregates;
		aggregates.push_back(DirectAggregateOf(*record.function, argument ? calc.Out(argument) : nullptr,
		                                       reads ? calc.Out(reads) : nullptr));
		return DirectAggregateOver(optimizer, std::move(read.plan), keys, std::move(aggregates));
	}

	//! The reducer's function over the classes of its `per` key that its group has: the rows it reads, with a class
	//! (no NULL in a NO_GROUP element), are grouped by key and class to read the class's body, and the function runs
	//! over the classes of each group. Only SUM, AVG, MIN and MAX take a `per`, so a group with no class gives NULL
	//! whether its row is in the table or not.
	unique_ptr<LogicalOperator> CountedByClass(const DecideBoundAggregate &record, const vector<ExprPtr> &keys) {
		D_ASSERT(record.argument);
		ExprPtr counted = record.reads ? Over(*record.reads) : nullptr;
		vector<ExprPtr> classes = keys;
		for (auto &element : DecideKeyElements(decide.keys, record.class_key)) {
			classes.push_back(Over(element.Grouping()));
			// A row whose written `per` column is NULL is in no class, so it is not counted.
			if (element.null_mode == DecideKeyNullMode::NO_GROUP) {
				auto present = calc.If(calc.IsNull(Over(*element.expression)), calc.Bool(false), calc.Bool(true));
				counted = counted ? calc.If(counted, present, calc.Bool(false)) : present;
			}
		}
		unique_ptr<LogicalOperator> rows = std::move(read.plan);
		if (counted) {
			auto filter = make_uniq<LogicalFilter>(calc.Out(counted));
			filter->children.push_back(std::move(rows));
			rows = std::move(filter);
		}
		// The key determines the body (a scope check), so any row of a class reads it.
		vector<unique_ptr<Expression>> body;
		body.push_back(Arbitrary(Over(*record.argument)));
		auto per_class = DirectAggregateOver(optimizer, std::move(rows), classes, std::move(body));
		auto value_type = per_class->expressions[0]->return_type;
		vector<ExprPtr> by_key;
		for (idx_t i = 0; i < keys.size(); i++) {
			by_key.push_back(calc.Col(keys[i]->return_type, ColumnBinding(per_class->group_index, i)));
		}
		auto class_value = calc.Col(value_type, ColumnBinding(per_class->aggregate_index, 0));
		vector<unique_ptr<Expression>> aggregates;
		aggregates.push_back(DirectAggregateOf(*record.function, calc.Out(class_value), nullptr));
		return DirectAggregateOver(optimizer, std::move(per_class), by_key, std::move(aggregates));
	}
};

} // namespace

void DirectAddBoundTables(Optimizer &optimizer, DirectSourceReader &reader, LogicalDecide &decide) {
	BoundTableBuilder builder(optimizer, reader, decide);
	for (auto &record : decide.bound_aggregates) {
		builder.Add(record);
	}
	decide.bound_aggregates.clear();
}

} // namespace duckdb
