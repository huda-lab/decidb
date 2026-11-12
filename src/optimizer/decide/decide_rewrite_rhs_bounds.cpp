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
//     group's MIN and `>=` its MAX; `=` must have one value per group.
// The solver (PhysicalDecide) and the direct-solve rules then read plain columns, and neither reduces a bound.
//
// A bound the group's own aggregate can compute is not evaluated here. It is recorded as a DecideGroupBound: the MIN
// and/or MAX of a raw value, or an expression of reducers that read exactly the clause's group (`sum(x) by (g) <=
// sum(cap) by (g)`). A direct-solve rule computes its aggregates in the group aggregate it already runs; the solver
// path records their aggregates (RecordGroupBounds) as it does for every other bound.
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
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
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
//! computed. A group bound that a direct-solve rule can compute itself is recorded as a DecideGroupBound instead.
class RhsBoundRewriter {
public:
	RhsBoundRewriter(Optimizer &optimizer_p, LogicalDecide &decide_p)
	    : optimizer(optimizer_p), decide(decide_p), calc(optimizer_p.context),
	      canonicalizer(optimizer_p.context, decide_p.decide_index, decide_p.variable_scopes, decide_p.keyed_scopes,
	                    decide_p.keys, decide_p.is_boolean_var) {
	}

	void Run() {
		// The parser requires a SUCH THAT clause with at least one constraint.
		D_ASSERT(decide.decide_constraints);
		Walk(*decide.decide_constraints, DecideEnclosingScope());
		if (decide.decide_objective) {
			WalkObjective(decide.decide_objective);
		}
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
			auto &comparison = expr.Cast<BoundComparisonExpression>();
			read_as = DecideValueRole::BOUND;
			RewriteBound(comparison);
			read_as = DecideValueRole::COEFFICIENT;
			ReplaceDataUnderDecisions(comparison.left, nullptr);
		}
	}

	//! The objective, under its `when c:`: an objective is produced once, so it has no `per` key. Its condition holds
	//! no reducer.
	void WalkObjective(unique_ptr<Expression> &expr) {
		when = EnterDecideScope(*expr, DecideEnclosingScope()).Condition();
		class_key = DConstants::INVALID_INDEX;
		read_as = DecideValueRole::COEFFICIENT;
		ReplaceDataUnderDecisions(expr, nullptr);
	}

	//! A reducer over data alone inside one over decisions is a value of its path's group (language/guide.md §5.5), as
	//! it is inside one on a bound: its column, read as a coefficient, which must be finite. `path` holds the reducers
	//! over decisions around `expr`; null outside every one, where canonicalization left no reducer over data.
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
			if (path) {
				expr = MakeReducerColumn(reducer, *path);
			}
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
		auto frame = FrameText(*comparison.right);
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
		idx_t frame;
		if (TryGetDecideFrame(expr, frame)) {
			return false;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			idx_t scope_idx;
			auto own = GroupKeyOf(expr);
			reducers++;
			// A reducer holding a reducer reads its inner one per group of a path key: a side table. One holding a
			// frame is refused there (MakeReducerColumn). A rule's group aggregate reads one argument, so one that
			// reads more -- a collated MIN or MAX -- takes a side table too.
			auto &reducer = expr.Cast<BoundAggregateExpression>();
			if (reducer.children.size() > 1) {
				return false;
			}
			if (!reducer.children.empty() && (DecideCanonicalizer::ContainsReducer(*reducer.children[0]) ||
			                                  ContainsDecideFrame(*reducer.children[0]))) {
				return false;
			}
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
		// A group bound holds no nested reducer (ReadsOnlyGroupReducers): its argument is data.
		input.invalid = ArgumentMessage(reducer, reducer.children.empty() ? nullptr : reducer.children[0].get(),
		                                Reads(reducer.filter.get()), DecideReducerPath());
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
		AddValueCheck(ArgumentMessage(reducer, arguments.empty() ? nullptr : arguments[0].get(), reads, path));
		auto function = reducer.Copy();
		auto &aggregate = function->Cast<BoundAggregateExpression>();
		aggregate.children.clear();
		aggregate.filter = nullptr;
		DecideBoundAggregate record;
		record.function = std::move(function);
		record.arguments = std::move(arguments);
		record.key = PathKeyOf(reducer, path);
		idx_t scope_idx;
		if (TryParseQualifiedReducerTag(reducer.GetAlias(), scope_idx)) {
			D_ASSERT(scope_idx < decide.keyed_scopes.size());
			// Only the optimizer's helper decisions have scopes over a link, and they are made after this pass.
			D_ASSERT(decide.keyed_scopes[scope_idx].value_link == DConstants::INVALID_INDEX);
			record.counts_classes = true;
			record.class_key = decide.keyed_scopes[scope_idx].key;
		}
		vector<const Expression *> keys;
		for (auto &element : DecideKeyElements(decide.keys, record.key)) {
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
		if (TryParseQualifiedReducerTag(reducer.GetAlias(), scope_idx)) {
			record.class_key = decide.keyed_scopes[scope_idx].key;
		}
		record.text = Written(reducer);
		ExtractDecideTagPayload(reducer.GetAlias(), WRITTEN_NORM_TAG_PREFIX, record.norm_order);
		auto alias = MakeFrameAggregateTag(decide.frame_aggregates.size());
		if (TryParseQualifiedReducerTag(reducer.GetAlias(), scope_idx)) {
			AddDecideTag(alias, string(TREE_REDUCER_SCOPE_TAG_PREFIX) + to_string(scope_idx) + "__");
		}
		aggregate.SetAlias(alias);
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
		record.column =
		    ColumnBinding(optimizer.binder.GenerateTableIndex(), DecideKeyElements(decide.keys, record.key).size());
		decide.bound_aggregates.push_back(std::move(record));
		return decide.bound_aggregates.back().column;
	}

	//! The message a row raises for a NaN in what a reducer reads: its argument (or a norm's written body: `SIGN(NaN)`
	//! is 0) on the rows it reads. A reducer reads its argument as the same SQL aggregate does: it skips a NULL. A NaN
	//! is an error, except to COUNT, which counts it, and so is an infinity where the value is read as a coefficient
	//! (`read_as`). Null when there is none to raise. `argument` is the argument with
	//! the reducers inside it replaced by their columns, `reads` the rows it reads and `path` the reducers around it.
	unique_ptr<Expression> ArgumentMessage(const BoundAggregateExpression &reducer, const Expression *argument,
	                                       const ExprPtr &reads, const DecideReducerPath &path) {
		auto name = StringUtil::Lower(reducer.function.name);
		D_ASSERT(name == "count" || name == "count_star" || name == "min" || name == "max" || name == "arg_min" ||
		         name == "arg_max" || name == "sum" || name == "avg");
		// The binder rejects DISTINCT and drops an ORDER BY these functions ignore, canonicalization gave every norm of
		// data its definition, and the AVG rewrite of `<>` runs after this pass.
		string payload;
		D_ASSERT(!reducer.IsDistinct() && !reducer.order_bys && !HasDecideTag(reducer.GetAlias(), AVG_REWRITE_TAG) &&
		         !ExtractDecideTagPayload(reducer.GetAlias(), NORM_MARKER_TAG_PREFIX, payload));
		if (!argument || name == "count") {
			return nullptr;
		}
		// A norm's definition can hide a NaN (an L0 count's SIGN reads it as 0), so the value checked is the body.
		auto body = DecideWrittenNormBody(reducer);
		auto checked = body ? body : argument;
		auto nan = NanMessage(*checked, Written(body ? *body : *reducer.children[0]), read_as);
		if (!nan) {
			return nullptr;
		}
		return calc.Out(DirectOnKeptRows(calc, reads, InKey(PathKeyOf(reducer, path), nan)));
	}

	//! The reducer's value on a row, given the column that holds its aggregate: NULL on a row whose written `by` column
	//! is NULL, which is in no group, so the NULL bound skips the row's instance (deck p. 50). The value carries the
	//! reducer's written text as its name, so every place that quotes a bound still reads `sum(cap) by (g)`; the tag
	//! keeps a NULL message from naming it as a column.
	unique_ptr<Expression> NamedReducerValue(const BoundAggregateExpression &reducer, idx_t key, ExprPtr column) const {
		auto result = calc.Out(InKey(key, column));
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
		return InKey(GroupKeyIndex(tagged), std::move(value));
	}

	//! `value` on a row in a group of the key `key_index`, NULL on a row in none; `value` for the whole query.
	ExprPtr InKey(idx_t key_index, ExprPtr value) const {
		for (auto &element : DecideKeyElements(decide.keys, key_index)) {
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
				record.arguments.push_back(input.argument->Copy());
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
		idx_t frame;
		if (TryGetDecideFrame(bound, frame)) {
			// `first` or `last` reads one row of each timeline, so one value in a group that lies in one timeline:
			// every element of the timeline key is part of the group key.
			auto &spec = decide.frames[frame];
			if (spec.Relative()) {
				return false;
			}
			for (auto &element : DecideKeyElements(decide.keys, spec.timeline_key)) {
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

	//! The message a row raises for a `value` its `role` refuses -- a NaN bound, a coefficient that is not finite --
	//! and NULL elsewhere; null when `value` cannot be either.
	ExprPtr NanMessage(const Expression &value, const string &written,
	                   DecideValueRole role = DecideValueRole::BOUND) const {
		if (value.return_type != LogicalType::FLOAT && value.return_type != LogicalType::DOUBLE) {
			return nullptr;
		}
		auto number = calc.Cast(calc.Of(value), LogicalType::DOUBLE);
		// A NULL is no value: neither test holds of it.
		auto refused = role == DecideValueRole::BOUND
		                   ? calc.Fn("isnan", number)
		                   : calc.Cmp(ExpressionType::COMPARE_EQUAL, calc.Fn("isfinite", number), calc.Bool(false));
		return calc.If(std::move(refused), calc.Constant(Value(DecideInvalidValueMessage(role, written))),
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

	//! The key a reducer under the reducers of `path` groups by: its `by` key under every enclosing one, an index
	//! into `keys` (FindOrCreateDecidePathKey); INVALID_INDEX for the whole query.
	idx_t PathKeyOf(const BoundAggregateExpression &reducer, const DecideReducerPath &path) const {
		const auto own = GroupKeyIndex(reducer);
		return path.keys.empty() ? own : FindOrCreateDecidePathKey(path.KeysOf(own), decide.keys);
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

	//! The rows a reducer under the reducers of `path` reads: those its own `filter` keeps among those every enclosing
	//! `when` keeps (path.conditions) among the clause's kept rows; null for every row.
	ExprPtr Reads(const DecideReducerPath &path, const Expression *filter) const {
		auto rows = Reads(filter);
		for (auto *condition : path.conditions) {
			auto enclosing = calc.Wrap(DirectActiveWhen(calc, condition));
			rows = rows ? calc.If(rows, enclosing, calc.Bool(false)) : enclosing;
		}
		return rows;
	}

	string Written(const Expression &expr) const {
		return RenderDecideSource(expr, decide.source_fragments, decide.keyed_scopes, &decide.keys);
	}

	//! The first frame selector `expr` holds, as written; empty for none.
	string FrameText(const Expression &expr) const {
		idx_t frame;
		if (TryGetDecideFrame(expr, frame)) {
			return frame < decide.frames.size() ? decide.frames[frame].text : Written(expr);
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
	DecideCanonicalizer canonicalizer;
	//! The clause being rewritten: its `when` condition, null for none, and the key of its `per K:`, INVALID_INDEX for
	//! none or `per ()`.
	const Expression *when = nullptr;
	idx_t class_key = DConstants::INVALID_INDEX;
	//! What the reducers being rewritten are read as: a bound's value, or a coefficient's inside a reducer over
	//! decisions.
	DecideValueRole read_as = DecideValueRole::BOUND;
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
