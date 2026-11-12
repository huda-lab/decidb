#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <algorithm>
#include <cmath>

#include "duckdb/common/exception.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_ranked_prefix.hpp"
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
//! it selects, or under `SUM(w*x) >= B`, minimizing how many.
struct Condition final : DirectApplicabilityCondition {
	//! The budget's weight terms, all on the one decision; its weight on a row is `sum(sign * coefficient)`.
	DirectLinearPart budget;
	//! The budget's limit B: a constant, or a value of the data read per group (DirectReadLimit).
	DirectLimit limit;
	//! The budget is a minimum, `SUM(w*x) >= B`, and the objective minimizes the count: the fewest items that reach B.
	//! Otherwise the budget is a maximum, `<= B`, and the objective maximizes the count. The plan is the same with the
	//! sort reversed, the comparison turned and the shortest prefix taken for the longest.
	bool at_least = false;
	//! The rows and groups the budget covers (DirectSharedScope): its `when` (the clause's, with the sum's own), over
	//! the source, null for every row, a row it drops reading no weight and being in no budget; and its `by` key, each
	//! group a budget of its own, empty for one budget for all the rows. A `per K:` class is declined, so `classes` and
	//! `gated` are empty.
	DirectSharedScope scope;
	//! Whether the objective counts the budget's rows only: its `when` is the budget's, so a row the budget drops adds
	//! nothing to the objective either. Otherwise every row adds one, and a row outside the budget is selected.
	bool objective_on_kept = false;
	//! What the per-row clauses fix: a row fixed to 1 is selected and its weight counts toward its group's budget; a
	//! row fixed to 0 is not selected and leaves the prefix; a row fixed to both is infeasible.
	DirectFixedValues fixed;
};

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}

	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		auto condition = make_uniq<Condition>();
		const DirectIRConstraint *clause = nullptr;
		if (!DirectIsOneRowDecision(ir, DirectDomain::BOOL) || !ProveConstraints(ir, context, *condition, clause) ||
		    !ProveBudget(ir, context, *clause, *condition) || !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		return std::move(condition);
	}

	string Summary() const override {
		return "max c*SUM(x), SUM(w*x) <= B; min c*SUM(x), SUM(w*x) >= B";
	}

	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveConstraints(const DirectIR &ir, ClientContext &context, Condition &condition,
	                      const DirectIRConstraint *&clause) const;
	bool ProveBudget(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &clause,
	                 Condition &condition) const;
	bool ProveScope(const DirectIR &ir, const DirectIRConstraint &clause, Condition &condition) const;
	bool ProveWeights(ClientContext &context, const vector<DecideSplitTerm> &terms) const;
	bool ProveObjectivePart(const DirectIR &ir, ClientContext &context, const DirectPart &part,
	                        unique_ptr<Expression> &kept, double &count) const;
	bool ProveObjectiveRows(const Condition &condition, const unique_ptr<Expression> &kept, bool first,
	                        bool &filtered) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! The constraints are one budget, compared with `<=` or `>=`, and the per-row clauses on the decision, which fix items
//! (DirectReadFixedValues reads them as S1 does); a membership constraint, a second budget and no budget are declines.
//! That the budget is an aggregate is proved with its scope (ProveScope).
bool Rule::ProveConstraints(const DirectIR &ir, ClientContext &context, Condition &condition,
                            const DirectIRConstraint *&clause) const {
	vector<const DirectIRConstraint *> fixes, budgets;
	if (!DirectSplitFixes(ir, fixes, budgets) || budgets.size() != 1) {
		return false;
	}
	clause = budgets[0];
	condition.at_least = clause->comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	return (condition.at_least || clause->comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO) &&
	       DirectReadFixedValues(ir, context, fixes, condition.fixed);
}

//! The budget covers the rows its `when` (on the clause or in the sum) keeps, or every row, in the groups of its `by`
//! key. A `per K:` class gives it a smaller scope, which stays with the solver.
bool Rule::ProveScope(const DirectIR &ir, const DirectIRConstraint &clause, Condition &condition) const {
	vector<const DirectIRConstraint *> budgets {&clause};
	// A gated scope has classes, so the classes decline it too.
	return DirectProveSharedScope(ir, budgets, condition.scope) && condition.scope.classes.empty();
}

