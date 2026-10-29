//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_rewrite_rhs_window.cpp
//
// The bound-side evaluator. See decide_optimizer.cpp.
//
// A bound is data: `SUM(x) <= SUM(cap)`, `SUM(x) BY (g) <= cap`. Every value a bound needs that is not one row's
// value is computed here, once, as a column of the DECIDE source:
//   - each reducer on a bound (`sum(cap)`, `sum(per D: cap) by (g)`), as the same DuckDB aggregate over a window of
//     its groups, so the surrounding arithmetic (`COUNT(*) * k`) is evaluated in DuckDB's own types;
//   - the tightest bound of a clause in the aggregate shape: such a clause is one constraint per group, and the
//     constraint on each kept row of the group holds when the tightest one does (paper §3.2.1), so `<=` takes the
//     group's MIN and `>=` its MAX; `=` must have one value per group.
// The solver (PhysicalDecide) and the direct-solve rules then read plain columns, and neither reduces a bound.
//
// The values the windows read are checked by VALUE_IS_VALID checks appended to the DECIDE node's scope checks, so a
// NaN, or an `=` bound with two values in a group, is reported after the scope checks, as the bound's reader would
// report it. A NULL bound is no value: the window's reducer skips it as SQL's does, and a group whose tightest bound
// is NULL has no instance, which the bound's reader skips. A window itself raises nothing.
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
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

namespace {

//! The windows one DECIDE node's bounds need, collected while the constraint tree is walked and installed as a stack
//! of LogicalWindows directly above DECIDE's child: a window that reads another window's column stands on the level
//! above it. A window keeps its child's bindings and appends its own, so no binding the DECIDE node holds changes.
//!
//! Every window is partitioned by whether the clause's `when` keeps the row (when it has one): the rows the clause
//! drops form partitions no constraint reads, and nothing a window computes is evaluated on them. A group a reducer's
//! own `when` leaves empty reads NULL, and a NULL bound skips its instance.
class RhsWindowRewriter {
public:
	RhsWindowRewriter(Optimizer &optimizer_p, LogicalDecide &decide_p)
	    : optimizer(optimizer_p), decide(decide_p), calc(optimizer_p.context),
	      canonicalizer(optimizer_p.context, decide_p.decide_index, decide_p.variable_scopes) {
		canonicalizer.ReadBooleanDecisions(decide_p.is_boolean_var);
	}

	void Run() {
		// The parser requires a SUCH THAT clause with at least one constraint.
		D_ASSERT(decide.decide_constraints);
		Walk(*decide.decide_constraints);
		Install();
	}

	//! Builds the windows of the bounds `Run` recorded: the tightest value of each, and the message of an `=` bound
	//! that takes two. The solver path needs them; a direct rule reduces the bound itself.
	void InstallRecorded() {
		idx_t inserted = 0;
		for (auto &bound : decide.reduced_bounds) {
			auto &comparison = bound.comparison->Cast<BoundComparisonExpression>();
			when = bound.when;
			auto disagreement = ReduceToTightest(comparison, bound.text);
			auto position = decide.scope_checks.begin() + NumericCast<int64_t>(bound.check_position + inserted);
			for (auto message : {&bound.invalid, &disagreement}) {
				if (!*message) {
					continue;
				}
				DecideScopeCheck check;
				check.kind = DecideScopeCheckKind::VALUE_IS_VALID;
				check.value = std::move(*message);
				position = decide.scope_checks.insert(position, std::move(check)) + 1;
				inserted++;
			}
		}
		decide.reduced_bounds.clear();
		Install();
	}

private:
	//! One LogicalWindow of the stack: level 0 reads the source, level k also the columns of the levels below it.
	struct Level {
		idx_t table_index;
		vector<unique_ptr<Expression>> windows;
	};
	//! A reducer's column and the `by` key it has one value per group of.
	struct ReducerColumn {
		ColumnBinding binding;
		vector<const Expression *> keys;
	};

	void Walk(Expression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conjunction = expr.Cast<BoundConjunctionExpression>();
			auto saved_when = when;
			auto saved_class_key = class_key;
			if (IsWhenConstraintWrapper(conjunction)) {
				D_ASSERT(conjunction.children.size() == 2);
				when = conjunction.children[1].get();
			}
			// A `per K:` wrapper groups no reducer: its child is read like any other clause. Its key says which rows
			// are in a class.
			if (IsPerClassWrapper(conjunction)) {
				class_key = PerWrapperKey(conjunction);
			}
			for (idx_t i = 0; i < conjunction.children.size(); i++) {
				if (IsConstraintChild(conjunction, i)) {
					Walk(*conjunction.children[i]);
				}
			}
			when = saved_when;
			class_key = saved_class_key;
			return;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			RewriteBound(expr.Cast<BoundComparisonExpression>());
		}
	}

