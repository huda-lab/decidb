#include "duckdb/optimizer/decide/direct/direct_row_checks.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
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
		return check.kind == DirectRowCheckKind::VALUE_IS_VALID ? ValueCheck(check) : DeterminationCheck(check);
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

	//! A hash aggregate of `child` on `groups`; with none, one row however many rows `child` has.
	unique_ptr<LogicalAggregate> AggregateOf(unique_ptr<LogicalOperator> child, vector<ExprPtr> groups,
	                                         vector<unique_ptr<Expression>> aggregates) const {
		auto &binder = optimizer.binder;
		auto aggregate = make_uniq<LogicalAggregate>(binder.GenerateTableIndex(), binder.GenerateTableIndex(),
		                                             std::move(aggregates));
		for (auto &group : groups) {
			aggregate->groups.push_back(calc.Out(group));
		}
		aggregate->children.push_back(std::move(child));
		return aggregate;
	}

	ExprPtr Result(const LogicalAggregate &aggregate, idx_t i) const {
		return calc.Col(aggregate.expressions[i]->return_type, ColumnBinding(aggregate.aggregate_index, i));
	}

	//! A value check: the least message any row raises.
	CheckPlan ValueCheck(const DirectRowCheck &check) {
		vector<unique_ptr<Expression>> aggregates;
		aggregates.push_back(Aggregate("min", {Over(*check.invalid)}));
		auto aggregate = AggregateOf(std::move(read.plan), {}, std::move(aggregates));
		auto message = ColumnBinding(aggregate->aggregate_index, 0);
		return {std::move(aggregate), message};
	}

	//! `value` as a message writes it: 'East', 10, NULL (DecideLiteralIsBare).
	ExprPtr Literal(const ExprPtr &value) const {
		auto text = calc.Cast(value, LogicalType::VARCHAR);
		if (!DecideLiteralIsBare(value->return_type)) {
			auto quote = calc.Constant(Value("'"));
			auto doubled = calc.Fn("replace", {text, quote, calc.Constant(Value("''"))});
			text = calc.Fn("concat", {quote, doubled, quote});
		}
		return calc.If(calc.IsNull(value), calc.Constant(Value("NULL")), text);
	}

	//! A determination check: an aggregate per class of the key counts the class's rows and finds its least and
	//! greatest value, and a second aggregate over the classes that show more than one takes the message of the least
	//! of them by key value.
	CheckPlan DeterminationCheck(const DirectRowCheck &check) {
		// The rows the check looks at: the clause's `when`, then the reducer's own, which is read on the rows the first
		// keeps.
		ExprPtr looked_at;
		if (check.rows) {
			looked_at = calc.Wrap(DirectActiveWhen(calc, check.rows.get()));
		}
		if (check.filter) {
			auto own = calc.Wrap(DirectActiveWhen(calc, check.filter.get()));
			looked_at = looked_at ? calc.If(looked_at, own, calc.Bool(false)) : own;
		}
		if (looked_at) {
			looked_at = Over(*looked_at);
		}
		// The key is read on those rows alone, and a row whose key has a NULL in a NO_GROUP element is in no class.
		vector<ExprPtr> keys;
		auto in_class = looked_at;
		for (auto &element : check.key) {
			auto key = Over(*element.grouping);
			keys.push_back(key);
			if (element.no_group) {
				auto read_key = looked_at ? DirectOnKeptRows(calc, looked_at, key) : key;
				auto present = calc.If(calc.IsNull(read_key), calc.Bool(false), calc.Bool(true));
				in_class = in_class ? calc.If(in_class, present, calc.Bool(false)) : present;
			}
		}
		unique_ptr<LogicalOperator> rows = std::move(read.plan);
		if (in_class) {
			auto filter = make_uniq<LogicalFilter>(calc.Out(in_class));
			filter->children.push_back(std::move(rows));
			rows = std::move(filter);
		}
		// Per class: its rows, and of a value its non-NULL rows, least and greatest, and the least spelling of each key
		// element, which names the class.
		vector<unique_ptr<Expression>> aggregates;
		aggregates.push_back(Aggregate("count_star", {}));
		if (check.value) {
			auto value = Over(*check.value);
			aggregates.push_back(Aggregate("count", {value}));
			aggregates.push_back(Aggregate("min", {value}));
			aggregates.push_back(Aggregate("max", {value}));
		}
		for (auto &element : check.key) {
			// A class is named by its least spelling in binary order, as a collated key's classes are.
			auto spelling = Over(*element.label);
			if (spelling->return_type.id() == LogicalTypeId::VARCHAR) {
				// Concatenating drops a collation, so MIN compares the spellings as bytes.
				spelling = calc.If(calc.IsNull(spelling), calc.Constant(Value(LogicalType::VARCHAR)),
				                   calc.Fn("concat", spelling, calc.Constant(Value(""))));
			}
			aggregates.push_back(Aggregate("min", {spelling}));
		}
		auto classes = AggregateOf(std::move(rows), keys, std::move(aggregates));
		auto key_index = classes->group_index;
		idx_t column = 0;
		auto count = Result(*classes, column++);
		auto bigint = [&](int64_t value) {
			return calc.Constant(Value::BIGINT(value));
		};
		ExprPtr failing, least, greatest, with_null;
		if (check.value) {
			auto non_null = Result(*classes, column++);
			least = Result(*classes, column++);
			greatest = Result(*classes, column++);
			with_null = calc.Cmp(ExpressionType::COMPARE_LESSTHAN, non_null, count);
			// A class fails when it shows two values. NULL is a value of its own.
			failing = calc.If(
			    calc.Cmp(ExpressionType::COMPARE_EQUAL, non_null, bigint(0)), calc.Bool(false),
			    calc.If(with_null, calc.Bool(true), calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, least, greatest)));
		} else {
			failing = calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, count, bigint(1));
		}
		// The message, in the solver's text (DecideDeterminesText).
		vector<ExprPtr> arguments {calc.Constant(Value(check.text.Format()))};
		if (!check.key.empty()) {
			string names;
			vector<ExprPtr> labels {nullptr};
			for (idx_t i = 0; i < check.key.size(); i++) {
				names += (i > 0 ? ", " : "") + StringUtil::Replace(check.key[i].name, "%", "%%") + " = %s";
				labels.push_back(Literal(Result(*classes, column++)));
			}
			labels[0] = calc.Constant(Value(names));
			arguments.push_back(calc.Fn("printf", labels));
		}
		if (check.value) {
			arguments.push_back(Literal(least));
			arguments.push_back(calc.If(with_null, calc.Constant(Value("NULL")), Literal(greatest)));
		}
		auto message = calc.Fn("printf", arguments);
		// The failing classes alone reach the second aggregate, so only theirs is written.
		auto failing_classes = make_uniq<LogicalFilter>(calc.Out(failing));
		failing_classes->children.push_back(std::move(classes));
		vector<unique_ptr<Expression>> first;
		if (check.key.empty()) {
			first.push_back(Aggregate("min", {message}));
		} else {
			// Several classes can fail; the least by key value names its message. A struct orders its fields as ORDER
			// BY does, NULL last, and is never NULL itself, so a class whose key is NULL is ordered too.
			vector<ExprPtr> fields;
			for (idx_t i = 0; i < keys.size(); i++) {
				auto field = calc.Col(keys[i]->return_type, ColumnBinding(key_index, i));
				field->SetAlias("k" + std::to_string(i));
				fields.push_back(field);
			}
			first.push_back(Aggregate("arg_min", {message, calc.Fn("struct_pack", fields)}));
		}
		auto named = AggregateOf(std::move(failing_classes), {}, std::move(first));
		auto binding = ColumnBinding(named->aggregate_index, 0);
		return {std::move(named), binding};
	}
};

} // namespace

