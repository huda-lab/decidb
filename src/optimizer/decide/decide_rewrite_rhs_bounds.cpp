//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/optimizer/decide/decide_rewrite_rhs_bounds.cpp
//
// The bound-side evaluator. See decide_optimizer.cpp.
//
// A bound is data: `SUM(x) <= SUM(cap)`, `SUM(x) BY (g) <= cap`. Every value a bound needs that is not one row's
// value is computed once, as a column of the DECIDE source:
//   - each reducer on a bound (`sum(cap)`, `sum(per D: cap) by (g)`) is recorded (LogicalDecide::bound_aggregates), and
//     the bound reads a placeholder column where its value goes. The path builds the aggregate as a side table of
//     the DECIDE source, one row per group, joined back to the rows by the group key (DirectAddBoundTables), so the
//     surrounding arithmetic (`COUNT(*) * k`) is evaluated in DuckDB's own types. A reducer inside a reducer's
//     argument is recorded first, grouped by its path key over the rows its path keeps (language/guide.md §5.5), so
//     the one around it reads its value as a column of an earlier table;
//   - the tightest bound of a clause in the aggregate shape: such a clause is one constraint per group, and the
//     constraint on each kept row of the group holds when the tightest one does (paper §3.2.1), so `<=` takes the
//     group's MIN and `>=` its MAX, recorded as a side table too; `=` must have one value per group.
// The solver (PhysicalDecide) and the direct-solve rules then read plain columns, each with one value on every row of
// its clause's group, and neither reduces a bound.
//
// A frame selector's values are columns execution computes from the rows it selects
// (DecideFrameReads::columns), so no side table can read one: a row-wise bound reads a frame as it reads a
// column, as does a bound over groups that has one value per group, which execution checks as it reads it; one that
// takes its group's tightest value execution takes too (FRAME_TIGHTEST_BOUND_TAG, D85), and a reducer over data whose
// argument reads one execution computes (MakeFrameAggregate, LogicalDecide::frame_aggregates).
//
// The values the side tables hold are checked by VALUE_IS_VALID checks appended to the DECIDE node's scope checks, so a
// NaN, or an `=` bound with two values in a group, is reported after the scope checks, as the bound's reader would
// report it. A NULL bound is no value: the reducer skips it as SQL's does, and a group whose tightest bound
// is NULL has no instance, which the bound's reader skips. A side table itself raises nothing.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include <algorithm>

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"
#include "duckdb/optimizer/decide/decide_value_checks.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

namespace {

//! The bound-side rewrite of one DECIDE node: every value a bound needs that is not one row's value is replaced by a
//! placeholder column, and the reducers behind the placeholders are recorded (LogicalDecide::bound_aggregates), not
//! computed.
class RhsBoundRewriter {
public:
	RhsBoundRewriter(Optimizer &optimizer_p, LogicalDecide &decide_p)
	    : optimizer(optimizer_p), decide(decide_p), calc(optimizer_p.context) {
	}

	void Run() {
		// A clause or an objective a value was replaced in goes back through the canonicalizing boundary: a column
		// stands where a reducer stood, which the canonicalizer reads as a value of the row.
		vector<BoundDecideClause *> rewritten;
		decide.EnumerateBodies([&](DecideOwnedBody &body) {
			if (body.owner != DecideOwnedBody::Owner::CLAUSE && body.owner != DecideOwnedBody::Owner::OBJECTIVE) {
				return;
			}
			auto written = body.expr->Copy();
			if (body.owner == DecideOwnedBody::Owner::CLAUSE) {
				ReadClause(*body.clause);
			} else {
				ReadObjective(body.expr, body.rows);
			}
			if (!Expression::Equals(*written, *body.expr)) {
				rewritten.push_back(body.clause.get());
			}
		});
		for (auto clause : rewritten) {
			if (clause == &decide.bound.objective) {
				decide.SetObjective(optimizer.context, std::move(decide.bound.objective));
			} else {
				decide.CanonicalizeClause(optimizer.context, *clause);
			}
		}
	}

private:
	//! A reducer's column and the `by` key it has one value per group of.
	struct ReducerColumn {
		ColumnBinding binding;
		vector<const Expression *> keys;
	};