	//! A clause in the aggregate shape is one constraint per group. Its data bound is checked on every kept row, before
	//! any value is read, and a bound that may differ between the kept rows of a group is reduced to the tightest one.
	//! `<>` keeps every value: each one is excluded. A query-wide value has one value already. A row-wise bound is
	//! checked by the solver as it reads it. A bound to reduce is only recorded; the solver path builds its window
	//! later (InstallRecorded).
	void RewriteBound(BoundComparisonExpression &comparison) {
		// Canonicalization put every decision on the left, so the right is data.
		bool aggregate_data_bound =
		    !comparison.right->IsFoldable() &&
		    canonicalizer.ClassifyCanonicalComparison(comparison) == CanonicalConstraintClass::AGGREGATE;
		bool query_wide = IsQueryWideBoundTag(comparison.right->GetAlias());
		auto text = Written(*comparison.right);
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
			Record(comparison, text, std::move(invalid));
			return;
		}
		AddValueCheck(std::move(invalid));
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

	//! Leaves the bound raw. Its checks stand where they would have, in clause order, once the windows are built.
	void Record(BoundComparisonExpression &comparison, const string &text, unique_ptr<Expression> invalid) {
		DecideReducedBound bound;
		bound.comparison = &comparison;
		bound.when = when;
		bound.text = text;
		bound.invalid = std::move(invalid);
		bound.equality = comparison.type == ExpressionType::COMPARE_EQUAL;
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
	//! kept rows of its `by` group that pass its own `when`), counting each class of its `per` key once.
	unique_ptr<Expression> MakeReducerColumn(const BoundAggregateExpression &reducer) {
		auto name = StringUtil::Lower(reducer.function.name);
		D_ASSERT(name == "count" || name == "count_star" || name == "min" || name == "max" || name == "sum" ||
		         name == "avg");
		// The binder rejects DISTINCT and drops an ORDER BY these functions ignore, canonicalization gave every norm of
		// data its definition, and the AVG rewrite of `<>` runs after this pass.
		string payload;
		D_ASSERT(!reducer.IsDistinct() && !reducer.order_bys && !HasDecideTag(reducer.GetAlias(), AVG_REWRITE_TAG) &&
		         !ExtractDecideTagPayload(reducer.GetAlias(), NORM_MARKER_TAG_PREFIX, payload));
		auto reads = Reads(reducer);
		// A reducer reads its argument as the same SQL aggregate does: it skips a NULL. A NaN is an error, except to
		// COUNT, which counts it.
		if (!reducer.children.empty() && name != "count") {
			// A norm's definition can hide a NaN (an L0 count's SIGN reads it as 0), so the value checked is the body.
			auto body = DecideWrittenNormBody(reducer);
			auto argument = body ? body : reducer.children[0].get();
			auto nan = NanMessage(*argument, Written(*argument));
			if (nan) {
				AddValueCheck(calc.Out(OnRows(reads, InGroup(reducer, nan))));
			}
		}
		auto keys = GroupKeyOf(reducer);
		// Hand-built: a window is an operator-specific node, which the builder (scalar expressions) has no operation
		// for.
		auto window = make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_AGGREGATE, reducer.return_type,
		                                               make_uniq<AggregateFunction>(reducer.function),
		                                               reducer.bind_info ? reducer.bind_info->Copy() : nullptr);
		window->start = WindowBoundary::UNBOUNDED_PRECEDING;
		window->end = WindowBoundary::UNBOUNDED_FOLLOWING;
		AddGroupPartitions(*window, keys);
		auto counted = reads;
		idx_t scope_idx;
		if (TryParseQualifiedReducerTag(reducer.GetAlias(), scope_idx)) {
			auto first = calc.Cmp(ExpressionType::COMPARE_EQUAL, FirstOfClass(scope_idx, keys, reads),
			                      calc.Constant(Value::BIGINT(1)));
			// A row whose written `per` column is NULL is in no class, so it is not counted.
			for (auto &element : DecideKeyElements(decide.keys, decide.entity_scopes[scope_idx].key)) {
				if (element.null_mode == DecideKeyNullMode::NO_GROUP) {
					first = calc.If(calc.IsNull(calc.Of(*element.expression)), calc.Bool(false), first);
				}
			}
			counted = reads ? calc.If(reads, first, calc.Bool(false)) : first;
		}
		if (counted) {
			window->filter_expr = calc.Out(counted);
		}
		if (!reducer.children.empty()) {
			// Computed only on the rows the reducer reads.
			auto argument = calc.Of(*reducer.children[0]);
			window->children.push_back(calc.Out(OnRows(reads, argument)));
		}
		auto column = Place(std::move(window));
		reducer_columns.push_back({column->Cast<BoundColumnRefExpression>().binding, std::move(keys)});
		// A row whose written `by` column is NULL is in no group: the reducer has no value there, and the NULL bound
		// skips the row's instance (deck p. 50).
		column = InGroup(reducer, column);
		// The column carries the reducer's written text as its name, so every place that quotes a bound still reads
		// `sum(cap) by (g)`; the tag keeps a NULL message from naming it as a column.
		auto result = calc.Out(column);
		auto alias = Written(reducer);
		AddDecideTag(alias, BOUND_REDUCER_TAG);
		result->SetAlias(std::move(alias));
		return result;
	}

