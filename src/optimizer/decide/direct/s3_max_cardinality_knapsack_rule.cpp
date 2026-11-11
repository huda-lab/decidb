#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <cmath>

#include "duckdb/common/exception.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_s3_max_cardinality_knapsack {

constexpr const char *RULE_NAME = "S3_MAX_CARDINALITY_KNAPSACK";

//! What CheckCondition proved: one row-scoped BOOL decision under one budget `SUM(w*x) <= B`, maximizing how many rows
//! it selects.
struct Condition final : DirectApplicabilityCondition {
	//! The budget's weight terms, all on the one decision; its weight on a row is `sum(sign * coefficient)`.
	DirectLinearPart budget;
	//! The budget's limit B: a constant, or a value of the data read per group (DirectReadLimit).
	DirectLimit limit;
	//! The budget's `by` key: each group has a budget of its own. Empty without a key: one budget for all the rows.
	vector<DirectKeyElement> group_keys;
	//! The rows the budget covers: its `when` (the clause's, with the sum's own), over the source; null for every row.
	//! A row it drops reads no weight and is in no budget.
	unique_ptr<Expression> when;
	//! Whether the objective counts the budget's rows only: its `when` is the budget's, so a row the budget drops adds
	//! nothing to the objective either. Otherwise every row adds one, and a row outside the budget is selected.
	bool objective_on_kept = false;