//! Each weight is a finite constant or a number of the source row (DirectReadLinearPart has proved the latter, which
//! may raise when evaluated; the plan evaluates it on every row, as the solver does). A body of constants only must
//! have them all finite, and must not count the rows: constants that add up to exactly 1 or -1 are S1's
//! (DirectReadCountBody).
bool Rule::ProveWeights(ClientContext &context, const vector<DecideSplitTerm> &terms) const {
	bool constants_only = std::all_of(terms.begin(), terms.end(),
	                                  [](const DecideSplitTerm &term) { return term.coefficient->IsFoldable(); });
	double slope;
	bool negated;
	return (!constants_only || DirectConstantSlope(context, terms, slope)) &&
	       !DirectReadCountBody(context, terms, negated);
}

//! `SUM(w*x) <= B` or `>= B`: a plain SUM of linear terms on the decision, in the scope ProveScope proves, with weights
//! ProveWeights proves and a limit the shared reader reads.
bool Rule::ProveBudget(const DirectIR &ir, ClientContext &context, const DirectIRConstraint &clause,
                       Condition &condition) const {
	if (!ProveScope(ir, clause, condition)) {
		return false;
	}
	// ProveScope has proved that the clause is a plain SUM.
	auto sum = clause.PlainSum();
	return DirectOnlyLinearTerms(sum->terms) &&
	       DirectReadLinearPart(ir, context, *sum, clause.written, condition.budget) &&
	       ProveWeights(context, sum->terms) && DirectReadLimit(ir, context, clause, condition.limit);
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
	double slope;
	if (!DirectConstantSlope(context, part.terms, slope)) {
		return false;
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
	if (part_filtered && (!condition.scope.when || !Expression::Equals(*kept, *condition.scope.when))) {
		return false;
	}
	if (first) {
		filtered = part_filtered;
	}
	return part_filtered == filtered;
}

//! The objective counts the selected rows in the direction the budget pairs with: its parts' coefficients add up to a
//! finite number, below zero for a maximum budget (the parts are a minimization, so a count that is maximized is
//! negative) and above zero for a minimum one, on all the rows or on the budget's. Any other objective, none included,
//! and the other two pairings (a trivial answer: select nothing, or everything) are a decline.
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
	return std::isfinite(total) && (condition.at_least ? total > 0 : total < 0);
}

