#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <cmath>

#include "duckdb/common/exception.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

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
		if (!ProveBudget(ir, context, *condition) || !ProveObjective(ir, context)) {
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
	bool ProveObjectivePart(const DirectIR &ir, ClientContext &context, const DirectPart &part, double &count) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context) const;
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

//! The budget covers every row, or every row of a group of its `by` key. A `when` (on the clause or in the sum) or a
//! `per K:` class gives it a smaller scope, which stays with the solver.
bool Rule::ProveScope(const DirectIR &ir, Condition &condition) const {
	vector<const DirectIRConstraint *> budgets {&ir.constraints[0]};
	DirectSharedScope scope;
	// A gated scope has classes, so the classes decline it too.
	if (!DirectProveSharedScope(ir, budgets, scope) || scope.when || !scope.classes.empty()) {
		return false;
	}
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

//! One objective part: a plain SUM of linear terms on every row, each coefficient a finite constant. `count` is the
//! part's coefficient on the decision in the objective's own (minimizing) direction.
bool Rule::ProveObjectivePart(const DirectIR &ir, ClientContext &context, const DirectPart &part, double &count) const {
	if (!part.IsPlainSum() || !DirectOnlyLinearTerms(part.terms)) {
		return false;
	}
	DirectLinearPart read;
	unique_ptr<Expression> kept;
	if (!DirectReadLinearPart(ir, context, part, ir.objective.written, read) ||
	    !DirectObjectivePartKept(ir, part, kept) || kept) {
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

//! The objective counts the selected rows: its parts' coefficients add up to a number below zero (the parts are a
//! minimization, so a count that is maximized is negative) that is finite. Any other objective, none included, is a
//! decline.
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context) const {
	double total = 0;
	for (auto &part : ir.objective.parts) {
		double count;
		if (!ProveObjectivePart(ir, context, part, count)) {
			return false;
		}
		total += count;
	}
	return std::isfinite(total) && total < 0;
}

DirectSubstitute Rule::Apply(DirectSourceReader &, Optimizer &, const DirectApplicabilityCondition &,
                             const DirectStateErrors &) const {
	throw InternalException("DECIDE: the S3 plan is not built");
}

} // namespace direct_s3_max_cardinality_knapsack

unique_ptr<DirectSolveRule> MakeS3MaxCardinalityKnapsackRule() {
	return make_uniq<direct_s3_max_cardinality_knapsack::Rule>();
}

} // namespace duckdb
