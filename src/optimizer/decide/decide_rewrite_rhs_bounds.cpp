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
//     surrounding arithmetic (`COUNT(*) * k`) is evaluated in DuckDB's own types;
//   - the tightest bound of a clause in the aggregate shape: such a clause is one constraint per group, and the
//     constraint on each kept row of the group holds when the tightest one does (paper §3.2.1), so `<=` takes the
//     group's MIN and `>=` its MAX; `=` must have one value per group.
// The solver (PhysicalDecide) and the direct-solve rules then read plain columns, and neither reduces a bound.
//
// A bound the group's own aggregate can compute is not evaluated here. It is recorded as a DecideGroupBound: the MIN
// and/or MAX of a raw value, or an expression of reducers that read exactly the clause's group (`sum(x) by (g) <=
// sum(cap) by (g)`). A direct-solve rule computes its aggregates in the group aggregate it already runs; the solver
// path records their aggregates (RecordGroupBounds) as it does for every other bound.
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
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
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
//! computed. A group bound that a direct-solve rule can compute itself is recorded as a DecideGroupBound instead.
class RhsBoundRewriter {
public:
	RhsBoundRewriter(Optimizer &optimizer_p, LogicalDecide &decide_p)
	    : optimizer(optimizer_p), decide(decide_p), calc(optimizer_p.context),
	      canonicalizer(optimizer_p.context, decide_p.decide_index, decide_p.variable_scopes, decide_p.keyed_scopes,
	                    decide_p.is_boolean_var) {
	}

	void Run() {
		// The parser requires a SUCH THAT clause with at least one constraint.
		D_ASSERT(decide.decide_constraints);
		Walk(*decide.decide_constraints, DecideEnclosingScope());
	}

	//! Records the aggregates of the group bounds `Run` left to a rule, and puts their checks where they would have
	//! stood. The solver path needs them; a direct rule computes the bound itself.
	void RecordGroupBounds() {
		idx_t inserted = 0;
		for (auto &bound : decide.reduced_bounds) {
			when = bound.when;
			auto messages = RecordGroupBound(bound.comparison->Cast<BoundComparisonExpression>(), *bound.group);
			auto position = decide.scope_checks.begin() + NumericCast<int64_t>(bound.check_position + inserted);
			for (auto &message : messages) {
				DecideScopeCheck check;
				check.kind = DecideScopeCheckKind::VALUE_IS_VALID;
				check.value = std::move(message);
				position = decide.scope_checks.insert(position, std::move(check)) + 1;
				inserted++;
			}
		}
		decide.reduced_bounds.clear();
	}

private:
	//! A reducer's column and the `by` key it has one value per group of.
	struct ReducerColumn {
		ColumnBinding binding;
		vector<const Expression *> keys;
	};