	//! Whether rows can be outside every budget: a `when` drops them, or a NULL in a `by` key element.
	bool Scoped() const {
		return when || !group_keys.empty();
	}
};

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}

	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (!ProveDecision(ir) || !ProveConstraint(ir)) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		if (!ProveBudget(ir, context, *condition) || !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		return std::move(condition);
	}

	string Summary() const override {
		return "max c*SUM(x), SUM(w*x) <= B";
	}

	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveDecision(const DirectIR &ir) const;
	bool ProveConstraint(const DirectIR &ir) const;
	bool ProveBudget(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveScope(const DirectIR &ir, Condition &condition) const;
	bool ProveWeights(ClientContext &context, const DirectLinearPart &budget) const;
	bool ProveObjectivePart(const DirectIR &ir, ClientContext &context, const DirectPart &part,
	                        unique_ptr<Expression> &kept, double &count) const;
	bool ProveObjectiveRows(const Condition &condition, const unique_ptr<Expression> &kept, bool first,
	                        bool &filtered) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! One decision, BOOL, one per row.
bool Rule::ProveDecision(const DirectIR &ir) const {
	return ir.decisions.size() == 1 && ir.decisions[0].domain == DirectDomain::BOOL &&
	       ir.decisions[0].scope == DecideVarScope::ROW && ir.keyed_scopes.empty();
}

//! The one constraint is the budget, compared with `<=`: a second constraint of any kind is a decline. That it is an
//! aggregate is proved with its scope (ProveScope).
bool Rule::ProveConstraint(const DirectIR &ir) const {
	return ir.constraints.size() == 1 && ir.constraints[0].comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO;
}

//! The budget covers the rows its `when` (on the clause or in the sum) keeps, or every row, in the groups of its `by`
//! key. A `per K:` class gives it a smaller scope, which stays with the solver.
bool Rule::ProveScope(const DirectIR &ir, Condition &condition) const {
	vector<const DirectIRConstraint *> budgets {&ir.constraints[0]};
	DirectSharedScope scope;
	// A gated scope has classes, so the classes decline it too.
	if (!DirectProveSharedScope(ir, budgets, scope) || !scope.classes.empty()) {
		return false;
	}
	condition.when = std::move(scope.when);
	condition.group_keys = std::move(scope.keys);
	return true;
}

//! Each weight is a finite constant or a number of the source row (DirectReadLinearPart has proved the latter, which
//! may raise when evaluated; the plan evaluates it on every row, as the solver does). Constants that add up to exactly
//! 1 or -1 make the sum a count, which is S1's (S1's ReadCountBody sums the signed constants the same way).
bool Rule::ProveWeights(ClientContext &context, const DirectLinearPart &budget) const {
	bool all_constant = true;
	double slope = 0;
	for (auto &term : budget.terms) {
		if (!term.coefficient->IsFoldable()) {
			all_constant = false;
			continue;
		}
		double value;
		if (!TryEvaluateFoldableDouble(context, *term.coefficient, value, DIRECT_FINITE_CONSTANT)) {
			return false;
		}
		slope += term.sign * value;
	}
	return !all_constant || (slope != 1.0 && slope != -1.0);
}

//! `SUM(w*x) <= B`: a plain SUM of linear terms on the decision, in the scope ProveScope proves, with weights
//! ProveWeights proves and a limit the shared reader reads.
bool Rule::ProveBudget(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	auto &ir_node = ir.constraints[0];
	if (!ProveScope(ir, condition)) {
		return false;
	}
	// ProveScope has proved that the clause is a plain SUM.
	auto sum = ir_node.PlainSum();
	return DirectOnlyLinearTerms(sum->terms) &&
	       DirectReadLinearPart(ir, context, *sum, ir_node.written, condition.budget) &&
	       ProveWeights(context, condition.budget) && DirectReadLimit(ir, context, ir_node, condition.limit);
}

//! One objective part: a plain SUM of linear terms, each coefficient a finite constant, on the rows `kept` names (null
//! for every row). `count` is the part's coefficient on the decision in the objective's own (minimizing) direction.
bool Rule::ProveObjectivePart(const DirectIR &ir, ClientContext &context, const DirectPart &part,
                              unique_ptr<Expression> &kept, double &count) const {
	if (!part.IsPlainSum() || !DirectOnlyLinearTerms(part.terms)) {
		return false;
	}
	DirectLinearPart read;
	if (!DirectReadLinearPart(ir, context, part, ir.objective.written, read) ||
	    !DirectObjectivePartKept(ir, part, kept)) {
		return false;
	}
	double slope = 0;
	for (auto &term : read.terms) {
		double value;
		if (!TryEvaluateFoldableDouble(context, *term.coefficient, value, DIRECT_FINITE_CONSTANT)) {
			return false;
		}
		slope += term.sign * value;
	}
	count = DirectOriented(read, slope);
	return true;
}

//! The rows every part of the objective counts are all the rows or exactly the budget's: the same `when`, by expression
//! equality, in every part. A part with a `when` of its own gives the rows of one budget values of 0 and 1, a two-value
//! knapsack, and a decline. `filtered` is what the first part (`first`) settled.
bool Rule::ProveObjectiveRows(const Condition &condition, const unique_ptr<Expression> &kept, bool first,
                              bool &filtered) const {
	bool part_filtered = kept != nullptr;
	if (part_filtered && (!condition.when || !Expression::Equals(*kept, *condition.when))) {
		return false;
	}
	if (first) {
		filtered = part_filtered;
	}
	return part_filtered == filtered;
}

//! The objective counts the selected rows: its parts' coefficients add up to a number below zero (the parts are a
//! minimization, so a count that is maximized is negative) that is finite, on all the rows or on the budget's. Any
//! other objective, none included, is a decline.
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	double total = 0;
	bool filtered = false;
	for (idx_t i = 0; i < ir.objective.parts.size(); i++) {
		unique_ptr<Expression> kept;
		double count;
		if (!ProveObjectivePart(ir, context, ir.objective.parts[i], kept, count) ||
		    !ProveObjectiveRows(condition, kept, i == 0, filtered)) {
			return false;
		}
		total += count;
	}
	condition.objective_on_kept = filtered;
	return std::isfinite(total) && total < 0;
}

