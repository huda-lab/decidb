#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/decide/decide_singleton_row_folding.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_s1_uniform_matroid {

constexpr const char *RULE_NAME = "S1_UNIFORM_MATROID";

struct Condition final : DirectApplicabilityCondition {
	struct SourceBound {
		ExpressionType comparison;
		//! One value per group on every row the clause keeps: the bound-side window rewrite reduced the bound to its
		//! group's tightest value and checked it (DecideOptimizer::RewriteRhsAggregates).
		unique_ptr<Expression> value;
		//! The count body is `-x`, so `comparison` is already flipped and the value to compare against is `-B`.
		bool negated = false;
	};
	//! A fixed value that comes from the row: it must be neither NULL nor, for a floating point value, NaN.
	struct SourceFixedValue {
		unique_ptr<Expression> value;
		bool reject_nan;
		//! The clause's `when`; the value is read only on the rows it keeps. Null for every row.
		unique_ptr<Expression> when;
	};
	struct ObjectivePart {
		int sign;
		unique_ptr<Expression> coefficient;
		//! A constant factor the part's objective term is multiplied by, or divided by when `divides`.
		bool scaled = false;
		double scale = 1.0;
		bool divides = false;
	};

	//! The constraints of the IR, which outlives the condition: the count bounds, and the fixed Boolean values.
	vector<const DirectIRConstraint *> bound_factors;
	vector<const DirectIRConstraint *> local_fixes;

	idx_t decide_index;
	idx_t lower = 0;
	idx_t upper = 0;
	bool has_upper = false;
	bool impossible = false;
	DecideSense sense;
	vector<ObjectivePart> objective_parts;
	vector<idx_t> group_key_slots;
	unique_ptr<Expression> when_condition;
	vector<unique_ptr<Expression>> fixed_one_conditions;
	vector<unique_ptr<Expression>> fixed_zero_conditions;
	vector<SourceBound> source_bounds;
	vector<SourceFixedValue> source_fixed_values;
	bool scoped = false;
};

//! Every term is the decision times a coefficient.
bool OnlyDecisionTerms(const vector<DecideSplitTerm> &terms) {
	for (auto &term : terms) {
		if (term.kind != DecideTermKind::LINEAR || term.var_a != 0) {
			return false;
		}
	}
	return !terms.empty();
}

//! The terms add up to exactly one `x`: `x`, `1*x`, `x*1.0`, `(1+0)*x`. Their signed constant coefficients are
//! summed as the solver's like-term collection sums them; a constant term (`x + 0`) is not a unit contribution.
bool IsUnitContribution(ClientContext &context, const vector<DecideSplitTerm> &terms) {
	double total = 0;
	for (auto &term : terms) {
		double value;
		if (!term.coefficient ||
		    !TryEvaluateFoldableDouble(context, *term.coefficient, value, DIRECT_FINITE_CONSTANT)) {
			return false;
		}
		total += term.sign * value;
	}
	return total == 1.0;
}

//! What the sum counts: `direction * (number of selected rows)`.
struct CountBody {
	//! +1 for `x`; -1 for `-x`.
	int direction = 1;
};

//! Reads a reducer body as `a*x`. The signed constant coefficients of `x` must add up to exactly +1 or -1, as the
//! solver's like-term collection sums them. Any other term, a constant included, is a decline: a constant term in the
//! body shifts the limit by an amount that depends on the group's row count, and the solver answers that.
bool ReadCountBody(ClientContext &context, const vector<DecideSplitTerm> &terms, CountBody &body) {
	double slope = 0;
	for (auto &term : terms) {
		double value;
		if (term.kind != DecideTermKind::LINEAR || term.var_a != 0 || !term.coefficient ||
		    !TryEvaluateFoldableDouble(context, *term.coefficient, value, DIRECT_FINITE_CONSTANT)) {
			return false;
		}
		slope += term.sign * value;
	}
	if (slope != 1.0 && slope != -1.0) {
		return false;
	}
	body.direction = slope > 0 ? 1 : -1;
	return true;
}

//! The comparison that holds between `-a` and `-b` when `comparison` holds between `a` and `b`.
ExpressionType FlipComparison(ExpressionType comparison) {
	switch (comparison) {
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	case ExpressionType::COMPARE_LESSTHAN:
		return ExpressionType::COMPARE_GREATERTHAN;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ExpressionType::COMPARE_LESSTHANOREQUALTO;
	case ExpressionType::COMPARE_GREATERTHAN:
		return ExpressionType::COMPARE_LESSTHAN;
	default:
		return comparison;
	}
}

//! The decision's terms on a per-row left side, as in a fixed value `x = 1`; null otherwise. CheckCondition has
//! declined a membership constraint before it asks, and checks that the terms add up to exactly one `x`.
const vector<DecideSplitTerm> *FixedTerms(const DirectIRConstraint &ir_node) {
	if (ir_node.aggregate || ir_node.lhs.size() != 1) {
		return nullptr;
	}
	auto &part = ir_node.lhs[0];
	if (part.sign != 1 || part.reducer != DirectReducer::NONE || part.scale || !OnlyDecisionTerms(part.terms)) {
		return nullptr;
	}
	return &part.terms;
}