	//! A `per K:` wrapper groups no reducer: its clause is read like any other. Its key says which rows are in a class.
	void Walk(Expression &expr, DecideEnclosingScope scope) {
		scope = EnterDecideScope(expr, scope);
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conjunction = expr.Cast<BoundConjunctionExpression>();
			for (idx_t i = 0; i < conjunction.children.size(); i++) {
				if (IsConstraintChild(conjunction, i)) {
					Walk(*conjunction.children[i], scope);
				}
			}
			return;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			when = scope.Condition();
			class_key = scope.PerKey();
			RewriteBound(expr.Cast<BoundComparisonExpression>());
		}
	}

	//! A clause in the aggregate shape is one constraint per group. Its data bound is checked on every kept row, before
	//! any value is read. A bound that reads data only through the reducers of its group is recorded as the aggregates
	//! it is a function of, and one that may differ between the kept rows of a group as its tightest value. `<>` keeps
	//! every value: each one is excluded. A query-wide value has one value already. A row-wise bound is checked by the
	//! solver as it reads it. The solver path records the aggregates of a recorded bound later (RecordGroupBounds).
	void RewriteBound(BoundComparisonExpression &comparison) {
		// Canonicalization put every decision on the left, so the right is data.
		bool aggregate_data_bound =
		    !comparison.right->IsFoldable() &&
		    canonicalizer.ClassifyCanonicalComparison(comparison) == CanonicalConstraintClass::AGGREGATE;
		bool query_wide = IsQueryWideBoundTag(comparison.right->GetAlias());
		auto text = Written(*comparison.right);
		if (aggregate_data_bound && IsGroupBound(comparison)) {
			RecordReducers(comparison, text);
			return;
		}
		ReplaceReducers(comparison.right);
		if (!aggregate_data_bound) {
			return;
		}
		bool reduces = comparison.type != ExpressionType::COMPARE_NOTEQUAL && !query_wide &&
		               !OneValuePerGroup(*comparison.right, GroupKeyOf(comparison));
		if (reduces) {
			InClassOnly(comparison.right);
		}
		auto invalid = InvalidValueMessage(*comparison.right, text, Kept());
		if (reduces) {
			Record(comparison,
			       DirectTightestBound(optimizer, comparison.type, comparison.right->Copy(), text, std::move(invalid)));
			return;
		}
		AddValueCheck(std::move(invalid));
	}

	//! Whether the bound is a function of its group's own reducers: each reads exactly the rows the clause's group
	//! holds, and the bound reads no data but through them, so it has one value per group, which the group's aggregate
	//! computes. A reducer with another `by` key reads other rows, and one that counts each class of a `per` key once
	//! is no plain aggregate; `<>` excludes each value its bound takes. The side tables compute all of those.
	bool IsGroupBound(const BoundComparisonExpression &comparison) const {
		if (comparison.type == ExpressionType::COMPARE_NOTEQUAL) {
			return false;
		}
		idx_t reducers = 0;
		return ReadsOnlyGroupReducers(*comparison.right, GroupKeyOf(comparison), reducers) && reducers > 0;
	}

	bool ReadsOnlyGroupReducers(const Expression &expr, const vector<const Expression *> &keys, idx_t &reducers) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			idx_t scope_idx;
			auto own = GroupKeyOf(expr);
			reducers++;
			return !TryParseQualifiedReducerTag(expr.GetAlias(), scope_idx) && own.size() == keys.size() &&
			       std::all_of(own.begin(), own.end(), [&](const Expression *key) {
				       return std::any_of(keys.begin(), keys.end(),
				                          [&](const Expression *group) { return group->Equals(*key); });
			       });
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		bool only = true;
		ExpressionIterator::EnumerateChildren(
		    expr, [&](const Expression &child) { only = only && ReadsOnlyGroupReducers(child, keys, reducers); });
		return only;
	}

	//! Records the bound as the aggregates of its reducers and an expression over their placeholders.
	void RecordReducers(BoundComparisonExpression &comparison, const string &text) {
		auto group = make_uniq<DecideGroupBound>();
		group->text = text;
		ReplaceWithPlaceholders(comparison.right, optimizer.binder.GenerateTableIndex(), *group);
		if (DirectIsUpperBound(comparison.type)) {
			group->upper = comparison.right->Copy();
		}
		if (DirectIsLowerBound(comparison.type)) {
			group->lower = comparison.right->Copy();
		}
		// The group's one value is not yet limited to the rows the clause keeps: the solver path does that, a rule's
		// group is those rows.
		auto nan = NanMessage(*comparison.right, text);
		group->invalid = nan ? calc.Out(nan) : nullptr;
		Record(comparison, std::move(group));
	}

	void ReplaceWithPlaceholders(unique_ptr<Expression> &expr, idx_t table_index, DecideGroupBound &group) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			ExpressionIterator::EnumerateChildren(
			    *expr, [&](unique_ptr<Expression> &child) { ReplaceWithPlaceholders(child, table_index, group); });
			return;
		}
		auto &reducer = expr->Cast<BoundAggregateExpression>();
		auto name = ReducerName(reducer);
		DecideGroupBound::Input input;
		input.invalid = ArgumentMessage(reducer);
		if (!reducer.children.empty()) {
			input.argument = std::move(reducer.children[0]);
			reducer.children.clear();
		}
		input.filter = std::move(reducer.filter);
		auto placeholder = calc.Out(calc.Col(reducer.return_type, ColumnBinding(table_index, group.aggregates.size())));
		placeholder->SetAlias(std::move(name));
		group.aggregates.push_back(
		    {ColumnBinding(table_index, group.aggregates.size()), std::move(expr), group.inputs.size()});
		group.inputs.push_back(std::move(input));
		expr = std::move(placeholder);
	}

	//! A row whose written `per` column is NULL is in no class (D5), so it has no instance and its bound is read by
	//! none: the bound is NULL there, which the tightest value and the checks skip, as they skip a row a `when` drops.
	//! A relation's columns compare NULL as a value and keep their rows.
	void InClassOnly(unique_ptr<Expression> &bound) const {
		if (class_key == DConstants::INVALID_INDEX) {
			return;
		}
		D_ASSERT(class_key < decide.keys.size());
		for (auto &element : decide.keys[class_key].elements) {
			if (element.null_mode == DecideKeyNullMode::NO_GROUP) {
				auto value = calc.Of(*bound);
				auto none = calc.Constant(Value(value->return_type));
				bound = calc.Out(calc.If(calc.IsNull(calc.Of(element.Grouping())), none, value));
			}
		}
	}

	//! Leaves the bound to the rule. Its checks stand where they would have, in clause order, once the aggregates are
	//! recorded.
	void Record(BoundComparisonExpression &comparison, unique_ptr<DecideGroupBound> group) {
		DecideReducedBound bound;
		bound.comparison = &comparison;
		bound.when = when;
		bound.group = std::move(group);
		bound.check_position = decide.scope_checks.size();
		decide.reduced_bounds.push_back(std::move(bound));
	}

	void ReplaceReducers(unique_ptr<Expression> &expr) {
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			expr = MakeReducerColumn(expr->Cast<BoundAggregateExpression>());
			return;
		}
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { ReplaceReducers(child); });
	}

	//! The reducer's value on each row: the aggregate over the rows of the row's group that it reads (the clause's
	//! kept rows of its `by` group that pass its own `when`), counting each class of its `per` key once. Recorded, not
	//! computed: the value is the record's placeholder column.
	unique_ptr<Expression> MakeReducerColumn(const BoundAggregateExpression &reducer) {
		AddValueCheck(ArgumentMessage(reducer));
		auto function = reducer.Copy();
		auto &aggregate = function->Cast<BoundAggregateExpression>();
		aggregate.children.clear();
		aggregate.filter = nullptr;
		DecideBoundAggregate record;
		record.function = std::move(function);
		if (!reducer.children.empty()) {
			record.argument = reducer.children[0]->Copy();
		}
		record.key = GroupKeyIndex(reducer);
		idx_t scope_idx;
		if (TryParseQualifiedReducerTag(reducer.GetAlias(), scope_idx)) {
			D_ASSERT(scope_idx < decide.keyed_scopes.size());
			// Only the optimizer's helper decisions have scopes over a link, and they are made after this pass.
			D_ASSERT(decide.keyed_scopes[scope_idx].value_link == DConstants::INVALID_INDEX);
			record.counts_classes = true;
			record.class_key = decide.keyed_scopes[scope_idx].key;
		}
		auto keys = GroupKeyOf(reducer);
		auto column = RecordAggregate(std::move(record), Reads(reducer.filter.get()));
		reducer_columns.push_back({column, std::move(keys)});
		return NamedReducerValue(reducer, calc.Col(reducer.return_type, column));
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
		record.column =
		    ColumnBinding(optimizer.binder.GenerateTableIndex(), DecideKeyElements(decide.keys, record.key).size());
		decide.bound_aggregates.push_back(std::move(record));
		return decide.bound_aggregates.back().column;
	}

	//! The message a row raises for a NaN in what a reducer reads: its argument (or a norm's written body: `SIGN(NaN)`
	//! is 0) on the rows it reads. A reducer reads its argument as the same SQL aggregate does: it skips a NULL. A NaN
	//! is an error, except to COUNT, which counts it. Null when there is none to raise.
	unique_ptr<Expression> ArgumentMessage(const BoundAggregateExpression &reducer) {
		auto name = StringUtil::Lower(reducer.function.name);
		D_ASSERT(name == "count" || name == "count_star" || name == "min" || name == "max" || name == "sum" ||
		         name == "avg");
		// The binder rejects DISTINCT and drops an ORDER BY these functions ignore, canonicalization gave every norm of
		// data its definition, and the AVG rewrite of `<>` runs after this pass.
		string payload;
		D_ASSERT(!reducer.IsDistinct() && !reducer.order_bys && !HasDecideTag(reducer.GetAlias(), AVG_REWRITE_TAG) &&
		         !ExtractDecideTagPayload(reducer.GetAlias(), NORM_MARKER_TAG_PREFIX, payload));
		if (reducer.children.empty() || name == "count") {
			return nullptr;
		}
		// A norm's definition can hide a NaN (an L0 count's SIGN reads it as 0), so the value checked is the body.
		auto body = DecideWrittenNormBody(reducer);
		auto argument = body ? body : reducer.children[0].get();
		auto nan = NanMessage(*argument, Written(*argument));
		return nan ? calc.Out(DirectOnKeptRows(calc, Reads(reducer.filter.get()), InGroup(reducer, nan))) : nullptr;
	}

	//! The reducer's value on a row, given the column that holds its aggregate: NULL on a row whose written `by` column
	//! is NULL, which is in no group, so the NULL bound skips the row's instance (deck p. 50). The value carries the
	//! reducer's written text as its name, so every place that quotes a bound still reads `sum(cap) by (g)`; the tag
	//! keeps a NULL message from naming it as a column.
	unique_ptr<Expression> NamedReducerValue(const BoundAggregateExpression &reducer, ExprPtr column) const {
		auto result = calc.Out(InGroup(reducer, column));
		result->SetAlias(ReducerName(reducer));
		return result;
	}

	string ReducerName(const Expression &reducer) const {
		auto alias = Written(reducer);
		AddDecideTag(alias, BOUND_REDUCER_TAG);
		return alias;
	}

	//! `value` on a row in a group of the reducer's `by` key, NULL on a row whose written key column is NULL, which is
	//! in no group. A relation's columns compare NULL as a value.
	ExprPtr InGroup(const Expression &tagged, ExprPtr value) const {
		idx_t key_index;
		if (!TryParseGroupKeyTag(tagged.GetAlias(), key_index)) {
			return value;
		}
		for (auto &element : decide.keys[key_index].elements) {
			if (element.null_mode == DecideKeyNullMode::NO_GROUP) {
				value =
				    calc.If(calc.IsNull(calc.Of(*element.expression)), calc.Constant(Value(value->return_type)), value);
			}
		}
		return value;
	}

	//! Records a left-over bound's aggregates over the group of its clause, so the comparison reads the bound as one
	//! column (named by the bound as written), and returns the checks of the bound in the solver's order: what each
	//! aggregate reads, a NaN bound, then an `=` bound that takes two values in a group.
	vector<unique_ptr<Expression>> RecordGroupBound(BoundComparisonExpression &comparison, DecideGroupBound &group) {
		vector<ColumnBinding> placeholders;
		vector<ExprPtr> columns;
		vector<unique_ptr<Expression>> values;
		for (auto &aggregate : group.aggregates) {
			auto &input = group.inputs[aggregate.input];
			DecideBoundAggregate record;
			record.function = aggregate.function->Copy();
			if (input.argument) {
				record.argument = input.argument->Copy();
			}
			record.key = GroupKeyIndex(comparison);
			placeholders.push_back(aggregate.column);
			columns.push_back(calc.Col(aggregate.function->return_type,
			                           RecordAggregate(std::move(record), Reads(input.filter.get()))));
			values.push_back(calc.Out(InGroup(comparison, columns.back())));
		}
		vector<unique_ptr<Expression>> messages;
		for (auto &input : group.inputs) {
			if (input.invalid) {
				messages.push_back(std::move(input.invalid));
			}
		}
		if (group.invalid) {
			DirectReplaceColumns(group.invalid, placeholders, values);
			messages.push_back(calc.Out(DirectOnKeptRows(calc, Kept(), calc.Wrap(std::move(group.invalid)))));
		}
		if (group.low != DConstants::INVALID_INDEX) {
			messages.push_back(calc.Out(DirectOnKeptRows(
			    calc, Kept(), DirectBoundDisagreement(calc, group.text, columns[group.low], columns[group.high]))));
		}
		comparison.right = std::move(DirectIsUpperBound(comparison.type) ? group.upper : group.lower);
		DirectReplaceColumns(comparison.right, placeholders, values);
		comparison.right->SetAlias(group.text);
		return messages;
	}

	//! Whether `bound` has one value in each group of `keys` among the kept rows: it reads data only through reducers
	//! whose `by` key is part of `keys`.
	bool OneValuePerGroup(const Expression &bound, const vector<const Expression *> &keys) const {
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

	//! The message a row raises for a NaN `value` among `rows` (every row when null), and NULL elsewhere: the one text
	//! of DecideInvalidValueMessage, naming the bound as `written`; null when `value` cannot be NaN. A NULL bound skips
	//! its instance (deck p. 50) and infinity is a value.
	unique_ptr<Expression> InvalidValueMessage(const Expression &value, const string &written,
	                                           const ExprPtr &rows) const {
		auto nan = NanMessage(value, written);
		if (!nan) {
			return nullptr;
		}
		return calc.Out(DirectOnKeptRows(calc, rows, nan));
	}

	//! The message a row raises for a NaN `value`, and NULL elsewhere; null when `value` cannot be NaN.
	ExprPtr NanMessage(const Expression &value, const string &written) const {
		if (value.return_type != LogicalType::FLOAT && value.return_type != LogicalType::DOUBLE) {
			return nullptr;
		}
		return calc.If(calc.Fn("isnan", calc.Cast(calc.Of(value), LogicalType::DOUBLE)),
		               calc.Constant(Value(DecideInvalidValueMessage(DecideValueRole::BOUND, written))),
		               calc.Constant(Value(LogicalType::VARCHAR)));
	}

	void AddValueCheck(unique_ptr<Expression> message) {
		if (!message) {
			return;
		}
		for (auto &check : decide.scope_checks) {
			if (check.kind == DecideScopeCheckKind::VALUE_IS_VALID && check.value->Equals(*message)) {
				return;
			}
		}
		DecideScopeCheck check;
		check.kind = DecideScopeCheckKind::VALUE_IS_VALID;
		check.value = std::move(message);
		decide.scope_checks.push_back(std::move(check));
	}

	//! The key a reducer or bound groups by (an index into `keys`); INVALID_INDEX for the whole query.
	idx_t GroupKeyIndex(const Expression &tagged) const {
		idx_t index = DConstants::INVALID_INDEX;
		if (TryParseGroupKeyTag(tagged.GetAlias(), index)) {
			D_ASSERT(index < decide.keys.size());
		}
		return index;
	}

	vector<const Expression *> GroupKeyOf(const Expression &tagged) const {
		vector<const Expression *> keys;
		for (auto &element : DecideKeyElements(decide.keys, GroupKeyIndex(tagged))) {
			keys.push_back(&element.Grouping());
		}
		return keys;
	}

	//! Whether the clause's `when` keeps the row (NULL keeps none); null when it has none.
	ExprPtr Kept() const {
		return when ? calc.Wrap(DirectActiveWhen(calc, when)) : nullptr;
	}

	//! The rows a reducer reads: the clause's kept rows that pass its own `filter` (`when`); null for every row.
	ExprPtr Reads(const Expression *filter) const {
		if (!filter) {
			return Kept();
		}
		auto own = calc.Wrap(DirectActiveWhen(calc, filter));
		return when ? calc.If(Kept(), own, calc.Bool(false)) : own;
	}

	string Written(const Expression &expr) const {
		return RenderDecideSource(expr, decide.source_fragments, decide.keyed_scopes, &decide.keys);
	}

	Optimizer &optimizer;
	LogicalDecide &decide;
	ExpressionBuilder calc;
	DecideCanonicalizer canonicalizer;
	//! The clause being rewritten: its `when` condition, null for none, and the key of its `per K:`, INVALID_INDEX for
	//! none or `per ()`.
	const Expression *when = nullptr;
	idx_t class_key = DConstants::INVALID_INDEX;
	vector<ReducerColumn> reducer_columns;
};

} // namespace

void DecideOptimizer::RewriteRhsAggregates(LogicalDecide &decide) {
	D_ASSERT(decide.children.size() == 1);
	RhsBoundRewriter(optimizer, decide).Run();
}

void RecordReducedBounds(Optimizer &optimizer, LogicalDecide &decide) {
	if (!decide.reduced_bounds.empty()) {
		RhsBoundRewriter(optimizer, decide).RecordGroupBounds();
	}
}

} // namespace duckdb
