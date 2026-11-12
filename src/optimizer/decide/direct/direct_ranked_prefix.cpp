#include "duckdb/optimizer/decide/direct/direct_ranked_prefix.hpp"

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

DirectRankedPrefix::DirectRankedPrefix(Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
                                       const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p,
                                       const DirectSharedScope &scope_p, const DirectFixedValues &fixed_p)
    : optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context), source_bindings(source_bindings_p),
      source_types(source_types_p), errors(errors_p), scope(scope_p), fixed(fixed_p), scoped(scope_p.Scoped()) {
	D_ASSERT(source_bindings.size() == source_types.size());
}

idx_t DirectRankedPrefix::AddGroupBound(const DecideGroupBound &bound) {
	bounds.push_back(&bound);
	return bounds.size() - 1;
}

const DirectGroupValues &DirectRankedPrefix::GroupValues(idx_t bound) const {
	return bound_values[bound];
}

ExprPtr DirectRankedPrefix::RankKey() const {
	auto key = Column(LogicalType::DOUBLE, rank_key);
	if (!HasFixes()) {
		return key;
	}
	// A row a clause fixes to 1 or to 0 leaves the ranking: NULL sorts last, and a sum skips it.
	auto outside = calc.Constant(Value(LogicalType::DOUBLE));
	return calc.If(Column(LogicalType::BOOLEAN, key_fixed_one), outside,
	               calc.If(Column(LogicalType::BOOLEAN, key_fixed_zero), outside, key));
}

void DirectRankedPrefix::PartitionByGroup(const DirectCarried &carried, BoundWindowExpression &window) const {
	for (idx_t i = 0; i < carried.keys.size(); i++) {
		window.partitions.push_back(calc.Out(Column(carried.key.types[i], carried.keys[i])));
	}
}

ExprPtr DirectRankedPrefix::FixedOne(const DirectScopeState &state) const {
	return Column(LogicalType::BOOLEAN, state.extra[0]);
}

ExprPtr DirectRankedPrefix::FixedZero(const DirectScopeState &state) const {
	return Column(LogicalType::BOOLEAN, state.extra[1]);
}

unique_ptr<Expression> DirectRankedPrefix::OverState(const DirectScopeState &state, const Expression &value) const {
	return state.state_index == DConstants::INVALID_INDEX
	           ? value.Copy()
	           : DirectOverProjection(value, source_bindings, state.state_index);
}

DirectRankedPrefix::Stage DirectRankedPrefix::ProjectRows(unique_ptr<LogicalOperator> rows) {
	// S: the flags of the rows the clauses fix, over the source, in the scope projection.
	vector<unique_ptr<Expression>> flags;
	if (HasFixes()) {
		flags.push_back(DirectAnyCondition(calc, fixed.one_conditions));
		flags.push_back(DirectAnyCondition(calc, fixed.zero_conditions));
	}
	// A `per K:` clause has no instance when every kept row has a NULL K, which the scope projection reads as a flag.
	unique_ptr<Expression> instance;
	if (scope.gated) {
		ColumnBinding flag;
		rows = DirectAddInstanceFlag(optimizer, std::move(rows), scope.when.get(), scope.classes, flag);
		instance = calc.Out(calc.Col(LogicalType::BOOLEAN, flag));
	}
	auto state = DirectProjectScope(binder, std::move(rows), scoped, scope.when.get(), scope.keys, std::move(flags),
	                                std::move(instance));
	auto from_scope = DirectCarriedOf(state, source_bindings.size());
	auto over_state = [&](const Expression &value) {
		return OverState(state, value);
	};

	// P1: what each group bound reads, the rule's columns, and the checks below the rank window.
	Stage result;
	DirectProjectionStage p1(binder.GenerateTableIndex());
	result.columns.carried = p1.Forward(from_scope, source_types, scoped);
	if (HasFixes()) {
		result.columns.fixed_one = p1.Add(FixedOne(state));
		result.columns.fixed_zero = p1.Add(FixedZero(state));
	}
	auto eligible = scoped ? Column(LogicalType::BOOLEAN, from_scope.eligible) : nullptr;
	for (auto bound : bounds) {
		bound_inputs.push_back(
		    DirectLayoutGroupInputs(optimizer, calc, p1, *bound, eligible, over_state, &checks_above));
	}
	key_fixed_one = result.columns.fixed_one;
	key_fixed_zero = result.columns.fixed_zero;
	// The checks of the rows the clauses fix: each value a row fixes, and a row fixed to both values.
	for (auto &fixed_value : fixed.source_values) {
		checks_below.push_back(DirectRaiseMessage(
		    optimizer, calc, DirectFixedValueMessage(optimizer.context, calc, fixed_value, over_state)));
	}
	if (HasFixes()) {
		auto both = calc.If(FixedOne(state), FixedZero(state), calc.Bool(false));
		checks_below.push_back(DirectRaiseIf(calc, calc.Out(both), DirectInfeasibleErrorPredicate(optimizer, errors)));
	}
	rank_key = LayoutRows(p1, state);
	if (!checks_below.empty()) {
		result.columns.row_valid = p1.Add(DirectAllHold(calc, std::move(checks_below)));
	}
	result.plan = p1.Finish(std::move(state.plan));
	return result;
}