	//! A `per K:` groups no reducer: its clause is read like any other. Its key says which rows are in a class.
	void ReadClause(BoundDecideClause &clause) {
		if (clause.body->GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			in_objective = false;
			when = clause.scope.when.get();
			class_key = clause.scope.class_key;
			auto &comparison = clause.body->Cast<BoundComparisonExpression>();
			read_as = DecideValueRole::BOUND;
			RewriteBound(clause);
			read_as = DecideValueRole::COEFFICIENT;
			ReplaceDataUnderDecisions(comparison.left, nullptr);
		}
	}

	//! The objective, under its `when c:`: an objective is produced once, so it has no `per` key. Its condition holds
	//! no reducer.
	void ReadObjective(unique_ptr<Expression> &expr, const Expression *objective_when) {
		in_objective = true;
		when = objective_when;
		class_key = DConstants::INVALID_INDEX;
		read_as = DecideValueRole::COEFFICIENT;
		ReplaceDataUnderDecisions(expr, nullptr);
	}

	//! A reducer over data alone inside one over decisions is a value of its path's group (language/guide.md §5.5), as
	//! it is inside one on a bound: its column, read as a coefficient, which must be finite. `path` holds the reducers
	//! over decisions around `expr`; null outside every one, where canonicalization left only a reducer over data
	//! inside a function of a decision (`abs(x - max(t.v) by (t.g))`): a value of the clause's rows, as on a bound.
	void ReplaceDataUnderDecisions(unique_ptr<Expression> &expr, const DecideReducerPath *path) {
		idx_t frame;
		if (TryGetDecideFrame(*expr, frame)) {
			return;
		}
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			ExpressionIterator::EnumerateChildren(
			    *expr, [&](unique_ptr<Expression> &child) { ReplaceDataUnderDecisions(child, path); });
			return;
		}
		auto &reducer = expr->Cast<BoundAggregateExpression>();
		if (reducer.children.empty() || !BoundExpressionReferencesDecide(*reducer.children[0], decide.decide_index)) {
			expr = MakeReducerColumn(reducer, path ? *path : DecideReducerPath());
			return;
		}
		auto below = path ? path->Below(reducer) : DecideReducerPath().Below(reducer);
		ReplaceDataUnderDecisions(reducer.children[0], &below);
		// Its filter tests its argument's data for NULL (DecideCanonicalizer::SkipNullData), a reducer among it.
		if (reducer.filter) {
			ReplaceDataUnderDecisions(reducer.filter, &below);
		}
	}

	//! A clause in the aggregate shape is one constraint per group. Its data bound is checked on every kept row, before
	//! any value is read, and one that may differ between the kept rows of a group is read as its tightest value. `<>`
	//! keeps every value: each one is excluded. A query-wide value has one value already. A row-wise bound is checked
	//! by the solver as it reads it.
	void RewriteBound(BoundDecideClause &clause) {
		auto &comparison = clause.body->Cast<BoundComparisonExpression>();
		// Canonicalization put every decision on the left, so the right is data.
		bool aggregate_data_bound =
		    !comparison.right->IsFoldable() && clause.shape == CanonicalConstraintClass::AGGREGATE;
		bool query_wide = clause.query_wide_bound;
		if (!comparison.right->IsFoldable() && !query_wide) {
			clause.bound_read_shape = clause.shape;
			clause.bound_read_type = comparison.type;
		}
		auto text = Written(*comparison.right);
		auto location = comparison.right->GetQueryLocation();
		auto frame = FrameText(*comparison.right);
		ReplaceReducers(comparison.right);
		if (!aggregate_data_bound) {
			return;
		}
		bool reduces = comparison.type != ExpressionType::COMPARE_NOTEQUAL && !query_wide &&
		               !OneValuePerGroup(*comparison.right, KeyColumns(clause.group_key));
		if (!frame.empty()) {
			// Execution reads the bound, and its NaN, as it reads a row-wise one; one that may differ within a group
			// it reads as the group's tightest value (D85).
			if (reduces) {
				AddDecideTag(comparison.alias, FRAME_TIGHTEST_BOUND_TAG);
			}
			return;
		}
		if (reduces) {
			InClassOnly(comparison.right);
		}
		AddValueCheck(InvalidValueCheck(*comparison.right, text, SpanOf(location), Kept()));
		if (!reduces) {
			return;
		}
		// An `=` bound takes one value in each group, which its check states ahead of the value read; the NaN check
		// stands before it, so it reads valid values.
		if (comparison.type == ExpressionType::COMPARE_EQUAL) {
			AddDisagreementCheck(clause.group_key, *comparison.right, text, SpanOf(location),
			                     OwnWhens(*comparison.left));
		}
		auto function = DecideIsUpperBound(comparison.type) ? "min" : "max";
		comparison.right = calc.Out(InKey(clause.group_key, RecordOver(function, *comparison.right, clause.group_key)));
		// The bound is replaced by a column of its value, which a message quotes as it quotes the bound.
		if (location.IsValid()) {
			comparison.right->SetQueryLocation(location);
		} else {
			comparison.right->SetAlias(text);
		}
	}