//! The plan, over one read of the source, all in windows (no hash aggregate, no join back). The shared ranked-prefix
//! plan (DirectRankedPrefix) fixes its shape; S3 gives the ranking, the prefix test and the answer:
//!
//!   [W0]  the source read
//!   S     the scope projection (DirectProjectScope): the eligible flag and the group key, for a `by` budget or a
//!         `when`; and whether a clause fixes the row to 1 or to 0
//!   P1    the rows' weights `w` and what the data limit reads; a fixed row has no weight in the prefix; the checks of
//!         the fixed values and of a row fixed to both values (the builder's)
//!   W1    per group, the free rows in order of `w`: their rank and running sum of `w` (the prefix total); the total
//!         of the weights of the rows fixed to 1; and the group's limit B (DirectWindowSink)
//!   P2    whether the row's prefix fits (`fixed total + total <= B`), the rank when it does, and the checks: the
//!         weight, what the limit reads and what it gives, and the prefix total
//!   W2    per group, k, the largest rank that fits (NULL when none does); for a minimum, the smallest rank that
//!         reaches B, with the order of W1 reversed
//!   H     the rows held in a materialized CTE, and whether every group fits a prefix, raised in the hold; only when a
//!         group can fit none (NeedsVerdict)
//!   P4    the source columns, x = 1 when the rank is at most k, and the column that holds the checks
//!
//! The solver reads every row before it answers, so a bad value on the last row raises even under an outer LIMIT 1.
//! W2 holds every row before it gives one, and the checks of P2 stand below it, so every row has passed them when the
//! first leaves. The hold does the same for infeasibility: it raises a group that fits no prefix while the rows are
//! held, not when a row of that group is reached.
//!
//! The slot map: the source columns in order (the reader's: the source row id and the bounds' values come after the
//! node's, and the driver drops them), then x (INTEGER 0/1), then the column of the checks, which
//! keeps them live (`validated_columns`). A row outside every budget (a NULL in a NO_GROUP element of the `by` key,
//! D5, or dropped by the budget's `when`) is in no budget and selecting it costs nothing, so x = 1 there, unless the
//! objective drops it too (its `when` is the budget's): then it adds nothing and x = 0.
//!
//! Rows of equal weight are ordered as the sort gives them, and the answer is right only if the rank and the running
//! sum see the same order: with two sorts, tied rows a and b of weight 5 under B = 7 could be ranked a, b by one and
//! summed b, a by the other, and both would be selected. W1's rank and running sum have one partition and one order,
//! so DuckDB sorts them once, and the rank of a row is the position its prefix total was summed at.
class PlanBuilder final : public DirectRankedPrefix {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : DirectRankedPrefix(optimizer_p, source_bindings_p, source_types_p, errors_p, {condition_p.scope},
	                         condition_p.fixed),
	      condition(condition_p) {
		if (!condition.limit.constant) {
			limit_bound = condition.limit.data.GroupBound(optimizer);
			limit_index = AddGroupBound(0, *limit_bound);
		}
	}

private:
	const Condition &condition;
	//! The group bound of the data limit; null for a constant limit.
	unique_ptr<DecideGroupBound> limit_bound;
	idx_t limit_index = DConstants::INVALID_INDEX;
	//! P1: the weight of the row, and what the weight of a row fixed to 1 and the objective's rows add.
	ColumnBinding weight;
	//! The weight of a row fixed to 1, NULL elsewhere; set only with fixed rows.
	ColumnBinding fixed_weight;
	//! Whether the budget's `when` keeps the row, in P1 and in P2; set only when the objective counts the budget's rows
	//! only.
	ColumnBinding kept;
	ColumnBinding kept_after;
	//! W1: the prefix total, and the total of the weights of the group's rows fixed to 1 (NULL for none); the second is
	//! set only with fixed rows.
	ColumnBinding total;
	ColumnBinding fixed_total;
	//! P2: the rank when the row's prefix fits the limit, else NULL; and whether the empty prefix fits the limit, set
	//! only when a verdict or a minimum needs it.
	ColumnBinding fit_rank;
	ColumnBinding empty_fits;

	//! Whether the plan must tell a row the budget's `when` drops from the objective's: a maximum selects such a row
	//! unless the objective drops it too. A minimum never selects a row outside the budget, so the objective's `when`
	//! changes nothing.
	bool DropsRowsFromObjective() const {
		return condition.objective_on_kept && !condition.at_least;
	}

	//! Whether a group can fit no prefix, the empty one included. Without fixed rows, a constant limit of at least 0
	//! (at most 0 for a minimum) always fits the empty prefix.
	bool NeedsVerdict() const {
		if (HasFixes() || !condition.limit.constant) {
			return true;
		}
		return condition.at_least ? condition.limit.number > 0 : condition.limit.number < 0;
	}

	//! Whether P2 holds whether the empty prefix fits: the verdict reads it, and a minimum selects nothing when it
	//! does.
	bool NeedsEmptyFit() const {
		return NeedsVerdict() || condition.at_least;
	}

	//! The smallest weights have the smallest total of any k rows, the largest the largest.
	OrderType RankOrder() const override {
		return condition.at_least ? OrderType::DESCENDING : OrderType::ASCENDING;
	}