void DirectRankedPrefix::RankRows(Stage &rows) {
	// W1: the rank and the rule's windows share one partition and one order, so DuckDB sorts once.
	auto &columns = rows.columns;
	auto w1 = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
	auto rank =
	    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
	rank->start = WindowBoundary::UNBOUNDED_PRECEDING;
	rank->end = WindowBoundary::UNBOUNDED_FOLLOWING;
	PartitionByGroup(columns.carried, *rank);
	rank->orders.emplace_back(RankOrder(), OrderByNullType::NULLS_LAST, calc.Out(RankKey()));
	w1->expressions.push_back(std::move(rank));
	columns.rank = ColumnBinding(w1->window_index, 0);
	AddGroupWindows(*w1, columns);
	DirectWindowSink sink(*w1, [&](BoundWindowExpression &aggregate) { PartitionByGroup(columns.carried, aggregate); });
	auto in_group = scoped ? Column(LogicalType::BOOLEAN, columns.carried.eligible) : nullptr;
	for (idx_t i = 0; i < bounds.size(); i++) {
		bound_values.push_back(DirectAddGroupBound(calc, *bounds[i], bound_inputs[i], sink, in_group));
	}
	w1->children.push_back(std::move(rows.plan));
	rows.plan = std::move(w1);
}

DirectRankedPrefix::Stage DirectRankedPrefix::ProjectPrefix(Stage ranked) {
	// P2: the rule's prefix columns, and the checks that read W1's outputs.
	auto &input = ranked.columns;
	Stage result;
	auto &second = result.columns;
	DirectProjectionStage p2(binder.GenerateTableIndex());
	second.carried = p2.Forward(input.carried, source_types, scoped);
	if (HasFixes()) {
		second.fixed_one = p2.Add(Column(LogicalType::BOOLEAN, input.fixed_one));
		second.fixed_zero = p2.Add(Column(LogicalType::BOOLEAN, input.fixed_zero));
	}
	second.rank = p2.Add(calc.Out(Column(LogicalType::BIGINT, input.rank)));
	if (input.row_valid.table_index != DConstants::INVALID_INDEX) {
		second.row_valid = p2.Add(Column(LogicalType::BOOLEAN, input.row_valid));
	}
	LayoutPrefix(p2, input);
	for (auto &values : bound_values) {
		for (auto &message : values.messages) {
			checks_above.push_back(DirectRaiseMessage(optimizer, calc, message));
		}
	}
	if (!checks_above.empty()) {
		second.valid = p2.Add(DirectAllHold(calc, std::move(checks_above)));
	}
	result.plan = p2.Finish(std::move(ranked.plan));
	return result;
}