	//! The own `when` of each reducer of the clause's left side that has one: a group holds an instance only where each
	//! keeps a row (AddDisagreementCheck). A reducer with none reads every kept row of its group.
	static vector<const Expression *> OwnWhens(const Expression &left) {
		vector<const Expression *> whens;
		std::function<void(const Expression &)> walk = [&](const Expression &expr) {
			if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
				auto &reducer = expr.Cast<BoundAggregateExpression>();
				if (reducer.filter) {
					whens.push_back(reducer.filter.get());
				}
				return;
			}
			ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { walk(child); });
		};
		walk(left);
		return whens;
	}

	//! A row whose written `per` column is NULL is in no class (D5), so it has no instance and its bound is read by
	//! none: the bound is NULL there, which the tightest value and the checks skip, as they skip a row a `when` drops.
	//! A relation's columns compare NULL as a value and keep their rows.
	void InClassOnly(unique_ptr<Expression> &bound) const {
		if (class_key == DConstants::INVALID_INDEX) {
			return;
		}
		D_ASSERT(class_key < decide.keys.size());
		for (auto &element : decide.bound.keys[class_key].elements) {
			if (element.null_mode == DecideKeyNullMode::NO_GROUP) {
				auto value = calc.Of(*bound);
				auto none = calc.Constant(Value(value->return_type));
				bound = calc.Out(calc.If(calc.IsNull(calc.Of(element.Grouping())), none, value));
			}
		}
	}

	//! `path` holds the reducers around `expr`: their own `when`s and `by` keys (the clause's `when` is `when`).
	void ReplaceReducers(unique_ptr<Expression> &expr, const DecideReducerPath &path = DecideReducerPath()) {
		idx_t frame;
		if (TryGetDecideFrame(*expr, frame)) {
			return; // a row-wise value execution computes, whose payload holds no reducer
		}
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			expr = MakeReducerColumn(expr->Cast<BoundAggregateExpression>(), path);
			return;
		}
		ExpressionIterator::EnumerateChildren(*expr,
		                                      [&](unique_ptr<Expression> &child) { ReplaceReducers(child, path); });
	}

	//! The reducer's value on each row: the aggregate over the rows of the row's group that it reads (the clause's
	//! kept rows of its `by` group that pass its own `when`), counting each class of its `per` key once. Recorded, not
	//! computed: the value is the record's placeholder column. Under other reducers (`path`), its group is one of its
	//! path key and the rows it reads pass every enclosing `when` too; the reducers in its argument are recorded
	//! first, and its argument reads their columns.
	unique_ptr<Expression> MakeReducerColumn(const BoundAggregateExpression &reducer, const DecideReducerPath &path) {
		if (!reducer.children.empty() && !FrameText(*reducer.children[0]).empty()) {
			return MakeFrameAggregate(reducer, path);
		}
		vector<unique_ptr<Expression>> arguments;
		for (auto &child : reducer.children) {
			arguments.push_back(child->Copy());
			ReplaceReducers(arguments.back(), path.Below(reducer));
		}
		auto reads = Reads(path, reducer.filter.get());
		AddValueCheck(ArgumentCheck(reducer, arguments.empty() ? nullptr : arguments[0].get(), reads, path));
		auto function = reducer.Copy();
		auto &aggregate = function->Cast<BoundAggregateExpression>();
		aggregate.children.clear();
		aggregate.filter = nullptr;
		DecideBoundAggregate record;
		record.function = std::move(function);
		record.arguments = std::move(arguments);
		record.key = PathKeyOf(reducer, path);
		idx_t scope_idx;
		if (DecideReducerPer(reducer, scope_idx)) {
			D_ASSERT(scope_idx < decide.keyed_scopes.size());
			// Only the optimizer's helper decisions have scopes over a link, and they are made after this pass.
			D_ASSERT(decide.keyed_scopes[scope_idx].value_link == DConstants::INVALID_INDEX);
			record.counts_classes = true;
			record.class_key = decide.bound.keyed_scopes[scope_idx].key;
		}
		vector<const Expression *> keys;
		for (auto &element : DecideKeyElements(decide.bound.keys, record.key)) {
			keys.push_back(&element.Grouping());
		}
		const auto key = record.key;
		auto column = RecordAggregate(std::move(record), reads);
		reducer_columns.push_back({column, std::move(keys)});
		return NamedReducerValue(reducer, key, calc.Col(reducer.return_type, column));
	}

	//! A reducer over data whose argument reads a frame: no side table can compute it, since a frame's values are
	//! columns execution computes. It stays a reducer over the rows it reads (`Reads`, its new filter), tagged with its
	//! entry of LogicalDecide::frame_aggregates -- its path key and its `per` key --, which stage 05 makes a frame
	//! column execution computes after the frames (DecideFrameColumn::Kind::AGGREGATE). The reducers inside its
	//! argument are columns first, as for any reducer.
	unique_ptr<Expression> MakeFrameAggregate(const BoundAggregateExpression &reducer, const DecideReducerPath &path) {
		// A NaN its argument reads makes the reducer's value NaN, which the value's reader refuses; a norm's value can
		// hide it (an L0 count's SIGN reads it as 0), so its body is checked as a reducer over data's is, at the rows
		// the reducer reads.
		if (DecideWrittenNormBody(reducer)) {
			AddValueCheck(ArgumentCheck(reducer, reducer.children[0].get(), Reads(path, reducer.filter.get()), path));
		}
		auto marker = reducer.Copy();
		auto &aggregate = marker->Cast<BoundAggregateExpression>();
		for (auto &child : aggregate.children) {
			ReplaceReducers(child, path.Below(reducer));
		}
		auto reads = Reads(path, reducer.filter.get());
		aggregate.filter = reads ? calc.Out(reads) : nullptr;
		LogicalDecide::FrameAggregate record;
		record.key = PathKeyOf(reducer, path);
		idx_t scope_idx;
		const bool has_per = DecideReducerPer(reducer, scope_idx);
		if (has_per) {
			record.class_key = decide.bound.keyed_scopes[scope_idx].key;
		}
		record.text = Written(reducer);
		aggregate.SetAlias(MakeFrameAggregateTag(decide.frame_aggregates.size()));
		if (has_per) {
			SetDecideTreeReducerPer(aggregate, scope_idx);
		}
		decide.frame_aggregates.push_back(std::move(record));
		return marker;
	}

	//! Adds `record` to the node's bound aggregates, reading the rows `reads` (null for every row) of the clause being
	//! rewritten, and returns its placeholder column; an equal record is shared. The aggregate has one value per group
	//! of its key, so its table has the key's columns, then the value.
	ColumnBinding RecordAggregate(DecideBoundAggregate record, const ExprPtr &reads) {
		if (reads) {
			record.reads = calc.Out(reads);
		}
		for (auto &known : decide.bound_aggregates) {
			if (known.Equals(record)) {
				return known.column;
			}
		}
		record.column = ColumnBinding(optimizer.binder.GenerateTableIndex(),
		                              DecideKeyElements(decide.bound.keys, record.key).size());
		decide.bound_aggregates.push_back(std::move(record));
		return decide.bound_aggregates.back().column;
	}

	//! The check of what a reducer reads, which raises for a NaN in it: its argument (or a norm's written body:
	//! `SIGN(NaN)` is 0) on the rows it reads. A reducer reads its argument as the same SQL aggregate does: it skips a
	//! NULL. A NaN is an error, except to COUNT, which counts it, and so is an infinity where the value is read as a
	//! coefficient
	//! (`read_as`). The check is true on any other row, and null when there is none to make. `argument` is the argument
	//! with the reducers inside it replaced by their columns, `reads` the rows it reads and `path` the reducers around
	//! it.
	unique_ptr<Expression> ArgumentCheck(const BoundAggregateExpression &reducer, const Expression *argument,
	                                     const ExprPtr &reads, const DecideReducerPath &path) {
		auto name = StringUtil::Lower(reducer.function.name);
		D_ASSERT(name == "count" || name == "count_star" || name == "min" || name == "max" || name == "arg_min" ||
		         name == "arg_max" || name == "sum" || name == "avg");
		// The binder rejects DISTINCT and drops an ORDER BY these functions ignore, canonicalization gave every norm of
		// data its definition, and the AVG rewrite of `<>` runs after this pass.
		D_ASSERT(!reducer.IsDistinct() && !reducer.order_bys && !DecideReducerCountScale(reducer) &&
		         !IsDecideNormMarker(reducer));
		if (!argument || name == "count") {
			return nullptr;
		}
		// A norm's definition can hide a NaN (an L0 count's SIGN reads it as 0), so the value checked is the body.
		auto body = DecideWrittenNormBody(reducer);
		auto checked = body ? body : argument;
		auto &named = body ? *body : *reducer.children[0];
		auto nan = NanCheck(*checked, Written(named), SpanOf(named.GetQueryLocation()), read_as);
		if (!nan) {
			return nullptr;
		}
		return calc.Out(DecideCheckOnKeptRows(calc, reads, InKey(PathKeyOf(reducer, path), nan, calc.Bool(true))));
	}

	//! The reducer's value on a row, given the column that holds its aggregate: NULL on a row whose written `by` column
	//! is NULL, which is in no group, so the NULL bound skips the row's instance (deck p. 50). The value carries the
	//! reducer's written text as its name, so every place that quotes a bound still reads `sum(cap) by (g)`; the tag
	//! keeps a NULL message from naming it as a column.
	unique_ptr<Expression> NamedReducerValue(const BoundAggregateExpression &reducer, idx_t key, ExprPtr column) const {
		auto result = calc.Out(InKey(key, column));
		NameAsWritten(*result, reducer);
		MarkReducerValue(*result);
		if (in_objective && key == DConstants::INVALID_INDEX) {
			// The objective is produced once: a reducer over data with no `by` is one value for the whole query, as a
			// subquery is, so it may scale a reducer there.
			MarkDecideQueryWideValue(*result);
		}
		return result;
	}

	//! `value` stands for a recorded reducer and is read whole.
	static void MarkReducerValue(Expression &value) {
		MarkDecideValue(value, DecideValueKind::BOUND_REDUCER);
	}

	//! `value` stands in for `written`, equal to it for the rows it is read on, so it is quoted as `written` is: it
	//! takes its location (the location rule). An expression with no location, one a macro expanded, keeps the name
	//! it prints as.
	void NameAsWritten(Expression &value, const Expression &written) const {
		if (written.GetQueryLocation().IsValid()) {
			DecideReplaces(value, written);
		} else {
			value.SetAlias(Written(written));
		}
	}

	//! `value` on a row in a group of the key `key_index`, `outside` (NULL when null) on a row in none; `value` for the
	//! whole query.
	ExprPtr InKey(idx_t key_index, ExprPtr value, const ExprPtr &outside = nullptr) const {
		for (auto &element : DecideKeyElements(decide.bound.keys, key_index)) {
			if (element.null_mode == DecideKeyNullMode::NO_GROUP) {
				value = calc.If(calc.IsNull(calc.Of(*element.expression)),
				                outside ? outside : calc.Constant(Value(value->return_type)), value);
			}
		}
		return value;
	}

	//! The aggregate `function` of `argument` over the kept rows of each group of `key`, read as a column of its side
	//! table (the one record is shared with an equal one).
	ExprPtr RecordOver(const char *function, const Expression &argument, idx_t key) {
		vector<unique_ptr<Expression>> children;
		children.push_back(argument.Copy());
		DecideBoundAggregate record;
		record.function = optimizer.BindAggregateFunction(function, std::move(children));
		record.function->Cast<BoundAggregateExpression>().children.clear();
		record.arguments.push_back(argument.Copy());
		record.key = key;
		auto type = record.function->return_type;
		return calc.Col(type, RecordAggregate(std::move(record), Kept()));
	}

	//! The check of an `=` bound that takes two values (`low` and `high`) in its group: the row raises the one text of
	//! DecideBoundDisagreementMessage, read from `names` (the key's columns as the query writes them) and `values`
	//! (their value on the row), pointing at `place` (the bound's span in the query, invalid for none, which the
	//! raising call carries); the check is true where the values agree or where one of `held` is not true: a group
	//! whose reducers do not each read a row holds no instance, and constrains nothing.
	ExprPtr BoundDisagreement(const string &text, optional_idx place, const ExprPtr &low, const ExprPtr &high,
	                          const vector<ExprPtr> &held, const vector<string> &names,
	                          const vector<ExprPtr> &values) const {
		D_ASSERT(names.size() == values.size());
		vector<ExprPtr> arguments {calc.Constant(Value(text)), calc.Cast(low, LogicalType::DOUBLE),
		                           calc.Cast(high, LogicalType::DOUBLE)};
		for (idx_t i = 0; i < names.size(); i++) {
			arguments.push_back(calc.Constant(Value(names[i])));
			arguments.push_back(values[i]);
		}
		auto message = calc.Fn("__internal_decide_bound_disagreement", arguments);
		// The call carries its own place, as a cast does.
		message->SetQueryLocation(place);
		auto disagree = calc.Cmp(ExpressionType::COMPARE_NOTEQUAL, low, high);
		for (auto &read : held) {
			disagree =
			    calc.If(calc.Cmp(ExpressionType::COMPARE_EQUAL, read, calc.Bool(true)), disagree, calc.Bool(false));
		}
		return calc.If(disagree, message, calc.Bool(true));
	}

	//! The check of an `=` bound over a group (`bound`, a value that may differ between the kept rows of a group,
	//! written as `text`, at `place` in the query; `reads`, the own `when` of each reducer of the clause that has one):
	//! the group takes one value. A `per K:` makes one instance per class of each group and the groups merge them, so
	//! the bound holds one value in each group, which holds each class's. The check stands in `value_checks`, made
	//! over the source and the aggregates' side tables before any rule runs, so every path states it. A group where
	//! some reducer reads no row holds no instance (language/guide.md §7.4). The bound's NaN check stands before it
	//! (RewriteBound), so it reads valid values.
	void AddDisagreementCheck(idx_t by, const Expression &bound, const string &text, optional_idx place,
	                          const vector<const Expression *> &reads) {
		vector<ExprPtr> held;
		for (auto *read : reads) {
			held.push_back(RecordOver("bool_or", *read, by));
		}
		// The key's columns as the query writes them, each once, with their value on the row.
		vector<string> names;
		vector<ExprPtr> values;
		for (auto &element : DecideKeyElements(decide.bound.keys, by)) {
			if (std::find(names.begin(), names.end(), element.name) == names.end()) {
				names.push_back(element.name);
				values.push_back(calc.Of(*element.expression));
			}
		}
		AddValueCheck(
		    calc.Out(DecideCheckOnKeptRows(calc, Kept(),
		                                   BoundDisagreement(text, place, RecordOver("min", bound, by),
		                                                     RecordOver("max", bound, by), held, names, values))));
	}

	//! Whether `bound` has one value in each group of `keys` among the kept rows: it reads data only through reducers
	//! whose `by` key is part of `keys`.
	bool OneValuePerGroup(const Expression &bound, const vector<const Expression *> &keys) const {
		idx_t frame;
		if (TryGetDecideFrame(bound, frame)) {
			// `first` or `last` reads one row of each timeline, so one value in a group that lies in one timeline:
			// every element of the timeline key is part of the group key.
			auto &spec = decide.bound.frames[frame];
			if (spec.Relative()) {
				return false;
			}
			for (auto &element : DecideKeyElements(decide.bound.keys, spec.timeline_key)) {
				if (std::none_of(keys.begin(), keys.end(),
				                 [&](const Expression *key) { return key->Equals(element.Grouping()); })) {
					return false;
				}
			}
			return true;
		}
		// A column of the group key is one value in each group.
		if (std::any_of(keys.begin(), keys.end(), [&](const Expression *key) { return key->Equals(bound); })) {
			return true;
		}
		if (bound.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &binding = bound.Cast<BoundColumnRefExpression>().binding;
			for (auto &reducer : reducer_columns) {
				if (reducer.binding == binding) {
					return std::all_of(reducer.keys.begin(), reducer.keys.end(), [&](const Expression *key) {
						return std::any_of(keys.begin(), keys.end(),
						                   [&](const Expression *group) { return group->Equals(*key); });
					});
				}
			}
			return false;
		}
		bool one = true;
		ExpressionIterator::EnumerateChildren(
		    bound, [&](const Expression &child) { one = one && OneValuePerGroup(child, keys); });
		return one;
	}

	//! The check of a `value` among `rows` (every row when null): a row with a NaN raises the one error of
	//! DecideInvalidValueMessage, naming the bound as `written` and pointing at `place`, and any other row holds; null
	//! when `value` cannot be NaN. A NULL bound skips its instance (deck p. 50) and infinity is a value.
	unique_ptr<Expression> InvalidValueCheck(const Expression &value, const string &written, optional_idx place,
	                                         const ExprPtr &rows) const {
		auto nan = NanCheck(value, written, place);
		if (!nan) {
			return nullptr;
		}
		return calc.Out(DecideCheckOnKeptRows(calc, rows, nan));
	}

	//! Where a message about the expression at `location` points: the span the query text holds for it, none for a
	//! location in a table macro's tail.
	optional_idx SpanOf(optional_idx location) const {
		return DecideSpanLocation(decide.bound.source_text, location);
	}

	//! The check of a `value` its `role` refuses -- a NaN bound, a coefficient that is not finite --: the row raises,
	//! and any other holds; null when `value` cannot be either. `written` is the value as the query writes it, at
	//! `place`.
	ExprPtr NanCheck(const Expression &value, const string &written, optional_idx place,
	                 DecideValueRole role = DecideValueRole::BOUND) const {
		if (value.return_type != LogicalType::FLOAT && value.return_type != LogicalType::DOUBLE) {
			return nullptr;
		}
		// The tree evaluates the check, so the value keeps its tags (a frame's, a reducer's, a query-wide value's):
		// `Of` strips them, which would read a frame marker as a reducer over the rows.
		auto number = calc.Cast(calc.Wrap(value.Copy()), LogicalType::DOUBLE);
		// A NULL is no value: neither test holds of it.
		auto refused = role == DecideValueRole::BOUND
		                   ? calc.Fn("isnan", number)
		                   : calc.Cmp(ExpressionType::COMPARE_EQUAL, calc.Fn("isfinite", number), calc.Bool(false));
		return calc.If(std::move(refused), calc.RaiseInvalidValue(role, written, place), calc.Bool(true));
	}

	void AddValueCheck(unique_ptr<Expression> message) {
		decide_rewrite::AddValueCheck(decide, std::move(message));
	}

	//! The key a reducer under the reducers of `path` groups by: its `by` key under every enclosing one, an index
	//! into `keys` (FindOrCreateDecidePathKey); INVALID_INDEX for the whole query.
	idx_t PathKeyOf(const BoundAggregateExpression &reducer, const DecideReducerPath &path) const {
		const auto own = GroupKeyIndex(reducer);
		return path.keys.empty() ? own : FindOrCreateDecidePathKey(path.KeysOf(own), decide.bound.keys);
	}

	//! The key a reducer groups by (an index into `keys`); INVALID_INDEX for the whole query.
	idx_t GroupKeyIndex(const BoundAggregateExpression &reducer) const {
		const idx_t index = DecideReducerGroupKey(reducer);
		D_ASSERT(index == DConstants::INVALID_INDEX || index < decide.bound.keys.size());
		return index;
	}

	//! The grouping columns of the key `key`.
	vector<const Expression *> KeyColumns(idx_t key) const {
		vector<const Expression *> keys;
		for (auto &element : DecideKeyElements(decide.bound.keys, key)) {
			keys.push_back(&element.Grouping());
		}
		return keys;
	}

	//! Whether the clause's `when` keeps the row (NULL keeps none); null when it has none.
	ExprPtr Kept() const {
		return when ? calc.Wrap(DecideActiveWhen(calc, when)) : nullptr;
	}

	//! The rows a reducer reads: the clause's kept rows that pass its own `filter` (`when`); null for every row.
	ExprPtr Reads(const Expression *filter) const {
		if (!filter) {
			return Kept();
		}
		auto own = calc.Wrap(DecideActiveWhen(calc, filter));
		return when ? calc.If(Kept(), own, calc.Bool(false)) : own;
	}

	//! The rows a reducer under the reducers of `path` reads: those its own `filter` keeps among those every enclosing
	//! `when` keeps (path.conditions) among the clause's kept rows; null for every row.
	ExprPtr Reads(const DecideReducerPath &path, const Expression *filter) const {
		auto rows = Reads(filter);
		for (auto *condition : path.conditions) {
			auto enclosing = calc.Wrap(DecideActiveWhen(calc, condition));
			rows = rows ? calc.If(rows, enclosing, calc.Bool(false)) : enclosing;
		}
		return rows;
	}

	string Written(const Expression &expr) const {
		return RenderDecideSource(expr, decide.bound.source_text, decide.bound.keyed_scopes, &decide.bound.keys);
	}

	//! The first frame selector `expr` holds, as written; empty for none.
	string FrameText(const Expression &expr) const {
		idx_t frame;
		if (TryGetDecideFrame(expr, frame)) {
			return frame < decide.bound.frames.size() ? decide.bound.frames[frame].text : Written(expr);
		}
		string text;
		ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
			if (text.empty()) {
				text = FrameText(child);
			}
		});
		return text;
	}

	Optimizer &optimizer;
	LogicalDecide &decide;
	ExpressionBuilder calc;
	//! The clause being rewritten: its `when` condition, null for none, and the key of its `per K:`, INVALID_INDEX for
	//! none or `per ()`.
	const Expression *when = nullptr;
	idx_t class_key = DConstants::INVALID_INDEX;
	//! What the reducers being rewritten are read as: a bound's value, or a coefficient's inside a reducer over
	//! decisions.
	DecideValueRole read_as = DecideValueRole::BOUND;
	//! The objective is being rewritten, which is produced once.
	bool in_objective = false;
	vector<ReducerColumn> reducer_columns;
};

} // namespace

void DecideOptimizer::RewriteRhsAggregates(LogicalDecide &decide) {
	D_ASSERT(decide.children.size() == 1);
	RhsBoundRewriter(optimizer, decide).Run();
}

} // namespace duckdb
