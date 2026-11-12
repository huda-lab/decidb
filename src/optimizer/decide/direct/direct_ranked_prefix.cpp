#include "duckdb/optimizer/decide/direct/direct_ranked_prefix.hpp"

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

DirectRankedPrefix::DirectRankedPrefix(Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
                                       const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p,
                                       const vector<reference<const DirectSharedScope>> &scopes,
                                       const DirectFixedValues &fixed_p)
    : optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context), source_bindings(source_bindings_p),
      source_types(source_types_p), errors(errors_p), fixed(fixed_p) {
	D_ASSERT(source_bindings.size() == source_types.size());
	D_ASSERT(!scopes.empty());
	for (auto &scope : scopes) {
		levels.emplace_back(scope.get());
	}
}

idx_t DirectRankedPrefix::AddGroupBound(idx_t level, const DecideGroupBound &bound) {
	levels[level].bounds.push_back(&bound);
	return levels[level].bounds.size() - 1;
}

const DirectGroupValues &DirectRankedPrefix::GroupValues(idx_t level, idx_t bound) const {
	return levels[level].bound_values[bound];
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

void DirectRankedPrefix::PartitionByGroup(const DirectScopeColumns &level, BoundWindowExpression &window) const {
	for (idx_t i = 0; i < level.keys.size(); i++) {
		window.partitions.push_back(calc.Out(Column(level.key.types[i], level.keys[i])));
	}
}

void DirectRankedPrefix::OverRank(const DirectScopeColumns &level, BoundWindowExpression &window) const {
	PartitionByGroup(level, window);
	window.orders.emplace_back(RankOrder(), OrderByNullType::NULLS_LAST, calc.Out(RankKey()));
}

ExprPtr DirectRankedPrefix::Keeps(idx_t level, const DirectRankedColumns &rows) const {
	auto keeps = WithinLimit(0, rows);
	for (idx_t i = 1; i <= level; i++) {
		keeps = calc.And(keeps, WithinLimit(i, rows));
	}
	return keeps;
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

//! Moves the checks of `from` to the end of `to`.
static void AppendChecks(vector<unique_ptr<Expression>> &to, vector<unique_ptr<Expression>> from) {
	for (auto &check : from) {
		to.push_back(std::move(check));
	}
}

DirectScopeState DirectRankedPrefix::ProjectScopes(unique_ptr<LogicalOperator> rows) {
	// S: the flags of the rows the clauses fix, over the source, and the scope of each level, in one projection.
	vector<unique_ptr<Expression>> flags;
	if (HasFixes()) {
		flags.push_back(DirectAnyCondition(calc, fixed.one_conditions));
		flags.push_back(DirectAnyCondition(calc, fixed.zero_conditions));
	}
	vector<DirectScopeInput> scopes;
	for (auto &level : levels) {
		// A `per K:` clause has no instance when every kept row has a NULL K, which the scope projection reads as a
		// flag.
		unique_ptr<Expression> instance;
		if (level.scope.gated) {
			ColumnBinding flag;
			rows = DirectAddInstanceFlag(optimizer, std::move(rows), level.scope.when.get(), level.scope.classes, flag);
			instance = calc.Out(calc.Col(LogicalType::BOOLEAN, flag));
		}
		scopes.push_back({level.scoped, level.scope.when.get(), &level.scope.keys, std::move(instance)});
	}
	return DirectProjectScopes(binder, std::move(rows), std::move(scopes), std::move(flags));
}

void DirectRankedPrefix::LayoutGroupInputs(DirectProjectionStage &p1, const DirectScopeState &state,
                                           vector<unique_ptr<Expression>> &checks) {
	auto over_state = [&](const Expression &value) {
		return OverState(state, value);
	};
	for (idx_t i = 0; i < levels.size(); i++) {
		auto &level = levels[i];
		auto eligible = level.scoped ? Column(LogicalType::BOOLEAN, state.scopes[i].eligible) : nullptr;
		for (auto bound : level.bounds) {
			level.bound_inputs.push_back(
			    DirectLayoutGroupInputs(optimizer, calc, p1, *bound, eligible, over_state, &checks));
		}
	}
}

void DirectRankedPrefix::AppendGroupMessages(vector<unique_ptr<Expression>> &checks) const {
	for (auto &level : levels) {
		for (auto &values : level.bound_values) {
			for (auto &message : values.messages) {
				checks.push_back(DirectRaiseMessage(optimizer, calc, message));
			}
		}
	}
}

bool DirectRankedPrefix::HasGroupBounds() const {
	for (auto &level : levels) {
		if (!level.bounds.empty()) {
			return true;
		}
	}
	return false;
}

DirectRankedPrefix::Stage DirectRankedPrefix::ProjectRows(unique_ptr<LogicalOperator> rows) {
	auto state = ProjectScopes(std::move(rows));
	auto over_state = [&](const Expression &value) {
		return OverState(state, value);
	};

	// P1: what each group bound reads, the rule's columns, and the checks below the rank window.
	Stage result;
	auto &columns = result.columns;
	DirectProjectionStage p1(binder.GenerateTableIndex());
	vector<ColumnBinding> source(state.input_bindings.begin(),
	                             state.input_bindings.begin() + NumericCast<int64_t>(source_bindings.size()));
	columns.source = p1.Forward(source, source_types);
	for (idx_t i = 0; i < levels.size(); i++) {
		columns.levels.emplace_back(p1.Forward(state.scopes[i], levels[i].scoped));
	}
	if (HasFixes()) {
		columns.fixed_one = p1.Add(FixedOne(state));
		columns.fixed_zero = p1.Add(FixedZero(state));
	}
	vector<unique_ptr<Expression>> checks_below;
	LayoutGroupInputs(p1, state, result.checks);
	key_fixed_one = columns.fixed_one;
	key_fixed_zero = columns.fixed_zero;
	// The checks of the rows the clauses fix: each value a row fixes, and a row fixed to both values.
	for (auto &fixed_value : fixed.source_values) {
		checks_below.push_back(DirectRaiseMessage(
		    optimizer, calc, DirectFixedValueMessage(optimizer.context, calc, fixed_value, over_state)));
	}
	if (HasFixes()) {
		auto both = calc.If(FixedOne(state), FixedZero(state), calc.Bool(false));
		checks_below.push_back(DirectRaiseIf(calc, calc.Out(both), DirectInfeasibleErrorPredicate(optimizer, errors)));
	}
	auto layout = LayoutRows(p1, state);
	rank_key = layout.rank_key;
	AppendChecks(checks_below, std::move(layout.checks_below));
	AppendChecks(result.checks, std::move(layout.checks_above));
	if (!checks_below.empty()) {
		columns.row_valid = p1.Add(DirectAllHold(calc, std::move(checks_below)));
	}
	result.plan = p1.Finish(std::move(state.plan));
	return result;
}

unique_ptr<BoundWindowExpression> DirectRankedPrefix::RankWindow(idx_t level, const DirectRankedColumns &rows) const {
	unique_ptr<BoundWindowExpression> rank;
	if (level == 0) {
		rank =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
		rank->start = WindowBoundary::UNBOUNDED_PRECEDING;
		rank->end = WindowBoundary::UNBOUNDED_FOLLOWING;
	} else {
		// The rank among the rows the level before keeps: a row it does not keep adds nothing to the count.
		rank = DirectWindowMatchingCount(calc, calc.Out(Keeps(level - 1, rows)));
		rank->end = WindowBoundary::CURRENT_ROW_ROWS;
	}
	OverRank(rows.levels[level], *rank);
	return rank;
}

void DirectRankedPrefix::RankRows(Stage &rows) {
	// W1: per level, the rank and the rule's windows share one partition and one order, so DuckDB sorts once.
	auto &columns = rows.columns;
	for (idx_t i = 0; i < levels.size(); i++) {
		auto &level = levels[i];
		auto &at = columns.levels[i];
		auto w1 = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
		w1->expressions.push_back(RankWindow(i, columns));
		at.rank = ColumnBinding(w1->window_index, 0);
		AddGroupWindows(i, *w1, columns);
		DirectWindowSink sink(*w1, [&](BoundWindowExpression &aggregate) { OverRank(at, aggregate); });
		auto in_group = level.scoped ? Column(LogicalType::BOOLEAN, at.eligible) : nullptr;
		for (idx_t j = 0; j < level.bounds.size(); j++) {
			level.bound_values.push_back(
			    DirectAddGroupBound(calc, *level.bounds[j], level.bound_inputs[j], sink, in_group));
		}
		w1->children.push_back(std::move(rows.plan));
		rows.plan = std::move(w1);
	}
}

void DirectRankedPrefix::HoistGroupValues(Stage &ranked, ColumnBindingReplacer &forwarded) {
	// PL: what each group bound gives (its limits and its messages) is computed once per row, here, and P2 reads the
	// columns (loop-invariant code motion): the expressions are arithmetic over the group's aggregates, and a rule
	// reads them at several places, some of them under a CASE, where DuckDB's common subexpression elimination does not
	// look. P2 reads P1's and W1's columns too, so PL forwards every column W1 has, and P2's expressions are re-pointed
	// at the forwards (HoistGroupValues's caller).
	ranked.plan->ResolveOperatorTypes();
	auto bindings = ranked.plan->GetColumnBindings();
	DirectProjectionStage pl(binder.GenerateTableIndex());
	auto moved = pl.Forward(bindings, ranked.plan->types);
	for (idx_t i = 0; i < bindings.size(); i++) {
		forwarded.replacement_bindings.emplace_back(bindings[i], moved[i]);
	}
	auto hoist = [&](ExprPtr &value) {
		if (value) {
			value = Column(value->return_type, pl.Add(calc.Out(value)));
		}
	};
	for (auto &level : levels) {
		for (auto &values : level.bound_values) {
			hoist(values.upper);
			hoist(values.lower);
			for (auto &message : values.messages) {
				hoist(message);
			}
		}
	}
	ranked.plan = pl.Finish(std::move(ranked.plan));
}

DirectRankedPrefix::Stage DirectRankedPrefix::ProjectPrefix(Stage ranked) {
	// P2: the rule's prefix columns, and the checks that read W1's outputs.
	auto &input = ranked.columns;
	Stage result;
	auto &second = result.columns;
	ColumnBindingReplacer forwarded;
	bool has_bounds = HasGroupBounds();
	if (has_bounds) {
		HoistGroupValues(ranked, forwarded);
	}
	optional_ptr<LogicalOperator> hoisted = has_bounds ? ranked.plan.get() : nullptr;
	DirectProjectionStage p2(binder.GenerateTableIndex());
	second.source = p2.Forward(input.source, source_types);
	for (idx_t i = 0; i < levels.size(); i++) {
		second.levels.emplace_back(p2.Forward(input.levels[i], levels[i].scoped));
	}
	if (HasFixes()) {
		second.fixed_one = p2.Add(Column(LogicalType::BOOLEAN, input.fixed_one));
		second.fixed_zero = p2.Add(Column(LogicalType::BOOLEAN, input.fixed_zero));
	}
	for (idx_t i = 0; i < levels.size(); i++) {
		second.levels[i].rank = p2.Add(calc.Out(Column(LogicalType::BIGINT, input.levels[i].rank)));
	}
	if (input.row_valid.table_index != DConstants::INVALID_INDEX) {
		second.row_valid = p2.Add(Column(LogicalType::BOOLEAN, input.row_valid));
	}
	auto checks = std::move(ranked.checks);
	AppendChecks(checks, LayoutPrefix(p2, input));
	AppendGroupMessages(checks);
	if (!checks.empty()) {
		second.valid = p2.Add(DirectAllHold(calc, std::move(checks)));
	}
	result.plan = p2.Finish(std::move(ranked.plan));
	if (hoisted) {
		forwarded.stop_operator = hoisted;
		forwarded.VisitOperator(*result.plan);
	}
	return result;
}

unique_ptr<Expression> DirectRankedPrefix::OwedChecks(const DirectRankedColumns &columns,
                                                      unique_ptr<Expression> verdict) const {
	// The row's checks, then the group's, then the verdict: an earlier check guards the later ones.
	vector<unique_ptr<Expression>> checks;
	if (columns.row_valid.table_index != DConstants::INVALID_INDEX) {
		checks.push_back(calc.Out(Column(LogicalType::BOOLEAN, columns.row_valid)));
	}
	if (columns.valid.table_index != DConstants::INVALID_INDEX) {
		checks.push_back(calc.Out(Column(LogicalType::BOOLEAN, columns.valid)));
	}
	if (verdict) {
		checks.push_back(std::move(verdict));
	}
	return checks.empty() ? nullptr : DirectAllHold(calc, std::move(checks));
}

ExprPtr DirectRankedPrefix::AllLevelsMet(const DirectRankedColumns &columns) {
	ExprPtr met;
	for (idx_t i = 0; i < levels.size(); i++) {
		auto verdict = GroupVerdict(i, columns);
		if (!verdict) {
			continue;
		}
		// A row outside every group of the level has no group to meet.
		if (levels[i].scoped) {
			verdict = calc.If(Column(LogicalType::BOOLEAN, columns.levels[i].eligible), verdict, calc.Bool(true));
		}
		met = met ? calc.And(met, verdict) : verdict;
	}
	return met;
}

void DirectRankedPrefix::CloseGroups(Stage &prefix) {
	auto &columns = prefix.columns;
	// W2: the size of each group's prefix, where the rule needs a window for it.
	if (auto size_window = PrefixSize(columns)) {
		auto w2 = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
		columns.size = Column(size_window->return_type, ColumnBinding(w2->window_index, 0));
		w2->expressions.push_back(std::move(size_window));
		w2->children.push_back(std::move(prefix.plan));
		prefix.plan = std::move(w2);
	}
	// Whether every group can be met, where a group can fail.
	auto verdict = AllLevelsMet(columns);
	// A check above the last window reaches a later group only when the parent reads that far: the rows are held, and
	// the check is raised when they are. P2's checks stand below W2 when there is one, and so are held by it.
	bool owes_above = verdict || (columns.valid.table_index != DConstants::INVALID_INDEX && !columns.size);
	if (!owes_above) {
		return;
	}
	unique_ptr<Expression> raise;
	if (verdict) {
		raise = DirectRaiseUnless(calc, calc.Out(verdict), DirectInfeasibleErrorPredicate(optimizer, errors));
	}
	prefix.plan->ResolveOperatorTypes();
	auto &hold = prefix.hold;
	hold.rows = make_uniq<DirectHeldRows>(binder, "direct_group", prefix.plan->GetColumnBindings(), prefix.plan->types);
	hold.checks = hold.rows->Add(OwedChecks(columns, std::move(raise)));
	hold.reference = hold.rows->Read();
	hold.definition = std::move(prefix.plan);
	prefix.plan = std::move(hold.reference.plan);
}

DirectSubstitute DirectRankedPrefix::Answer(Stage prefix) {
	// A: the source columns, x and the column of the checks, which keeps them live (`validated_columns`). DuckDB keeps
	// a window whose columns no parent reads, so the rank window holds every row before one leaves.
	auto &columns = prefix.columns;
	DirectProjectionStage out(binder.GenerateTableIndex());
	for (idx_t i = 0; i < source_bindings.size(); i++) {
		out.Add(calc.Out(Column(source_types[i], columns.source[i])));
	}
	auto chosen = Chosen(columns);
	if (HasFixes()) {
		// A clause that fixes the row decides it.
		chosen = calc.If(Column(LogicalType::BOOLEAN, columns.fixed_one), calc.Bool(true),
		                 calc.If(Column(LogicalType::BOOLEAN, columns.fixed_zero), calc.Bool(false), chosen));
	}
	out.Add(calc.Out(calc.If(chosen, calc.Constant(Value::INTEGER(1)), calc.Constant(Value::INTEGER(0)))));
	// The held rows have raised every check already, so A reads the column that holds them.
	auto &hold = prefix.hold;
	auto checks = hold.rows ? calc.Out(Column(LogicalType::BOOLEAN, hold.checks)) : OwedChecks(columns, nullptr);
	bool owes_checks = checks != nullptr;
	if (owes_checks) {
		out.Add(std::move(checks));
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
	if (hold.rows) {
		// A reads the definition's columns and the hold's, which the reference has under bindings of its own.
		hold.reference.replacer.VisitOperator(*substitute.child);
		substitute.child = hold.rows->Close(std::move(hold.definition), std::move(substitute.child));
	}
	return substitute;
}

DirectSubstitute DirectRankedPrefix::Build(unique_ptr<LogicalOperator> rows) {
	auto ranked = ProjectRows(std::move(rows));
	RankRows(ranked);
	auto prefix = ProjectPrefix(std::move(ranked));
	CloseGroups(prefix);
	return Answer(std::move(prefix));
}

} // namespace duckdb