//! Turns `SUM(x) <comparison> expr` (`SUM(x) <comparison> -expr` when `negate`) into an inclusive count limit in
//! [0, 2^53]. A bound below zero on an upper side is infeasible; one at or below zero on a lower side is no bound.
//! False when `expr` is not an exactly representable finite constant.
bool NormalizeCapacity(ClientContext &context, const Expression &expr, ExpressionType comparison, idx_t &capacity,
                       bool &impossible, bool negate = false) {
	double as_double;
	if (!TryEvaluateFoldableDouble(context, expr, as_double, DIRECT_FINITE_CONSTANT)) {
		return false;
	}
	if (negate) {
		as_double = -as_double;
	}
	double inclusive;
	switch (comparison) {
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		inclusive = std::ceil(as_double);
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		if (as_double >= DECIDE_MAX_EXACT_INTEGER) {
			return false;
		}
		inclusive = DecideStrictBound(true, as_double);
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		inclusive = std::floor(as_double);
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		inclusive = DecideStrictBound(false, as_double);
		break;
	default:
		return false;
	}
	if (DirectIsLowerBound(comparison)) {
		if (inclusive <= 0.0) {
			capacity = 0;
			return true;
		}
	} else if (inclusive < 0.0) {
		impossible = true;
		capacity = 0;
		return true;
	}
	if (!DecideIsWithinExactRange(inclusive)) {
		return false;
	}
	capacity = static_cast<idx_t>(inclusive);
	return true;
}

//! Which of the values 0 and 1 a per-row bound on `x` leaves allowed. Neither allowed means no row it covers can
//! satisfy it.
struct FixedDomain {
	bool zero = true;
	bool one = true;
};

bool SatisfiesComparison(ExpressionType comparison, double value, double bound) {
	switch (comparison) {
	case ExpressionType::COMPARE_EQUAL:
		return value == bound;
	case ExpressionType::COMPARE_NOTEQUAL:
		return value != bound;
	case ExpressionType::COMPARE_LESSTHAN:
		return value < bound;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return value <= bound;
	case ExpressionType::COMPARE_GREATERTHAN:
		return value > bound;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return value >= bound;
	default:
		throw InternalException("S1 direct solve received a fixed-value comparison it did not match");
	}
}

//! Reads `x <comparison> bound` as the values of {0, 1} it allows.
//!
//! The solver reads a bound with a `when`, or one it does not fold, as a per-row constraint and applies plain
//! arithmetic to it. A bound it folds (`variable_bound`) is a bound on the variable itself. The solver seeds the box of
//! a Boolean at [0, 1] and a written bound only narrows it, so a bound that always holds (`x >= -3`, `x > -3`) changes
//! nothing and reads here as both values allowed, which is what comparing 0 and 1 against it gives. A bound that cannot
//! hold makes the solver raise an error that quotes it (`x >= 2 cannot hold for a BOOL variable.`) before it reads a
//! row; that early error is not S1's to reproduce, so false for those. A strict bound is shifted to a whole number
//! first, with the solver's own function, so `x > 0.5` is the lower bound 1 and `x < 1.5` the upper bound 1 here as
//! there; for a variable that is 0 or 1 that is the same as the comparison itself. Whether the box contradicts the
//! built-in domain of a Boolean is the solver's own test (DecideBoxContradictsDomain).
bool ConstantFixedDomain(const DirectIRConstraint &ir_node, double bound, FixedDomain &domain) {
	auto comparison = ir_node.comparison;
	if (ir_node.variable_bound) {
		bool has_lower = comparison == ExpressionType::COMPARE_GREATERTHAN ||
		                 comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
		                 comparison == ExpressionType::COMPARE_EQUAL;
		bool has_upper = comparison == ExpressionType::COMPARE_LESSTHAN ||
		                 comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
		                 comparison == ExpressionType::COMPARE_EQUAL;
		double lower = comparison == ExpressionType::COMPARE_GREATERTHAN ? DecideStrictBound(true, bound) : bound;
		double upper = comparison == ExpressionType::COMPARE_LESSTHAN ? DecideStrictBound(false, bound) : bound;
		DecideBoundBox box;
		if (has_lower) {
			DecideFoldSingletonRow(box, '>', lower);
		}
		if (has_upper) {
			DecideFoldSingletonRow(box, '<', upper);
		}
		if (DecideBoxContradictsDomain(box, true)) {
			return false;
		}
	}
	domain.zero = SatisfiesComparison(comparison, 0.0, bound);
	domain.one = SatisfiesComparison(comparison, 1.0, bound);
	return true;
}

const Expression *SourceBooleanFixedValue(const Expression &expr, idx_t decide_index,
                                          const vector<ColumnBinding> &source_bindings) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CAST) {
		return nullptr;
	}
	auto &cast = expr.Cast<BoundCastExpression>();
	if (cast.child->return_type != LogicalType::BOOLEAN || !expr.return_type.IsNumeric() ||
	    DirectMayThrow(*cast.child) || !DirectIsDecisionFreeDeterministic(*cast.child, decide_index) ||
	    !DirectReferencesOnlySource(*cast.child, source_bindings) || !DirectHasColumnReference(*cast.child)) {
		return nullptr;
	}
	return cast.child.get();
}

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}
	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (ir.decisions.size() != 1 || ir.decisions[0].domain != DirectDomain::BOOL ||
		    ir.decisions[0].scope != DecideVarScope::ROW || ir.decisions[0].output_type != LogicalType::INTEGER ||
		    !ir.entity_scopes.empty()) {
			return nullptr;
		}
		if (ir.constraints.empty() || ir.source_clause_count != ir.constraints.size()) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		vector<uint8_t> seen_sources(ir.source_clause_count, 0);
		for (auto &ir_node : ir.constraints) {
			if (ir_node.source_clause_id >= seen_sources.size() || seen_sources[ir_node.source_clause_id]++) {
				return nullptr;
			}
			if (ir_node.comparison == ExpressionType::COMPARE_IN) {
				return nullptr;
			}
			if (FixedTerms(ir_node)) {
				condition->local_fixes.push_back(&ir_node);
				continue;
			}
			if (!DirectIsLowerBound(ir_node.comparison) && !DirectIsUpperBound(ir_node.comparison)) {
				return nullptr;
			}
			condition->bound_factors.push_back(&ir_node);
		}
		if (condition->bound_factors.empty()) {
			return nullptr;
		}
		if (ir.objective.scope.when || !ir.objective.scope.per_key_slots.empty()) {
			return nullptr;
		}
		if (ir.objective.parts.empty() && ir.objective.sense != DecideSense::FEASIBILITY) {
			return nullptr;
		}
		condition->decide_index = ir.decide_index;
		condition->sense = ir.objective.sense;
		if (!ProveScope(ir, *condition) || !ProveFixedVariables(ir, context, *condition) ||
		    !ProveBounds(ir, context, *condition) || !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		return std::move(condition);
	}
	string Summary() const override {
		return "min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi";
	}
	DirectSubstitute Apply(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const vector<string> &source_fragments) const override;