void DirectRankedPrefix::CloseGroups(Stage &prefix, ExprPtr &feasible) {
	auto &columns = prefix.columns;
	// W2: the size of each group's prefix, where the rule needs a window for it.
	if (auto size_window = PrefixSize(columns)) {
		auto w2 = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
		PartitionByGroup(columns.carried, *size_window);
		columns.size = Column(size_window->return_type, ColumnBinding(w2->window_index, 0));
		w2->expressions.push_back(std::move(size_window));
		w2->children.push_back(std::move(prefix.plan));
		prefix.plan = std::move(w2);
	}
	// W3: whether every group can be met, over all the rows, where a group can fail.
	if (auto verdict = GroupVerdict(columns)) {
		// A row outside every group has no group to meet.
		if (scoped) {
			verdict = calc.If(Column(LogicalType::BOOLEAN, columns.carried.eligible), verdict, calc.Bool(true));
		}
		auto w3 = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
		w3->expressions.push_back(DirectWindowExtremum(optimizer, "bool_and", calc.Out(verdict)));
		feasible = Column(LogicalType::BOOLEAN, ColumnBinding(w3->window_index, 0));
		w3->children.push_back(std::move(prefix.plan));
		prefix.plan = std::move(w3);
	}
	D_ASSERT(columns.valid.table_index == DConstants::INVALID_INDEX || columns.size || feasible);
}

DirectSubstitute DirectRankedPrefix::Answer(Stage prefix, const ExprPtr &feasible) {
	// A: the source columns, x and the column of the checks, which keeps them live (`validated_columns`). DuckDB keeps
	// a window whose columns no parent reads, so the rank window holds every row before one leaves.
	auto &columns = prefix.columns;
	DirectProjectionStage out(binder.GenerateTableIndex());
	for (idx_t i = 0; i < source_bindings.size(); i++) {
		out.Add(calc.Out(Column(source_types[i], columns.carried.source[i])));
	}
	auto chosen = Chosen(columns);
	if (HasFixes()) {
		// A clause that fixes the row decides it.
		chosen = calc.If(Column(LogicalType::BOOLEAN, columns.fixed_one), calc.Bool(true),
		                 calc.If(Column(LogicalType::BOOLEAN, columns.fixed_zero), calc.Bool(false), chosen));
	}
	out.Add(calc.Out(calc.If(chosen, calc.Constant(Value::INTEGER(1)), calc.Constant(Value::INTEGER(0)))));
	vector<unique_ptr<Expression>> checks;
	if (columns.row_valid.table_index != DConstants::INVALID_INDEX) {
		checks.push_back(calc.Out(Column(LogicalType::BOOLEAN, columns.row_valid)));
	}
	if (columns.valid.table_index != DConstants::INVALID_INDEX) {
		checks.push_back(calc.Out(Column(LogicalType::BOOLEAN, columns.valid)));
	}
	if (feasible) {
		checks.push_back(
		    DirectRaiseUnless(calc, calc.Out(feasible), DirectInfeasibleErrorPredicate(optimizer, errors)));
	}
	bool owes_checks = !checks.empty();
	if (owes_checks) {
		out.Add(DirectAllHold(calc, std::move(checks)));
	}

	DirectSubstitute substitute;
	substitute.prunable_decisions.push_back(1);
	for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
		substitute.slot_map.push_back(i);
	}
	if (owes_checks) {
		substitute.validated_columns.push_back(source_bindings.size() + 1);
	}
	substitute.child = out.Finish(std::move(prefix.plan));
	return substitute;
}

DirectSubstitute DirectRankedPrefix::Build(unique_ptr<LogicalOperator> rows) {
	auto ranked = ProjectRows(std::move(rows));
	RankRows(ranked);
	auto prefix = ProjectPrefix(std::move(ranked));
	ExprPtr feasible;
	CloseGroups(prefix, feasible);
	return Answer(std::move(prefix), feasible);
}

} // namespace duckdb