	//! P1: the weight of each row, `sum(sign * coefficient)` of the budget's terms, checked as the solver checks a
	//! coefficient (a NULL term reads 0: the front end guards it), and the weight of a row fixed to 1. A fixed row
	//! leaves the prefix: the builder ranks it last and the sum skips it.
	DirectRowLayout LayoutRows(DirectProjectionStage &p1, const DirectScopeState &state) override {
		DirectRowLayout layout;
		auto over_state = [&](const Expression &value) {
			return OverState(state, value);
		};
		auto values = DirectValuesOn(condition.budget, 0);
		// A row the `when` drops reads no weight: it is NULL there, so the check passes it and no total counts it.
		ExprPtr kept_when;
		auto &when = levels[0].scope.when;
		if (when) {
			kept_when = calc.Wrap(DirectActiveWhen(calc, over_state(*when).get()));
			if (DropsRowsFromObjective()) {
				kept = p1.Add(kept_when);
			}
		}
		auto weight_value = DirectOnKeptRows(calc, kept_when, calc.Wrap(DirectSumOf(optimizer, values, over_state)));
		weight = p1.Add(weight_value);
		if (HasFixes()) {
			fixed_weight = p1.Add(calc.If(FixedOne(state), weight_value, calc.Constant(Value(LogicalType::DOUBLE))));
		}
		if (auto check =
		        DirectCheckNumber(optimizer, weight, condition.budget.names[0], values, source_bindings, p1.index)) {
			layout.checks_above.push_back(std::move(check));
		}
		layout.rank_key = weight;
		return layout;
	}

	//! W1: adds to `w1` the running sum of the weights, per group in order of the weight, and the total of the weights
	//! of the rows fixed to 1.
	void AddGroupWindows(idx_t level, LogicalWindow &w1, const DirectRankedColumns &rows) override {
		auto key = RankKey();
		auto sum = DirectRunningSum(optimizer, calc, key, key, RankOrder(), {});
		PartitionByGroup(rows.levels[level], *sum);
		w1.expressions.push_back(std::move(sum));
		total = ColumnBinding(w1.window_index, w1.expressions.size() - 1);
		if (HasFixes()) {
			auto sum = DirectWindowExtremum(optimizer, "sum", calc.Out(Column(LogicalType::DOUBLE, fixed_weight)));
			OverRank(rows.levels[level], *sum);
			w1.expressions.push_back(std::move(sum));
			fixed_total = ColumnBinding(w1.window_index, w1.expressions.size() - 1);
		}
	}

	//! The group's limit B as a DOUBLE: NULL when the group has no limit (a NULL data limit skips its instance). The
	//! group's value is the tightest of its rows': the MIN of a maximum, the MAX of a minimum.
	ExprPtr LimitOf() const {
		if (condition.limit.constant) {
			return calc.Num(condition.limit.number);
		}
		auto &limit = GroupValues(0, limit_index);
		return calc.Cast(condition.at_least ? limit.lower : limit.upper, LogicalType::DOUBLE);
	}

	//! Whether `total <= B` (`total >= B` for a minimum); true when the group has no limit.
	ExprPtr Fits(const ExprPtr &spent) const {
		auto limit = LimitOf();
		auto side = condition.at_least ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
		                               : ExpressionType::COMPARE_LESSTHANOREQUALTO;
		auto fits = calc.Cmp(side, spent, limit);
		return condition.limit.constant ? fits : calc.If(calc.IsNull(limit), calc.Bool(true), fits);
	}

	//! P2: the checks of the rows fixed to 1: their total and its sum with the prefix total are numbers the plan
	//! computes (a DOUBLE overflow).
	void AddFixedChecks(const ExprPtr &spent, vector<unique_ptr<Expression>> &checks) {
		if (!HasFixes()) {
			return;
		}
		checks.push_back(
		    DirectRaiseTooLarge(optimizer, Column(LogicalType::DOUBLE, fixed_total), condition.budget.names[0].clause));
		checks.push_back(DirectRaiseTooLarge(optimizer, spent, condition.budget.names[0].clause));
	}

