#include "duckdb/optimizer/decide/direct/direct_row_checks.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

namespace {

//! Builds the windows of the row checks and the filter above them. The windows stand in three levels, each reading the
//! columns of the one below: level 0 reads the source (a value check's least message, a determination check's counts
//! and extremes per group), level 1 the group's verdict (the rank of the least failing group), level 2 the one message
//! every row reads (the least message, or the least failing group's message).
class RowCheckBuilder {
public:
	RowCheckBuilder(Optimizer &optimizer_p) : optimizer(optimizer_p), calc(optimizer_p.context) {
	}

	unique_ptr<LogicalOperator> Build(const vector<DirectRowCheck> &checks, unique_ptr<LogicalOperator> source) {
		auto source_columns = source->GetColumnBindings().size();
		vector<ExprPtr> messages;
		for (auto &check : checks) {
			messages.push_back(check.kind == DirectRowCheckKind::VALUE_IS_VALID ? ValueMessage(check)
			                                                                    : DeterminationMessage(check));
		}
		auto plan = std::move(source);
		for (auto &level : levels) {
			if (level.windows.empty()) {
				continue;
			}
			auto window = make_uniq<LogicalWindow>(level.table_index);
			window->expressions = std::move(level.windows);
			window->children.push_back(std::move(plan));
			plan = std::move(window);
		}
		vector<unique_ptr<Expression>> raised;
		for (auto &message : messages) {
			raised.push_back(DirectRaiseMessage(optimizer, calc, message));
		}
		auto filter = make_uniq<LogicalFilter>(DirectAllHold(calc, std::move(raised)));
		for (idx_t i = 0; i < source_columns; i++) {
			filter->projection_map.push_back(i);
		}
		filter->children.push_back(std::move(plan));
		filter->ResolveOperatorTypes();
		return std::move(filter);
	}

private:
	struct Level {
		idx_t table_index = DConstants::INVALID_INDEX;
		vector<unique_ptr<Expression>> windows;
	};
	static constexpr idx_t LEVEL_GROUP = 0, LEVEL_VERDICT = 1, LEVEL_MESSAGE = 2;

	Optimizer &optimizer;
	ExpressionBuilder calc;
	Level levels[3];

	//! A column reading `window`, placed on `level`.
	ExprPtr Place(idx_t level, unique_ptr<BoundWindowExpression> window) {
		auto &target = levels[level];
		if (target.table_index == DConstants::INVALID_INDEX) {
			target.table_index = optimizer.binder.GenerateTableIndex();
		}
		auto type = window->return_type;
		target.windows.push_back(std::move(window));
		return calc.Col(type, ColumnBinding(target.table_index, target.windows.size() - 1));
	}

	//! `value` on the rows `rows` holds (every row when null), NULL on the others, where it is not computed.
	ExprPtr OnRows(const ExprPtr &rows, const ExprPtr &value) const {
		return rows ? DirectOnKeptRows(calc, rows, value) : value;
	}