private:
	bool ProveScope(const DirectIR &ir, Condition &condition) const;
	bool ProveFixedVariables(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! Every bound counts the same rows: one `SUM(x)` per bound with the same group keys and the same `when` membership.
//! Records the group keys and membership the plan scopes by. The front end fills no group key today (it declines a
//! `by` key); reading one must make a NULL key a group of its own (D5), where the scope leaves it out as the removed
//! PER did.
bool Rule::ProveScope(const DirectIR &ir, Condition &condition) const {
	const auto &first_scope = *condition.bound_factors.front();
	bool ok;
	auto first_when = first_scope.Membership(ok);
	if (!ok) {
		return false;
	}
	for (auto factor : condition.bound_factors) {
		auto when = factor->Membership(ok);
		if (!ok) {
			return false;
		}
		if (factor->scope.per_key_slots != first_scope.scope.per_key_slots ||
		    static_cast<bool>(when) != static_cast<bool>(first_when)) {
			return false;
		}
		if (when && !Expression::Equals(*when, *first_when)) {
			return false;
		}
	}
	condition.group_key_slots = first_scope.scope.per_key_slots;
	if (first_when) {
		if (!DirectIsSourceOnlyPredicate(*first_when, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		condition.when_condition = std::move(first_when);
	}
	condition.scoped = !condition.group_key_slots.empty() || condition.when_condition != nullptr;
	return true;
}

//! Per-row fixed Boolean values: `x = 0/1` and its one-sided forms against a constant, or against a source Boolean.
//! Each becomes a condition under which the row is fixed to one or to zero.
bool Rule::ProveFixedVariables(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	ExpressionBuilder calc(context);
	for (auto factor : condition.local_fixes) {
		if (!factor->scope.per_key_slots.empty()) {
			return false;
		}
		if (!IsUnitContribution(context, *FixedTerms(*factor))) {
			return false;
		}
		auto &when = factor->scope.when;
		if (when && !DirectIsSourceOnlyPredicate(*when, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		double constant;
		if (TryEvaluateFoldableDouble(context, *factor->rhs, constant, DIRECT_FINITE_CONSTANT)) {
			FixedDomain domain;
			if (!ConstantFixedDomain(*factor, constant, domain)) {
				return false;
			}
			// Both values allowed is no constraint. Neither allowed fixes the row to both, which the plan reports as
			// DECIDE's infeasible error.
			if (!domain.zero || !domain.one) {
				auto row_condition = [&]() {
					return calc.Out(when ? calc.Of(*when) : calc.Bool(true));
				};
				if (!domain.zero) {
					condition.fixed_one_conditions.push_back(row_condition());
				}
				if (!domain.one) {
					condition.fixed_zero_conditions.push_back(row_condition());
				}
			}
			continue;
		}
		auto type = factor->comparison;
		auto active = [&](const ExprPtr &value) {
			return calc.Out(when ? calc.If(calc.Of(*when), value, calc.Bool(false)) : value);
		};
		auto source_value = SourceBooleanFixedValue(*factor->rhs, ir.decide_index, ir.source_bindings);
		if (source_value &&
		    (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
		     type == ExpressionType::COMPARE_GREATERTHANOREQUALTO)) {
			if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
				condition.fixed_one_conditions.push_back(active(calc.Of(*source_value)));
			}
			if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
				condition.fixed_zero_conditions.push_back(
				    active(calc.Cmp(ExpressionType::COMPARE_EQUAL, calc.Of(*source_value), calc.Bool(false))));
			}
			condition.source_fixed_values.push_back({source_value->Copy(), false, when ? when->Copy() : nullptr});
			continue;
		}
		// A numeric value from the row: the row allows 0 when `0 <comparison> value` holds and 1 when `1 <comparison>
		// value` holds, as the solver reads it (with a `when` or not), so the row is fixed to the one value it allows
		// and is infeasible when it allows neither.
		auto &value = *factor->rhs;
		if (value.IsFoldable() || !DirectIsSourceOnlyNumeric(value, ir.decide_index, ir.source_bindings)) {
			return false;
		}
		auto disallows = [&](double allowed_value) {
			auto as_double = calc.Cast(calc.Of(value), LogicalType::DOUBLE);
			auto allowed = calc.Cmp(type, calc.Num(allowed_value), as_double);
			return calc.If(allowed, calc.Bool(false), calc.Bool(true));
		};
		condition.fixed_one_conditions.push_back(active(disallows(0.0)));
		condition.fixed_zero_conditions.push_back(active(disallows(1.0)));
		auto reject_nan = value.return_type == LogicalType::FLOAT || value.return_type == LogicalType::DOUBLE;
		condition.source_fixed_values.push_back({value.Copy(), reject_nan, when ? when->Copy() : nullptr});
	}
	return true;
}

//! The count bounds: constant ones fold into one inclusive interval, source-valued ones are kept for the plan to
//! read per group. Every bound counts `x` once, as `x` or `-x`; `SUM(-x) <op> B` is `SUM(x) <flipped op> -B`. A
//! constant term in the body (`SUM(x + c)`) is a decline.
bool Rule::ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	auto fold_bound = [&](const Expression &bound, ExpressionType side, idx_t &target, bool negate) {
		bool impossible = false;
		if (!NormalizeCapacity(context, bound, side, target, impossible, negate)) {
			return false;
		}
		condition.impossible |= impossible;
		return true;
	};
	for (auto factor : condition.bound_factors) {
		auto sum = factor->PlainSum();
		CountBody body;
		if (!ReadCountBody(context, sum->terms, body)) {
			return false;
		}
		auto &bound = *factor->rhs;
		bool negated = body.direction < 0;
		auto type = negated ? FlipComparison(factor->comparison) : factor->comparison;
		// A source-valued bound has one value per group already. The plan evaluates it on the rows of the groups, as
		// the solver does, so an expression that can raise (an overflow in `COUNT(*) * k` over a window column) raises
		// on both paths or on neither.
		if (!bound.IsFoldable() && DirectIsSourceOnlyNumericMayRaise(bound, ir.decide_index, ir.source_bindings)) {
			condition.source_bounds.push_back({type, bound.Copy(), negated});
			continue;
		}
		if (DirectIsLowerBound(type)) {
			idx_t lower;
			if (!fold_bound(bound,
			                type == ExpressionType::COMPARE_EQUAL ? ExpressionType::COMPARE_GREATERTHANOREQUALTO : type,
			                lower, negated)) {
				return false;
			}
			condition.lower = std::max(condition.lower, lower);
		}
		if (DirectIsUpperBound(type)) {
			idx_t upper;
			if (!fold_bound(bound,
			                type == ExpressionType::COMPARE_EQUAL ? ExpressionType::COMPARE_LESSTHANOREQUALTO : type,
			                upper, negated)) {
				return false;
			}
			condition.upper = condition.has_upper ? std::min(condition.upper, upper) : upper;
			condition.has_upper = true;
		}
	}
	if (condition.has_upper && condition.lower > condition.upper) {
		condition.impossible = true;
	}
	return true;
}

//! The objective: a signed sum of `SUM(coefficient * x)` terms. Each coefficient must be evaluable per source row.
bool Rule::ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	// Every linear term in x scores separately: `SUM((p + q) * x)` splits into `p * x` and `q * x`, and the score
	// adds the signed coefficients in order, as the solver's like-term collection does.
	auto &parts = ir.objective.parts;
	idx_t term_count = 0;
	for (auto &part : parts) {
		if (part.reducer != DirectReducer::SUM || part.filter || part.inner ||
		    part.qualifier != DConstants::INVALID_INDEX) {
			return false;
		}
		for (auto &term : part.terms) {
			if (term.kind != DecideTermKind::LINEAR || term.var_a != 0) {
				return false;
			}
		}
		term_count += part.terms.size();
	}
	for (auto &part : parts) {
		// A query-wide factor on the reducer (`2 * SUM(...)`, `SUM(...) / 2`) scales every term of that part and no
		// other. It must be a finite constant, and a divisor must not be zero. Anything else stays on the solver.
		DirectLinearPart read;
		if (!DirectReadLinearPart(ir, context, part, read)) {
			return false;
		}
		for (auto &term : read.terms) {
			if (term_count > 1 && DirectMayThrow(*term.coefficient)) {
				return false;
			}
			condition.objective_parts.push_back(
			    {read.sign * term.sign, std::move(term.coefficient), read.scaled, read.scale, read.divides});
		}
	}
	return true;
}

//! Builds the relational plan for one S1 problem whose condition holds. The constructor derives what every stage
//! shares; each stage takes the plan built so far and returns it extended. Data a later stage needs travels in explicit
//! structs.
//!
//!   ProjectScopeState -> AddBoundValidation -> ProjectScore
//!     -> RankAndFilterFeasibility -> ProjectAssignment
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, LogicalOperator &source,
	            const vector<string> &fragments_p)
	    : condition(condition_p), fragments(fragments_p), optimizer(optimizer_p), binder(optimizer_p.binder),
	      calc(optimizer_p.context) {
		// The rule driver hands the input over with its types resolved.
		source_bindings = source.GetColumnBindings();
		source_types = source.types;
		D_ASSERT(source_bindings.size() == source_types.size());
		score_index = binder.GenerateTableIndex();
		window_index = binder.GenerateTableIndex();
		result_index = binder.GenerateTableIndex();
		has_fixes = !condition.fixed_one_conditions.empty() || !condition.fixed_zero_conditions.empty();
		for (auto &bound : condition.source_bounds) {
			source_lower |= DirectIsLowerBound(bound.comparison);
			source_upper |= DirectIsUpperBound(bound.comparison);
		}
		has_lower = condition.lower || source_lower;
		has_upper = condition.has_upper || source_upper;
		// Slots of the score projection, which every later stage reads. This is the one place that lays them out;
		// ProjectScore appends its columns in this order and checks each against it.
		idx_t next_slot = source_bindings.size();
		score_binding = ColumnBinding(score_index, next_slot++);
		if (condition.scoped) {
			eligible_binding = ColumnBinding(score_index, next_slot++);
		}
		if (has_fixes) {
			fixed_one_binding = ColumnBinding(score_index, next_slot++);
			fixed_zero_binding = ColumnBinding(score_index, next_slot++);
		}
		for (idx_t i = 0; i < condition.source_bounds.size(); i++) {
			bound_bindings.push_back(ColumnBinding(score_index, next_slot++));
		}
	}

	DirectSubstitute Build(unique_ptr<LogicalOperator> source) const {
		ScopeBindings scope;
		source = ProjectScopeState(std::move(source), scope);
		source = AddFixedValueValidation(std::move(source), scope);
		source = ProjectScore(std::move(source), scope);
		auto ranked = RankAndFilterFeasibility(std::move(source));
		auto result = ProjectAssignment(std::move(ranked));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1); // x is pure once score validation and ranking remain live
		// The rank window reads every row before it emits one, so the hidden rank is this plan's validation breaker.
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.child = std::move(result);
		return substitute;
	}