	//! P2: the rank of the row when its prefix total fits the group's limit, whether the empty prefix does, and the
	//! checks of the prefix total (a DOUBLE overflow); the limit's own are the builder's (a NaN limit, an `=` limit
	//! that takes two values in a group).
	vector<unique_ptr<Expression>> LayoutPrefix(DirectProjectionStage &p2, const DirectRankedColumns &input) override {
		auto rank = Column(LogicalType::BIGINT, input.levels[0].rank);
		// The prefix and the rows fixed to 1 share the budget: `fixed total + prefix total <= B`, in a DOUBLE.
		auto prefix_total = Column(LogicalType::DOUBLE, total);
		ExprPtr fixed_sum = calc.Num(0.0);
		ExprPtr spent = prefix_total;
		if (HasFixes()) {
			fixed_sum = calc.Coalesce({Column(LogicalType::DOUBLE, fixed_total), fixed_sum});
			spent = calc.Add(prefix_total, fixed_sum);
		}
		// A fixed row has no weight in the prefix, so it sorts after the free rows and its running total repeats the
		// full prefix's. It fits only when the full prefix does, and then every free rank is chosen already: it needs
		// no mark of its own.
		auto fits = Fits(spent);
		fit_rank = p2.Add(calc.Out(calc.If(fits, rank, calc.Constant(Value(LogicalType::BIGINT)))));
		if (DropsRowsFromObjective()) {
			kept_after = p2.Add(Column(LogicalType::BOOLEAN, kept));
		}
		if (NeedsEmptyFit()) {
			empty_fits = p2.Add(calc.Out(Fits(fixed_sum)));
		}
		// The weights are numbers by now (the checks above). A prefix total past a DOUBLE, in either direction, is a
		// number the plan computes, and it names the budget as the solver path names a number it computes.
		vector<unique_ptr<Expression>> checks;
		checks.push_back(DirectRaiseTooLarge(optimizer, prefix_total, condition.budget.names[0].clause));
		AddFixedChecks(spent, checks);
		return checks;
	}

	//! The largest rank that fits, not the first that fails: with negative weights the prefix totals fall first. For a
	//! minimum, the smallest rank that reaches B, not the first that falls short of it.
	unique_ptr<BoundWindowExpression> PrefixSize(const DirectRankedColumns &rows) override {
		auto size = DirectWindowExtremum(optimizer, condition.at_least ? "min" : "max",
		                                 calc.Out(Column(LogicalType::BIGINT, fit_rank)));
		PartitionByGroup(rows.levels[0], *size);
		return size;
	}

	//! Whether the row's group fits a prefix: some rank fits, or the empty prefix does (with the rows fixed to 1). A
	//! row outside every group has no budget (the builder). Null where no group can fit none.
	ExprPtr GroupVerdict(idx_t level, const DirectRankedColumns &rows) override {
		if (!NeedsVerdict()) {
			return nullptr;
		}
		return calc.Any({calc.Not(calc.IsNull(rows.size)), Column(LogicalType::BOOLEAN, empty_fits)});
	}

	//! Whether the row is selected: it is in the prefix of the group's size.
	ExprPtr Chosen(const DirectRankedColumns &rows) override {
		// When no rank fits, k is NULL and the empty prefix, rank 0, is the one selected. For a minimum the empty
		// prefix is the shortest one, so it is taken whenever it reaches B.
		auto zero = calc.Constant(Value::BIGINT(0));
		auto prefix_size = calc.Coalesce({rows.size, zero});
		if (condition.at_least) {
			prefix_size = calc.If(Column(LogicalType::BOOLEAN, empty_fits), zero, prefix_size);
		}
		auto chosen = calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                       Column(LogicalType::BIGINT, rows.levels[0].rank), prefix_size);
		if (levels[0].scoped) {
			// A row outside every budget costs nothing to select: a maximum selects it, a minimum has no use for it.
			chosen =
			    calc.If(Column(LogicalType::BOOLEAN, rows.levels[0].eligible), chosen, calc.Bool(!condition.at_least));
		}
		if (DropsRowsFromObjective()) {
			// A row the budget's `when` drops adds nothing to the objective and costs nothing: it is not selected.
			chosen = calc.If(Column(LogicalType::BOOLEAN, kept_after), chosen, calc.Bool(false));
		}
		return chosen;
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