//! The plan, over one read of the source, all in windows (no hash aggregate, no join back):
//!
//!   [W0]  the source read
//!   S     the scope projection (DirectProjectScope): the eligible flag and the group key, for a `by` budget
//!   P1    the rows' weights `w` and what the data limit reads
//!   W1    per group, the rows in order of `w`: their running sum of `w` (the prefix total) and their rank; and the
//!         group's limit B (DirectWindowSink)
//!   P2    whether the row's prefix fits (`total <= B`), the rank when it does, and the checks: the weight, what the
//!         limit reads and what it gives
//!   W2    per group, k, the largest rank that fits (NULL when none does)
//!   W3    over all the rows, whether every group fits a prefix; only when a group can fit none (NeedsVerdict)
//!   P4    the source columns, x = 1 when the rank is at most k, and the column that holds the checks
//!
//! The solver reads every row before it answers, so a bad value on the last row raises even under an outer LIMIT 1.
//! W2 holds every row before it gives one, and the checks of P2 stand below it, so every row has passed them when the
//! first leaves. W3 does the same for infeasibility: a group that fits no prefix raises when the first row is
//! released, not when a row of that group is reached.
//!
//! The slot map: the source columns in order (the reader's: the source row id and the bounds' values come after the
//! node's, and the driver drops them), then x (INTEGER 0/1), then the column of the checks, which keeps them live
//! (`validated_columns`). A row outside every budget (a NULL in a NO_GROUP element of the `by` key, D5, or dropped by
//! the budget's `when`) is in no budget and selecting it costs nothing, so x = 1 there, unless the objective drops it
//! too (its `when` is the budget's): then it adds nothing and x = 0.
//!
//! Rows of equal weight are ordered as the sort gives them, and the answer is right only if the rank and the running
//! sum see the same order: with two sorts, tied rows a and b of weight 5 under B = 7 could be ranked a, b by one and
//! summed b, a by the other, and both would be selected. W1's rank and running sum have one partition and one order,
//! so DuckDB sorts them once, and the rank of a row is the position its prefix total was summed at. This relies on
//! PhysicalPlanGenerator::CreatePlan(LogicalWindow) (src/execution/physical_plan/plan_window.cpp), which puts the
//! windows whose BoundWindowExpression::PartitionsAreEquivalent into one PhysicalWindow and so one sort.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p), errors(errors_p),
	      scoped(condition_p.Scoped()) {
		D_ASSERT(source_bindings.size() == source_types.size());
		auto &limit = condition.limit;
		if (limit.constant) {
			return;
		}
		// A limit the rewrite left as a group bound is one already; any other is one value per row, whose tightest
		// value in the group is the limit.
		limit_bound = limit.data.group ? limit.data.group->Copy()
		                               : DirectTightestBound(optimizer, limit.data.comparison, limit.data.value->Copy(),
		                                                     string(), nullptr);
	}

	DirectSubstitute Build(unique_ptr<LogicalOperator> rows) const {
		auto state =
		    DirectProjectScope(binder, std::move(rows), scoped, condition.when.get(), condition.group_keys, {}, nullptr);
		auto from_scope = DirectCarriedOf(state, source_bindings.size());

		DirectProjectionStage p1(binder.GenerateTableIndex());
		auto weighted = LayoutWeights(p1, from_scope, state.state_index);
		auto plan = p1.Finish(std::move(state.plan));

		auto w1 = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
		auto sorted = AddPrefixWindows(*w1, weighted);
		w1->children.push_back(std::move(plan));

		DirectProjectionStage p2(binder.GenerateTableIndex());
		auto fitted = ComputeFits(p2, weighted, sorted);
		plan = p2.Finish(std::move(w1));

		auto w2 = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
		// The largest rank that fits, not the first that fails: with negative weights the prefix totals fall first.
		auto largest = DirectWindowExtremum(optimizer, "max", calc.Out(Column(LogicalType::BIGINT, fitted.fit_rank)));
		PartitionByGroup(fitted.carried, *largest);
		w2->expressions.push_back(std::move(largest));
		auto k = Column(LogicalType::BIGINT, ColumnBinding(w2->window_index, 0));
		w2->children.push_back(std::move(plan));
		plan = std::move(w2);

		ExprPtr feasible;
		if (NeedsVerdict()) {
			auto w3 = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
			w3->expressions.push_back(DirectWindowExtremum(optimizer, "bool_and", calc.Out(GroupHasPrefix(fitted, k))));
			feasible = Column(LogicalType::BOOLEAN, ColumnBinding(w3->window_index, 0));
			w3->children.push_back(std::move(plan));
			plan = std::move(w3);
		}
		return Answer(std::move(plan), fitted, k, feasible);
	}