	//! A value check: the least message any row raises.
	ExprPtr ValueMessage(const DirectRowCheck &check) {
		return Place(LEVEL_GROUP, DirectWindowExtremum(optimizer, "min", check.invalid->Copy()));
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

	//! A determination check: the message of the least failing group, on every row.
	ExprPtr DeterminationMessage(const DirectRowCheck &check) {
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
		// The key is read on those rows alone, and a row whose key has a NULL in a NO_GROUP element is in no class.
		vector<ExprPtr> keys;
		auto in_class = looked_at;
		for (auto &element : check.key) {
			keys.push_back(OnRows(looked_at, calc.Of(*element.grouping)));
			if (element.no_group) {
				auto present = calc.If(calc.IsNull(keys.back()), calc.Bool(false), calc.Bool(true));
				in_class = in_class ? calc.If(in_class, present, calc.Bool(false)) : present;
			}
		}
		auto partitioned = [&](unique_ptr<BoundWindowExpression> window) {
			for (auto &key : keys) {
				window->partitions.push_back(calc.Out(key));
			}
			return window;
		};
		auto rows = Place(
		    LEVEL_GROUP, partitioned(DirectWindowMatchingCount(calc, calc.Out(in_class ? in_class : calc.Bool(true)))));
		auto bigint = [&](int64_t value) {
			return calc.Constant(Value::BIGINT(value));
		};
		ExprPtr failing, least, greatest, with_null;
		if (check.value) {
			auto value = OnRows(in_class, calc.Of(*check.value));
			auto non_null = Place(LEVEL_GROUP,
			                      partitioned(DirectWindowMatchingCount(
			                          calc, calc.Out(calc.If(calc.IsNull(value), calc.Bool(false), calc.Bool(true))))));
			least = Place(LEVEL_GROUP, partitioned(DirectWindowExtremum(optimizer, "min", calc.Out(value))));
			greatest = Place(LEVEL_GROUP, partitioned(DirectWindowExtremum(optimizer, "max", calc.Out(value))));
			with_null = calc.Cmp(ExpressionType::COMPARE_LESSTHAN, non_null, rows);
			// A class fails when it shows two values. NULL is a value of its own.
			failing = calc.If(
			    calc.Cmp(ExpressionType::COMPARE_EQUAL, non_null, bigint(0)), calc.Bool(false),
			    calc.If(with_null, calc.Bool(true), calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, least, greatest)));
		} else {
			failing = calc.Cmp(ExpressionType::COMPARE_GREATERTHAN, rows, bigint(1));
		}
		// The message, in the solver's text (DecideDeterminesText).
		vector<ExprPtr> arguments {calc.Constant(Value(check.text.Format()))};
		if (!check.key.empty()) {
			string names;
			vector<ExprPtr> labels {nullptr};
			for (idx_t i = 0; i < check.key.size(); i++) {
				auto &element = check.key[i];
				names += (i > 0 ? ", " : "") + StringUtil::Replace(element.name, "%", "%%") + " = %s";
				// A class is named by its least spelling in binary order, as a collated key's classes are.
				auto spelling = calc.Of(*element.label);
				if (spelling->return_type.id() == LogicalTypeId::VARCHAR) {
					// Concatenating drops a collation, so MIN compares the spellings as bytes.
					spelling = calc.If(calc.IsNull(spelling), calc.Constant(Value(LogicalType::VARCHAR)),
					                   calc.Fn("concat", spelling, calc.Constant(Value(""))));
				}
				labels.push_back(Literal(
				    Place(LEVEL_GROUP,
				          partitioned(DirectWindowExtremum(optimizer, "min", calc.Out(OnRows(in_class, spelling)))))));
			}
			labels[0] = calc.Constant(Value(names));
			arguments.push_back(calc.Fn("printf", labels));
		}
		if (check.value) {
			arguments.push_back(Literal(least));
			arguments.push_back(calc.If(with_null, calc.Constant(Value("NULL")), Literal(greatest)));
		}
		auto message = calc.Fn("printf", arguments);
		if (check.key.empty()) {
			return Place(
			    LEVEL_MESSAGE,
			    DirectWindowExtremum(optimizer, "min",
			                         calc.Out(calc.If(failing, message, calc.Constant(Value(LogicalType::VARCHAR))))));
		}
		// Several groups can fail; the rows of the least one by key value read its message.
		auto rank =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_RANK_DENSE, LogicalType::BIGINT, nullptr, nullptr);
		rank->start = WindowBoundary::UNBOUNDED_PRECEDING;
		rank->end = WindowBoundary::UNBOUNDED_FOLLOWING;
		rank->partitions.push_back(calc.Out(failing));
		for (auto &key : keys) {
			rank->orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, calc.Out(key));
		}
		auto first = calc.Cmp(ExpressionType::COMPARE_EQUAL, Place(LEVEL_VERDICT, std::move(rank)), bigint(1));
		auto none = calc.Constant(Value(LogicalType::VARCHAR));
		auto named = calc.If(failing, calc.If(first, message, none), none);
		return Place(LEVEL_MESSAGE, DirectWindowExtremum(optimizer, "min", calc.Out(named)));
	}
};

} // namespace

unique_ptr<LogicalOperator> DirectAddRowChecks(Optimizer &optimizer, const vector<DirectRowCheck> &checks,
                                               unique_ptr<LogicalOperator> source) {
	if (checks.empty()) {
		return source;
	}
	return RowCheckBuilder(optimizer).Build(checks, std::move(source));
}

} // namespace duckdb