bool DirectAddRowChecks(Optimizer &optimizer, LogicalOperator &root, const vector<DirectRowCheck> &checks,
                        LogicalDecide &decide) {
	if (checks.empty()) {
		return false;
	}
	ExpressionBuilder calc(optimizer.context);
	// Each check reads a few columns of the source, so a copy of every column does not pay for its reads. Recomputing
	// a chain also keeps the source's own bindings for the rows' read.
	DirectSourceReader reader(optimizer, std::move(decide.children[0]), checks.size() + 1, DirectSourceCopy::NEVER);
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
	auto verdict_index = optimizer.binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> verdict_columns;
	verdict_columns.push_back(DirectAllHold(calc, std::move(raised)));
	auto verdict = make_uniq<LogicalProjection>(verdict_index, std::move(verdict_columns));
	verdict->children.push_back(std::move(verdicts));

	// The rows: a recomputed source is itself, under its own bindings; a materialized one is read like the checks.
	auto old_bindings = reader.SourceBindings();
	vector<ColumnBinding> new_bindings;
	auto rows = reader.TakeRecomputedSource();
	if (!rows) {
		auto read = reader.ReadSource();
		rows = std::move(read.plan);
		for (idx_t i = 0; i < old_bindings.size(); i++) {
			new_bindings.emplace_back(read.table_index, i);
		}
		decide.RepointSourceColumns(reader.Source(), new_bindings);
	}
	// The verdict is one row joined to every row; it is usually the build side, so it raises before any row passes,
	// as a scalar subquery's does. Correctness does not hang on that: a later pass may flip the sides, and the checks
	// read the same rows either way. The filter reads it, so no pass prunes it, and passes on exactly the rows'
	// columns.
	auto gate = make_uniq<LogicalFilter>(calc.Out(calc.Col(LogicalType::BOOLEAN, ColumnBinding(verdict_index, 0))));
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		gate->projection_map.push_back(i);
	}
	gate->children.push_back(LogicalCrossProduct::Create(std::move(rows), std::move(verdict)));
	auto &source = decide.children[0];
	source = reader.Finish(std::move(gate));
	source->ResolveOperatorTypes();
	if (new_bindings.empty()) {
		return false;
	}
	// The operators above read the source's columns where the read now has them.
	ColumnBindingReplacer replacer;
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		replacer.replacement_bindings.emplace_back(old_bindings[i], new_bindings[i]);
	}
	replacer.stop_operator = source.get();
	replacer.VisitOperator(root);
	return true;
}

} // namespace duckdb