private:
	//! Where the source columns and per-row flags live once the optional state projection has run.
	struct ScopeBindings {
		vector<ColumnBinding> input_bindings;
		idx_t state_index = DConstants::INVALID_INDEX;
		ColumnBinding eligible_binding;
		ColumnBinding fixed_one_binding;
		ColumnBinding fixed_zero_binding;
	};

	//! Ranked, feasibility-checked rows and the window slots the assignment reads.
	struct RankedRows {
		unique_ptr<LogicalOperator> plan;
		ColumnBinding rank_binding;
		idx_t fixed_count_slot = DConstants::INVALID_INDEX;
	};

	const Condition &condition;
	//! The DECIDE node's source-fragment registry, which a NULL message quotes the written subquery or cast from.
	const vector<string> &fragments;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	idx_t score_index;
	idx_t window_index;
	idx_t result_index;
	bool has_fixes = false;
	bool source_lower = false;
	bool source_upper = false;
	bool has_lower = false;
	bool has_upper = false;
	ColumnBinding score_binding;
	ColumnBinding eligible_binding;
	ColumnBinding fixed_one_binding;
	ColumnBinding fixed_zero_binding;
	//! Each source-valued count bound's value, in the score projection.
	vector<ColumnBinding> bound_bindings;

	//! Copy of a proved expression whose source references follow the state projection, when there is one.
	unique_ptr<Expression> CopyIntoState(const Expression &expr, const ScopeBindings &scope) const {
		auto value = expr.Copy();
		if (scope.state_index != DConstants::INVALID_INDEX) {
			// CheckCondition proved the expression reads source columns only.
			bool remapped = DirectRemapSourceReferences(*value, source_bindings, scope.state_index);
			D_ASSERT(remapped);
			(void)remapped;
		}
		return value;
	}

	//! Stage 1: pass the source through and append the per-row scope flags (group eligibility, fixed values).
	unique_ptr<LogicalOperator> ProjectScopeState(unique_ptr<LogicalOperator> source, ScopeBindings &scope) const {
		vector<unique_ptr<Expression>> fixed_flags;
		if (has_fixes) {
			// `Any` tests its first condition first; this plan tests the last first.
			auto any_of = [&](const vector<unique_ptr<Expression>> &conditions) {
				vector<ExprPtr> ordered;
				for (auto &condition : conditions) {
					ordered.push_back(calc.Of(*condition));
				}
				std::reverse(ordered.begin(), ordered.end());
				return calc.Out(calc.Any(ordered));
			};
			fixed_flags.push_back(any_of(condition.fixed_one_conditions));
			fixed_flags.push_back(any_of(condition.fixed_zero_conditions));
		}
		auto state = DirectProjectScope(binder, std::move(source), condition.scoped, condition.when_condition.get(),
		                                condition.group_key_slots, std::move(fixed_flags));
		scope.input_bindings = std::move(state.input_bindings);
		scope.state_index = state.state_index;
		scope.eligible_binding = state.eligible;
		if (has_fixes) {
			scope.fixed_one_binding = state.extra[0];
			scope.fixed_zero_binding = state.extra[1];
		}
		return std::move(state.plan);
	}

	//! Stage 2: validate the fixed values that come from the row, on the rows that read them. The source-valued bounds
	//! were validated with the scope checks under the plan (VALUE_IS_VALID).
	unique_ptr<LogicalOperator> AddFixedValueValidation(unique_ptr<LogicalOperator> source,
	                                                    const ScopeBindings &scope) const {
		vector<DirectRowBound> bounds;
		for (auto &fixed : condition.source_fixed_values) {
			DirectRowBound bound;
			bound.value = CopyIntoState(*fixed.value, scope);
			if (fixed.when) {
				bound.kept = DirectActiveWhen(calc, CopyIntoState(*fixed.when, scope).get());
			}
			bound.reject_nan = fixed.reject_nan;
			bounds.push_back(std::move(bound));
		}
		return DirectCheckRowBounds(optimizer, std::move(source), bounds, fragments);
	}

	//! Stage 3: the score projection. It forwards the source columns, the combined linear score, the scope and
	//! fixed-value flags, and the source-valued bounds, so every later stage reads one projection. The constructor
	//! laid out the slots; each is appended here at exactly that position.
	unique_ptr<LogicalOperator> ProjectScore(unique_ptr<LogicalOperator> source, const ScopeBindings &scope) const {
		vector<unique_ptr<Expression>> score_expressions;
		auto append_at = [&](const ColumnBinding &slot, unique_ptr<Expression> expression) {
			D_ASSERT(slot.table_index == score_index && slot.column_index == score_expressions.size());
			score_expressions.push_back(std::move(expression));
		};
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			score_expressions.push_back(calc.Out(calc.Col(source_types[i], scope.input_bindings[i])));
		}
		ExprPtr combined_score;
		// Without an objective there are no parts and every row scores 0.
		if (condition.objective_parts.size() != 1 || condition.objective_parts[0].sign < 0) {
			combined_score = calc.Num(0.0);
		}
		for (auto &part : condition.objective_parts) {
			auto coefficient = CopyIntoState(*part.coefficient, scope);
			auto term = calc.Wrap(
			    DirectScaledTerm(optimizer, std::move(coefficient), part.sign, part.scaled, part.scale, part.divides));
			combined_score = combined_score ? calc.Add(combined_score, term) : term;
		}
		append_at(score_binding, calc.Out(combined_score));
		if (condition.scoped) {
			append_at(eligible_binding, calc.Out(calc.Col(LogicalType::BOOLEAN, scope.eligible_binding)));
		}
		if (has_fixes) {
			append_at(fixed_one_binding, calc.Out(calc.Col(LogicalType::BOOLEAN, scope.fixed_one_binding)));
			append_at(fixed_zero_binding, calc.Out(calc.Col(LogicalType::BOOLEAN, scope.fixed_zero_binding)));
		}
		// A bound is read on the eligible rows only. On a row its clause's `when` drops, the windows it reads hold NULL
		// or a count of 0, so computing it there raises nothing.
		for (idx_t i = 0; i < condition.source_bounds.size(); i++) {
			auto &bound = condition.source_bounds[i];
			auto value = CopyIntoState(*bound.value, scope);
			if (bound.negated) {
				value = DirectScaledTerm(optimizer, std::move(value), -1, false, 1.0, false);
			}
			append_at(bound_bindings[i], calc.Out(calc.Cast(calc.Wrap(std::move(value)), LogicalType::DOUBLE)));
		}
		auto score = make_uniq<LogicalProjection>(score_index, std::move(score_expressions));
		score->children.push_back(std::move(source));
		return std::move(score);
	}

	//! Inclusive count limit of one source-valued bound, as BIGINT: its value rounded, clamped to the BIGINT range,
	//! and -1 (upper) or 0 (lower) when the value is below zero.
	ExprPtr SourceLimit(idx_t index, bool upper) const {
		auto &bound = condition.source_bounds[index];
		// Every caller asks for a side the bound has: an upper limit of an upper bound, a lower of a lower bound.
		D_ASSERT(upper ? DirectIsUpperBound(bound.comparison) : DirectIsLowerBound(bound.comparison));
		auto cap = [&]() {
			return calc.Col(LogicalType::DOUBLE, bound_bindings[index]);
		};
		bool strict =
		    bound.comparison == (upper ? ExpressionType::COMPARE_LESSTHAN : ExpressionType::COMPARE_GREATERTHAN);
		auto inclusive = calc.Cast(calc.Fn(upper == strict ? "ceil" : "floor", cap()), LogicalType::BIGINT);
		if (strict) {
			auto one = calc.Constant(Value::BIGINT(1));
			inclusive = upper ? calc.Sub(inclusive, one) : calc.Add(inclusive, one);
		}
		auto beyond_bigint =
		    calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, cap(), calc.Num(DECIDE_FIRST_DOUBLE_BEYOND_BIGINT));
		auto bounded =
		    calc.If(beyond_bigint, calc.Constant(Value::BIGINT(std::numeric_limits<int64_t>::max())), inclusive);
		auto below_zero =
		    calc.Cmp(strict == upper ? ExpressionType::COMPARE_LESSTHANOREQUALTO : ExpressionType::COMPARE_LESSTHAN,
		             cap(), calc.Num(0.0));
		return calc.If(below_zero, calc.Constant(Value::BIGINT(upper ? -1 : 0)), bounded);
	}

	//! Largest lower limit across the constant and source-valued bounds.
	ExprPtr LowerLimit() const {
		vector<ExprPtr> limits;
		limits.push_back(calc.Constant(Value::BIGINT(NumericCast<int64_t>(condition.lower))));
		for (idx_t i = 0; i < condition.source_bounds.size(); i++) {
			if (DirectIsLowerBound(condition.source_bounds[i].comparison)) {
				limits.push_back(SourceLimit(i, false));
			}
		}
		return calc.Max(limits);
	}

	//! Smallest upper limit across the constant and source-valued bounds. Only asked for when there is an upper bound.
	ExprPtr UpperLimit() const {
		vector<ExprPtr> limits;
		if (condition.has_upper) {
			limits.push_back(calc.Constant(Value::BIGINT(NumericCast<int64_t>(condition.upper))));
		}
		for (idx_t i = 0; i < condition.source_bounds.size(); i++) {
			if (DirectIsUpperBound(condition.source_bounds[i].comparison)) {
				limits.push_back(SourceLimit(i, true));
			}
		}
		D_ASSERT(!limits.empty());
		return calc.Min(limits);
	}

	//! Partition a rank or count window by eligibility and group key.
	void AddScope(BoundWindowExpression &expression) const {
		if (!condition.scoped) {
			return;
		}
		expression.partitions.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, eligible_binding)));
		for (auto slot : condition.group_key_slots) {
			expression.partitions.push_back(calc.Out(calc.Col(source_types[slot], ColumnBinding(score_index, slot))));
		}
	}

	//! A row with no fixed value, neither to one nor to zero.
	unique_ptr<Expression> FreeRow() const {
		return calc.Out(
		    calc.If(calc.Col(LogicalType::BOOLEAN, fixed_one_binding), calc.Bool(false),
		            calc.If(calc.Col(LogicalType::BOOLEAN, fixed_zero_binding), calc.Bool(false), calc.Bool(true))));
	}

	//! Order a window: free rows first when fixed values exist, then best score first.
	void AddOrder(BoundWindowExpression &expression) const {
		if (has_fixes) {
			expression.orders.emplace_back(OrderType::DESCENDING, OrderByNullType::NULLS_LAST, FreeRow());
		}
		expression.orders.emplace_back(
		    condition.sense == DecideSense::MAXIMIZE ? OrderType::DESCENDING : OrderType::ASCENDING,
		    OrderByNullType::NULLS_LAST, calc.Out(calc.Col(LogicalType::DOUBLE, score_binding)));
	}

	//! Stage 4: validate the score, rank rows within their scope, and raise the infeasibility error when the
	//! bounds, fixed values, and row counts cannot all hold.
	RankedRows RankAndFilterFeasibility(unique_ptr<LogicalOperator> score) const {
		// The score is checked above the score projection, which carries the source columns a NULL names.
		vector<DirectSignedValue> score_terms;
		for (auto &part : condition.objective_parts) {
			score_terms.push_back({part.sign, part.coefficient->Copy()});
		}
		auto score_source = DirectMergedOverProjection(optimizer.context, score_terms, source_bindings, score_index);
		auto guard = make_uniq<LogicalFilter>(DirectValidNumberPredicate(
		    optimizer, LogicalType::DOUBLE, score_binding, score_source.get(), DirectNumberKind::OBJECTIVE, fragments));
		guard->children.push_back(std::move(score));
		// Hand-built: a window is an operator-specific node, which the builder (scalar expressions) has no operation
		// for.
		auto rank =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
		rank->start = WindowBoundary::UNBOUNDED_PRECEDING;
		rank->end = WindowBoundary::UNBOUNDED_FOLLOWING;
		AddOrder(*rank);
		AddScope(*rank);
		auto window = make_uniq<LogicalWindow>(window_index);
		window->expressions.push_back(std::move(rank));
		RankedRows result;
		idx_t free_count_slot = DConstants::INVALID_INDEX;
		if (has_fixes) {
			result.fixed_count_slot = window->expressions.size();
			auto count = DirectWindowMatchingCount(calc, calc.Out(calc.Col(LogicalType::BOOLEAN, fixed_one_binding)));
			AddOrder(*count);
			AddScope(*count);
			window->expressions.push_back(std::move(count));
			if (has_lower && !condition.impossible) {
				free_count_slot = window->expressions.size();
				auto free_count = DirectWindowMatchingCount(calc, FreeRow());
				AddOrder(*free_count);
				AddScope(*free_count);
				window->expressions.push_back(std::move(free_count));
			}
		} else if (has_lower && !condition.impossible) {
			// Match the rank's order so the full-partition count can share its sort. Hand-built, like the rank above.
			auto count =
			    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_AGGREGATE, LogicalType::BIGINT,
			                                     make_uniq<AggregateFunction>(CountStarFun::GetFunction()), nullptr);
			count->start = WindowBoundary::UNBOUNDED_PRECEDING;
			count->end = WindowBoundary::UNBOUNDED_FOLLOWING;
			AddOrder(*count);
			AddScope(*count);
			window->expressions.push_back(std::move(count));
		}
		window->children.push_back(std::move(guard));
		result.rank_binding = ColumnBinding(window_index, 0);
		result.plan = std::move(window);
		if (condition.impossible || has_lower || has_fixes || source_upper) {
			auto feasible = make_uniq<LogicalFilter>(FeasiblePredicate(result, free_count_slot));
			feasible->children.push_back(std::move(result.plan));
			result.plan = std::move(feasible);
		}
		return result;
	}

	//! The feasibility predicate of stage 4: a list of checks, each true when it passes and DECIDE's infeasible error
	//! when it fails. Which failing check raises is not specified, and all of them raise the same error. The checks on
	//! the rows of a group apply to eligible rows only; a fixed-value conflict applies to every row.
	unique_ptr<Expression> FeasiblePredicate(const RankedRows &ranked, idx_t free_count_slot) const {
		// `ok` must hold; a NULL `ok` fails.
		auto must_hold = [&](const ExprPtr &ok) {
			return DirectRaiseUnless(calc, calc.Out(ok), DirectInfeasibleErrorPredicate(optimizer));
		};
		// `bad` must not hold; a NULL `bad` passes.
		auto must_not_hold = [&](const ExprPtr &bad) {
			return DirectRaiseIf(calc, calc.Out(bad), DirectInfeasibleErrorPredicate(optimizer));
		};
		auto fixed_count = [&]() {
			return calc.Col(LogicalType::BIGINT, ColumnBinding(window_index, ranked.fixed_count_slot));
		};
		vector<unique_ptr<Expression>> group_checks;
		if (condition.impossible) {
			// No bound can hold, so a group with any row is infeasible.
			group_checks.push_back(must_not_hold(calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
			                                              calc.Col(LogicalType::BIGINT, ranked.rank_binding),
			                                              calc.Constant(Value::BIGINT(1)))));
		} else {
			if (has_lower) {
				auto available_rows =
				    has_fixes ? calc.Add(fixed_count(),
				                         calc.Col(LogicalType::BIGINT, ColumnBinding(window_index, free_count_slot)))
				              : calc.Col(LogicalType::BIGINT, ColumnBinding(window_index, 1));
				group_checks.push_back(
				    must_hold(calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, available_rows, LowerLimit())));
			}
			if ((source_lower || source_upper) && has_upper) {
				group_checks.push_back(
				    must_hold(calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, LowerLimit(), UpperLimit())));
			}
			if (source_lower) {
				for (idx_t i = 0; i < condition.source_bounds.size(); i++) {
					if (!DirectIsLowerBound(condition.source_bounds[i].comparison)) {
						continue;
					}
					group_checks.push_back(must_not_hold(calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
					                                              calc.Col(LogicalType::DOUBLE, bound_bindings[i]),
					                                              calc.Num(DECIDE_FIRST_DOUBLE_BEYOND_BIGINT))));
				}
			}
			if (has_fixes && has_upper) {
				group_checks.push_back(
				    must_hold(calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, fixed_count(), UpperLimit())));
			}
		}
		vector<unique_ptr<Expression>> checks;
		if (!group_checks.empty()) {
			auto group = DirectAllHold(calc, std::move(group_checks));
			checks.push_back(condition.scoped ? calc.Out(calc.If(calc.Col(LogicalType::BOOLEAN, eligible_binding),
			                                                     calc.Wrap(std::move(group)), calc.Bool(true)))
			                                  : std::move(group));
		}
		if (has_fixes) {
			// A row fixed to both values.
			checks.push_back(
			    must_not_hold(calc.If(calc.Col(LogicalType::BOOLEAN, fixed_one_binding),
			                          calc.Col(LogicalType::BOOLEAN, fixed_zero_binding), calc.Bool(false))));
		}
		return DirectAllHold(calc, std::move(checks));
	}

	//! Stage 5: the 0/1 assignment (fixed values, then scope, then upper and lower rank limits, then score sign), the
	//! source columns, and the hidden rank that keeps score validation live.
	unique_ptr<LogicalOperator> ProjectAssignment(RankedRows ranked) const {
		auto &rank_binding = ranked.rank_binding;
		auto fixed_count_slot = ranked.fixed_count_slot;
		vector<unique_ptr<Expression>> result_expressions;
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			result_expressions.push_back(calc.Out(calc.Col(source_types[i], ColumnBinding(score_index, i))));
		}
		auto improving = calc.Cmp(condition.sense == DecideSense::MAXIMIZE ? ExpressionType::COMPARE_GREATERTHAN
		                                                                   : ExpressionType::COMPARE_LESSTHAN,
		                          calc.Col(LogicalType::DOUBLE, score_binding), calc.Num(0.0));
		auto free_choose = improving;
		auto choose = improving;
		auto rank = calc.Col(LogicalType::BIGINT, rank_binding);
		auto fixed_count = [&]() {
			return calc.Col(LogicalType::BIGINT, ColumnBinding(window_index, fixed_count_slot));
		};
		if (has_lower) {
			auto remaining_lower = has_fixes ? calc.Sub(LowerLimit(), fixed_count()) : LowerLimit();
			auto within_lower = calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, rank, remaining_lower);
			choose = calc.If(within_lower, calc.Bool(true), choose);
		}
		if (has_upper) {
			auto remaining_upper = has_fixes ? calc.Sub(UpperLimit(), fixed_count()) : UpperLimit();
			auto within_upper = calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, rank, remaining_upper);
			choose = calc.If(within_upper, choose, calc.Bool(false));
		}
		if (condition.scoped) {
			choose = calc.If(calc.Col(LogicalType::BOOLEAN, eligible_binding), choose, free_choose);
		}
		if (has_fixes) {
			choose = calc.If(calc.Col(LogicalType::BOOLEAN, fixed_zero_binding), calc.Bool(false), choose);
			choose = calc.If(calc.Col(LogicalType::BOOLEAN, fixed_one_binding), calc.Bool(true), choose);
		}
		result_expressions.push_back(
		    calc.Out(calc.If(choose, calc.Constant(Value::INTEGER(1)), calc.Constant(Value::INTEGER(0)))));
		// The otherwise unused rank is a blocking dependency. It forces a complete
		// value read before an outer LIMIT, COUNT, or column-pruning parent can return.
		result_expressions.push_back(calc.Out(rank));
		auto result = make_uniq<LogicalProjection>(result_index, std::move(result_expressions));
		result->children.push_back(std::move(ranked.plan));
		return std::move(result);
	}
};

DirectSubstitute Rule::Apply(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate,
                             const vector<string> &source_fragments) const {
	auto &condition = static_cast<const Condition &>(candidate);
	// CheckCondition declines an objective that is not there, and an objective that has no linear term.
	D_ASSERT(!condition.objective_parts.empty() || condition.sense == DecideSense::FEASIBILITY);
	PlanBuilder plan_builder(condition, optimizer, *source, source_fragments);
	return plan_builder.Build(std::move(source));
}

} // namespace direct_s1_uniform_matroid

unique_ptr<DirectSolveRule> MakeS1UniformMatroidRule() {
	return make_uniq<direct_s1_uniform_matroid::Rule>();
}

} // namespace duckdb