	//! `value` on a row in a group of the reducer's `by` key, NULL on a row whose written key column is NULL, which is
	//! in no group. A relation's columns compare NULL as a value.
	ExprPtr InGroup(const BoundAggregateExpression &reducer, ExprPtr value) const {
		idx_t key_index;
		if (!TryParseGroupKeyTag(reducer.GetAlias(), key_index)) {
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

	//! 1 on the first row of each class of the reducer's `per` key (or relation, in the colon form) in its group, among
	//! the rows it reads. That row stands for the class: the key determines the reducer's body (a scope check).
	ExprPtr FirstOfClass(idx_t scope_idx, const vector<const Expression *> &keys, const ExprPtr &reads) {
		D_ASSERT(scope_idx < decide.entity_scopes.size());
		auto &scope = decide.entity_scopes[scope_idx];
		// Only the optimizer's helper decisions have scopes over a link, and they are made after this pass.
		D_ASSERT(scope.value_link == DConstants::INVALID_INDEX);
		auto rank =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
		rank->start = WindowBoundary::UNBOUNDED_PRECEDING;
		rank->end = WindowBoundary::UNBOUNDED_FOLLOWING;
		if (reads) {
			rank->partitions.push_back(calc.Out(reads));
		}
		for (auto key : keys) {
			rank->partitions.push_back(key->Copy());
		}
		// `per ()` has no key column: one class per group.
		for (auto &element : DecideKeyElements(decide.keys, scope.key)) {
			rank->partitions.push_back(calc.Out(calc.Of(element.Grouping())));
		}
		return Place(std::move(rank));
	}

	//! The tightest bound of each group: the MIN of an upper bound, the MAX of a lower one. An `=` bound must have one
	//! value per group, and the message of a row of a group where it has two is returned (null for any other bound).
	unique_ptr<Expression> ReduceToTightest(BoundComparisonExpression &comparison, const string &text) {
		D_ASSERT(DirectIsUpperBound(comparison.type) || DirectIsLowerBound(comparison.type));
		auto kept = Kept();
		auto keys = GroupKeyOf(comparison);
		auto value = OnRows(kept, calc.Of(*comparison.right));
		auto extremum = [&](const char *name) {
			auto window = DirectWindowExtremum(optimizer, name, calc.Out(value));
			AddGroupPartitions(*window, keys);
			return Place(std::move(window));
		};
		auto tightest = extremum(DirectIsUpperBound(comparison.type) ? "min" : "max");
		unique_ptr<Expression> disagreement;
		if (comparison.type == ExpressionType::COMPARE_EQUAL) {
			disagreement = calc.Out(OnRows(kept, DirectBoundDisagreement(calc, text, tightest, extremum("max"))));
		}
		comparison.right = calc.Out(tightest);
		comparison.right->SetAlias(text);
		return disagreement;
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
		return calc.Out(OnRows(rows, nan));
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

	//! A column reading `window`, placed on the lowest level that can read its inputs and shared with an equal window
	//! placed before.
	ExprPtr Place(unique_ptr<BoundWindowExpression> window) {
		auto level = LevelAbove(*window);
		if (level == levels.size()) {
			levels.push_back({optimizer.binder.GenerateTableIndex(), {}});
		}
		auto &placed = levels[level].windows;
		idx_t slot = 0;
		while (slot < placed.size() && !placed[slot]->Equals(*window)) {
			slot++;
		}
		auto type = window->return_type;
		if (slot == placed.size()) {
			placed.push_back(std::move(window));
		}
		return calc.Col(type, ColumnBinding(levels[level].table_index, slot));
	}

	//! The level a window over `expr` stands on: 0 over the source alone, else one above the highest window it reads.
	idx_t LevelAbove(const Expression &expr) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto table_index = expr.Cast<BoundColumnRefExpression>().binding.table_index;
			for (idx_t i = 0; i < levels.size(); i++) {
				if (levels[i].table_index == table_index) {
					return i + 1;
				}
			}
			return 0;
		}
		idx_t level = 0;
		ExpressionIterator::EnumerateChildren(
		    expr, [&](const Expression &child) { level = MaxValue(level, LevelAbove(child)); });
		return level;
	}

	void Install() {
		for (auto &level : levels) {
			auto window = make_uniq<LogicalWindow>(level.table_index);
			window->expressions = std::move(level.windows);
			window->estimated_cardinality = decide.children[0]->estimated_cardinality;
			window->children.push_back(std::move(decide.children[0]));
			decide.children[0] = std::move(window);
		}
	}

	//! A group of the clause: its kept rows (when it has a `when`), by the value of `keys`. The rows whose written key
	//! column is NULL share a partition, whose value MakeReducerColumn replaces with NULL.
	void AddGroupPartitions(BoundWindowExpression &window, const vector<const Expression *> &keys) const {
		if (when) {
			window.partitions.push_back(calc.Out(Kept()));
		}
		for (auto key : keys) {
			window.partitions.push_back(key->Copy());
		}
	}

	vector<const Expression *> GroupKeyOf(const Expression &tagged) const {
		vector<const Expression *> keys;
		idx_t index;
		if (TryParseGroupKeyTag(tagged.GetAlias(), index)) {
			D_ASSERT(index < decide.keys.size());
			for (auto &element : decide.keys[index].elements) {
				keys.push_back(&element.Grouping());
			}
		}
		return keys;
	}

	//! Whether the clause's `when` keeps the row (NULL keeps none); null when it has none.
	ExprPtr Kept() const {
		return when ? calc.Wrap(DirectActiveWhen(calc, when)) : nullptr;
	}

	//! The rows a reducer reads: the clause's kept rows that pass its own `when`; null for every row.
	ExprPtr Reads(const BoundAggregateExpression &reducer) const {
		if (!reducer.filter) {
			return Kept();
		}
		auto own = calc.Wrap(DirectActiveWhen(calc, reducer.filter.get()));
		return when ? calc.If(Kept(), own, calc.Bool(false)) : own;
	}

	//! `value` on `rows` (every row when null), and NULL elsewhere, where it is not computed.
	ExprPtr OnRows(const ExprPtr &rows, const ExprPtr &value) const {
		return rows ? calc.If(rows, value, calc.Constant(Value(value->return_type))) : value;
	}

	string Written(const Expression &expr) const {
		return RenderDecideSource(expr, decide.source_fragments, decide.entity_scopes, &decide.keys);
	}

	Optimizer &optimizer;
	LogicalDecide &decide;
	ExpressionBuilder calc;
	DecideCanonicalizer canonicalizer;
	const Expression *when = nullptr;
	//! The key of the enclosing `per K:`, INVALID_INDEX for none or `per ()`.
	idx_t class_key = DConstants::INVALID_INDEX;
	vector<Level> levels;
	vector<ReducerColumn> reducer_columns;
};

} // namespace

void DecideOptimizer::RewriteRhsAggregates(LogicalDecide &decide) {
	D_ASSERT(decide.children.size() == 1);
	RhsWindowRewriter(optimizer, decide).Run();
}

void InstallReducedBoundWindows(Optimizer &optimizer, LogicalDecide &decide) {
	if (!decide.reduced_bounds.empty()) {
		RhsWindowRewriter(optimizer, decide).InstallRecorded();
	}
}

void DecideOptimizer::InstallReducedBounds(LogicalDecide &decide) {
	InstallReducedBoundWindows(optimizer, decide);
}

} // namespace duckdb