private:
	//! Columns of P1.
	struct Weighted {
		DirectCarried carried;
		ColumnBinding weight;
		//! Whether the budget's `when` keeps the row; set only when the objective counts the budget's rows only.
		ColumnBinding kept;
		//! What the aggregates of the data limit read, one member per input of the limit's group bound.
		vector<DirectGroupInputColumns> limit_inputs;
		//! The checks of the weight and of what the limit reads. Each reads P1's columns, so P2 evaluates them.
		vector<unique_ptr<Expression>> checks;
	};
	//! Where W1 puts the row's prefix total and rank, and what the group's limit gives.
	struct Sorted {
		ColumnBinding total;
		ColumnBinding rank;
		DirectGroupValues limit;
	};
	//! Columns of P2.
	struct Fitted {
		DirectCarried carried;
		ColumnBinding rank;
		//! The rank when the row's prefix fits the limit, else NULL.
		ColumnBinding fit_rank;
		//! Whether the budget's `when` keeps the row; set only when the objective counts the budget's rows only.
		ColumnBinding kept;
		//! Whether the empty prefix fits the limit; set only when a verdict is needed.
		ColumnBinding empty_fits;
		ColumnBinding valid;
	};

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	const DirectStateErrors &errors;
	//! Whether the budget has a `by` key.
	bool scoped;
	//! The group bound of the data limit; null for a constant limit.
	unique_ptr<DecideGroupBound> limit_bound;

	ExprPtr Column(const LogicalType &type, ColumnBinding binding) const {
		return calc.Col(type, binding);
	}

	//! Whether a group can fit no prefix, the empty one included. A constant limit of at least 0 always fits the empty
	//! prefix.
	bool NeedsVerdict() const {
		return !condition.limit.constant || condition.limit.number < 0;
	}

	//! Makes `window` run over the rows of the row's group. A row outside every group has a NULL where the key is
	//! compared with `=`, so it is apart from every group.
	void PartitionByGroup(const DirectCarried &carried, BoundWindowExpression &window) const {
		for (idx_t i = 0; i < carried.keys.size(); i++) {
			window.partitions.push_back(calc.Out(Column(carried.key.types[i], carried.keys[i])));
		}
	}

	//! P1: the weight of each row, `sum(sign * coefficient)` of the budget's terms, checked as the solver checks a
	//! coefficient (a NULL term reads 0: the front end guards it), and what the data limit reads.
	Weighted LayoutWeights(DirectProjectionStage &p1, const DirectCarried &from_scope, idx_t state_index) const {
		Weighted result;
		result.carried = p1.Forward(from_scope, source_types, scoped);
		auto over_state = [&](const Expression &value) {
			return state_index == DConstants::INVALID_INDEX ? value.Copy()
			                                                : DirectOverProjection(value, source_bindings, state_index);
		};
		auto &checks = result.checks;
		if (limit_bound) {
			auto eligible = scoped ? Column(LogicalType::BOOLEAN, from_scope.eligible) : nullptr;
			for (auto &input : DirectGroupInputs(calc, *limit_bound, eligible, over_state)) {
				DirectGroupInputColumns columns;
				if (input.reads) {
					columns.reads = p1.Add(calc.Out(input.reads));
				}
				if (input.argument) {
					columns.argument = p1.Add(calc.Out(input.argument));
				}
				if (input.invalid) {
					auto message = Column(LogicalType::VARCHAR, p1.Add(calc.Out(input.invalid)));
					checks.push_back(DirectRaiseMessage(optimizer, calc, message));
				}
				result.limit_inputs.push_back(columns);
			}
		}
		auto values = DirectValuesOn(condition.budget, 0);
		// A row the `when` drops reads no weight: it is NULL there, so the check passes it and no total counts it.
		ExprPtr kept;
		if (condition.when) {
			kept = calc.Wrap(DirectActiveWhen(calc, over_state(*condition.when).get()));
			if (condition.objective_on_kept) {
				result.kept = p1.Add(kept);
			}
		}
		result.weight = p1.Add(DirectOnKeptRows(calc, kept, calc.Wrap(DirectSumOf(optimizer, values, over_state))));
		if (auto check = DirectCheckNumber(optimizer, result.weight, condition.budget.names[0], values, source_bindings,
		                                   p1.index)) {
			checks.push_back(std::move(check));
		}
		return result;
	}

	//! W1: adds to `window` the running sum of the weights and the rank, per group in order of the weight, and the
	//! group's limit. The rank is built by hand: the builder (scalar expressions) has no operation for a window.
	Sorted AddPrefixWindows(LogicalWindow &window, const Weighted &weighted) const {
		auto &carried = weighted.carried;
		auto weight = Column(LogicalType::DOUBLE, weighted.weight);
		vector<ExprPtr> partitions;
		for (idx_t i = 0; i < carried.keys.size(); i++) {
			partitions.push_back(Column(carried.key.types[i], carried.keys[i]));
		}
		Sorted result;
		window.expressions.push_back(
		    DirectRunningSum(optimizer, calc, weight, weight, OrderType::ASCENDING, partitions));
		result.total = ColumnBinding(window.window_index, 0);
		auto rank =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
		rank->start = WindowBoundary::UNBOUNDED_PRECEDING;
		rank->end = WindowBoundary::UNBOUNDED_FOLLOWING;
		PartitionByGroup(carried, *rank);
		rank->orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, calc.Out(weight));
		window.expressions.push_back(std::move(rank));
		result.rank = ColumnBinding(window.window_index, 1);
		if (limit_bound) {
			DirectWindowSink sink(window,
			                      [&](BoundWindowExpression &aggregate) { PartitionByGroup(carried, aggregate); });
			result.limit = DirectAddGroupBound(calc, *limit_bound, weighted.limit_inputs, sink,
			                                   scoped ? Column(LogicalType::BOOLEAN, carried.eligible) : nullptr);
		}
		return result;
	}

	//! The group's limit B as a DOUBLE: NULL when the group has no limit (a NULL data limit skips its instance).
	ExprPtr LimitOf(const Sorted &sorted) const {
		return condition.limit.constant ? calc.Num(condition.limit.number)
		                                : calc.Cast(sorted.limit.upper, LogicalType::DOUBLE);
	}

	//! Whether `total <= B`; true when the group has no limit.
	ExprPtr Fits(const Sorted &sorted, const ExprPtr &total) const {
		auto limit = LimitOf(sorted);
		auto fits = calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, total, limit);
		return condition.limit.constant ? fits : calc.If(calc.IsNull(limit), calc.Bool(true), fits);
	}

	//! P2: the rank of the row when its prefix total fits the group's limit, whether the empty prefix does, and the
	//! checks: the weight and what the limit reads (P1), the prefix total (a DOUBLE overflow), then those of the limit
	//! (a NaN limit, an `=` limit that takes two values in a group).
	Fitted ComputeFits(DirectProjectionStage &p2, Weighted &weighted, const Sorted &sorted) const {
		Fitted result;
		result.carried = p2.Forward(weighted.carried, source_types, scoped);
		auto rank = Column(LogicalType::BIGINT, sorted.rank);
		result.rank = p2.Add(calc.Out(rank));
		auto fits = Fits(sorted, Column(LogicalType::DOUBLE, sorted.total));
		result.fit_rank = p2.Add(calc.Out(calc.If(fits, rank, calc.Constant(Value(LogicalType::BIGINT)))));
		if (condition.objective_on_kept) {
			result.kept = p2.Add(Column(LogicalType::BOOLEAN, weighted.kept));
		}
		if (NeedsVerdict()) {
			result.empty_fits = p2.Add(calc.Out(Fits(sorted, calc.Num(0.0))));
		}
		auto checks = std::move(weighted.checks);
		// The weights are numbers by now (the checks above). A prefix total past a DOUBLE, in either direction, is a
		// number the plan computes, and it names the budget as the solver path names a number it computes.
		checks.push_back(DirectRaiseTooLarge(optimizer, Column(LogicalType::DOUBLE, sorted.total),
		                                     condition.budget.names[0].clause));
		for (auto &message : sorted.limit.messages) {
			checks.push_back(DirectRaiseMessage(optimizer, calc, message));
		}
		result.valid = p2.Add(DirectAllHold(calc, std::move(checks)));
		return result;
	}

	//! Whether the row's group fits a prefix: some rank fits, or the empty prefix does. A row outside every group has
	//! no budget.
	ExprPtr GroupHasPrefix(const Fitted &fitted, const ExprPtr &k) const {
		auto has_prefix = calc.Not(calc.IsNull(k));
		if (!condition.limit.constant) {
			// A constant limit that needs a verdict is below 0, which no empty prefix fits.
			has_prefix = calc.Any({has_prefix, Column(LogicalType::BOOLEAN, fitted.empty_fits)});
		}
		return scoped ? calc.If(Column(LogicalType::BOOLEAN, fitted.carried.eligible), has_prefix, calc.Bool(true))
		              : has_prefix;
	}

	//! P4: the source columns, x, and the column of the checks.
	DirectSubstitute Answer(unique_ptr<LogicalOperator> plan, const Fitted &fitted, const ExprPtr &k,
	                        const ExprPtr &feasible) const {
		DirectProjectionStage out(binder.GenerateTableIndex());
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			out.Add(calc.Out(Column(source_types[i], fitted.carried.source[i])));
		}
		// When no rank fits, k is NULL and the empty prefix, rank 0, is the one selected.
		auto within = calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, Column(LogicalType::BIGINT, fitted.rank),
		                       calc.Coalesce({k, calc.Constant(Value::BIGINT(0))}));
		auto chosen =
		    scoped ? calc.If(Column(LogicalType::BOOLEAN, fitted.carried.eligible), within, calc.Bool(true)) : within;
		if (condition.objective_on_kept) {
			// A row the budget's `when` drops adds nothing to the objective and costs nothing: it is not selected.
			chosen = calc.If(Column(LogicalType::BOOLEAN, fitted.kept), chosen, calc.Bool(false));
		}
		out.Add(calc.Out(calc.If(chosen, calc.Constant(Value::INTEGER(1)), calc.Constant(Value::INTEGER(0)))));
		vector<unique_ptr<Expression>> checks;
		checks.push_back(calc.Out(Column(LogicalType::BOOLEAN, fitted.valid)));
		if (feasible) {
			checks.push_back(
			    DirectRaiseUnless(calc, calc.Out(feasible), DirectInfeasibleErrorPredicate(optimizer, errors)));
		}
		out.Add(DirectAllHold(calc, std::move(checks)));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1);
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		substitute.child = out.Finish(std::move(plan));
		return substitute;
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	return DirectBuildOverOneRead(
	    reader, [&](unique_ptr<LogicalOperator> rows) { return plan_builder.Build(std::move(rows)); });
}

} // namespace direct_s3_max_cardinality_knapsack

unique_ptr<DirectSolveRule> MakeS3MaxCardinalityKnapsackRule() {
	return make_uniq<direct_s3_max_cardinality_knapsack::Rule>();
}

} // namespace duckdb
