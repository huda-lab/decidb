#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_row_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
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
		//! One value on every row the clause keeps already: read per row. Null with a `group`.
		unique_ptr<Expression> value;
		//! The count body is `-x`, so `comparison` is already flipped and the value to compare against is `-B`.
		bool negated = false;
		//! The bound-side rewrite left the bound to the plan (DecideGroupBound): the plan computes its aggregates in a
		//! hash aggregate over the group, joined to every row, and makes the checks the rewrite would have made. Null
		//! when `value` is read.
		unique_ptr<DecideGroupBound> group;
	};
	struct ObjectivePart {
		//! The reducer the term is of, an index into `part_names`.
		idx_t part;
		int sign;
		unique_ptr<Expression> coefficient;
		//! A constant factor the part's objective term is multiplied or divided by.
		DirectFactor factor;
	};

	//! The constraints of the IR, which outlives the condition: the count bounds, and the fixed Boolean values.
	vector<const DirectIRConstraint *> bound_factors;
	vector<const DirectIRConstraint *> local_fixes;

	idx_t lower = 0;
	idx_t upper = 0;
	bool has_upper = false;
	bool impossible = false;
	vector<ObjectivePart> objective_parts;
	//! What a message names for each reducer's share of the score (DirectNamesOf), and the objective, which the score
	//! names once every share is fine and their sum is not.
	vector<DirectNumberNames> part_names;
	//! The rows each reducer reads (DirectObjectivePartKept), null for every row: any other row scores 0 in it.
	vector<unique_ptr<Expression>> part_kept;
	string objective_written;
	//! The rows and groups every bound counts: the `by` key (the groups, each its own interval; one group of all the
	//! rows the bounds cover when empty; a row with a NULL in a NO_GROUP element is in no group, D5), the `per` key,
	//! and the `when` membership.
	DirectSharedScope scope;
	DirectFixedValues fixed;
	vector<SourceBound> source_bounds;

	//! Whether some bound is left to the plan to compute from the aggregates of its group.
	bool HasGroupBound() const {
		for (auto &bound : source_bounds) {
			if (bound.group) {
				return true;
			}
		}
		return false;
	}
};

//! What the sum counts: `direction * (number of selected rows)`.
struct CountBody {
	//! +1 for `x`; -1 for `-x`.
	int direction = 1;
};

//! Reads a reducer body as `a*x`. The signed constant coefficients of `x` must add up to exactly +1 or -1, as the
//! solver's like-term collection sums them. Any other term, a constant included, is a decline: a constant term in the
//! body shifts the limit by an amount that depends on the group's row count, and the solver answers that.
bool ReadCountBody(ClientContext &context, const vector<DecideSplitTerm> &terms, CountBody &body) {
	double slope;
	if (!DirectOnlyLinearTerms(terms) || !DirectConstantSlope(context, terms, slope) ||
	    (slope != 1.0 && slope != -1.0)) {
		return false;
	}
	body.direction = slope > 0 ? 1 : -1;
	return true;
}

//! Turns `SUM(x) <comparison> expr` (`SUM(x) <comparison> -expr` when `negate`) into an inclusive count limit in
//! [0, 2^53]. A bound below zero on an upper side is infeasible; one at or below zero on a lower side is no bound.
//! False when `expr` is not an exactly representable finite constant.
bool NormalizeCapacity(ClientContext &context, const Expression &expr, ExpressionType comparison, idx_t &capacity,
                       bool &impossible, bool negate) {
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

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}
	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (ir.decisions.size() != 1 || ir.decisions[0].domain != DirectDomain::BOOL ||
		    ir.decisions[0].scope != DecideVarScope::ROW || !ir.keyed_scopes.empty()) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		for (auto &ir_node : ir.constraints) {
			if (ir_node.comparison == ExpressionType::COMPARE_IN) {
				return nullptr;
			}
			if (DirectFixedTerms(ir_node)) {
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
		if (!ProveScope(ir, *condition) ||
		    !DirectReadFixedValues(ir, context, condition->local_fixes, condition->fixed) ||
		    !ProveBounds(ir, context, *condition) || !ProveObjective(ir, context, *condition)) {
			return nullptr;
		}
		// The aggregates of the group bounds are computed on a read of their own (BuildGroupBounds).
		condition->source_reads = condition->HasGroupBound() ? 2 : 1;
		return std::move(condition);
	}
	string Summary() const override {
		return "min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi";
	}
	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveScope(const DirectIR &ir, Condition &condition) const;
	bool ProveBounds(const DirectIR &ir, ClientContext &context, Condition &condition) const;
	bool ProveObjective(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! Every bound counts the same rows: one `SUM(x)` per bound with the same group keys and the same `when` membership.
//! Records the group keys (the `by` key of the `SUM(x)`s, and the `per` key of the clauses) and the membership the plan
//! scopes by. A row whose `by` key is NULL is in no group (D5): the plan ranks it apart from every group, and a `when`
//! that drops a row does the same.
bool Rule::ProveScope(const DirectIR &ir, Condition &condition) const {
	return DirectProveSharedScope(ir, condition.bound_factors, condition.scope);
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
		auto type = negated ? FlipComparisonExpression(factor->comparison) : factor->comparison;
		if (factor->group_bound) {
			if (!DirectGroupBoundIsReadable(ir, *factor->group_bound)) {
				return false;
			}
			condition.source_bounds.push_back({type, nullptr, negated, factor->group_bound->Copy()});
			continue;
		}
		// A source-valued bound has one value per group already. The plan evaluates it on the rows of the groups, as
		// the solver does, so an expression that can raise (an overflow in `COUNT(*) * k` over a window column) raises
		// on both paths or on neither.
		if (!bound.IsFoldable() && DirectIsSourceOnlyNumericMayRaise(bound, ir.decide_index, ir.source_bindings)) {
			condition.source_bounds.push_back({type, bound.Copy(), negated, nullptr});
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
	condition.objective_written = ir.objective.written;
	for (auto &part : parts) {
		if (!part.IsPlainSum() || !DirectOnlyLinearTerms(part.terms)) {
			return false;
		}
		term_count += part.terms.size();
	}
	for (auto &part : parts) {
		// A query-wide factor on the reducer (`2 * SUM(...)`, `SUM(...) / 2`) scales every term of that part and no
		// other. It must be a finite constant, and a divisor must not be zero. Anything else stays on the solver.
		DirectLinearPart read;
		unique_ptr<Expression> kept;
		if (!DirectReadLinearPart(ir, context, part, ir.objective.written, read) ||
		    !DirectObjectivePartKept(ir, part, kept)) {
			return false;
		}
		for (auto &term : read.terms) {
			if (term_count > 1 && DirectMayThrow(*term.coefficient)) {
				return false;
			}
			condition.objective_parts.push_back(
			    {condition.part_names.size(), read.sign * term.sign, std::move(term.coefficient), read.factor});
		}
		// Every term is in `x`, the one decision.
		condition.part_names.push_back(std::move(read.names[0]));
		condition.part_kept.push_back(std::move(kept));
	}
	return true;
}

//! Builds the relational plan for one S1 problem whose condition holds. The constructor derives what every stage
//! shares; each stage takes the plan built so far and returns it extended. Data a later stage needs travels in explicit
//! structs.
//!
//!   ProjectScopeState -> ProjectScore -> RankRows [-> verdict window] -> ProjectAssignment
//!
//! The solver reads every row before it answers, so a bad value or an infeasible bound in any group raises even under
//! an outer LIMIT 1. A window holds every row before it releases one, and releases them group by group, so a check that
//! stands above it reaches a later group only when the parent reads that far. The checks of the row (the score, what a
//! group bound reads, the values the row fixes) therefore stand in the score projection, below the rank window, which
//! holds every row before it gives one. The checks of a group (what its bounds give, and whether its rows and fixed
//! values can meet them) read the rank window's counts and the group's bounds, so they stand above it. A window over
//! all the rows (the verdict window, as S3's W3) is read by every row, so every row has passed them when the first
//! leaves; it stands only where a group can fail (NeedsVerdict).
//!
//! The aggregates of the group bounds are not the rank's: they are computed once per group by a hash aggregate over a
//! first read of the source (BuildGroupBounds), and RankRows joins them to every row of the window (group-then-join,
//! DirectJoinClassAnswers), where the checks that read them stand.
class PlanBuilder {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : condition(condition_p), optimizer(optimizer_p), binder(optimizer_p.binder), calc(optimizer_p.context),
	      source_bindings(source_bindings_p), source_types(source_types_p), errors(errors_p) {
		D_ASSERT(source_bindings.size() == source_types.size());
		score_index = binder.GenerateTableIndex();
		window_index = binder.GenerateTableIndex();
		verdict_index = binder.GenerateTableIndex();
		result_index = binder.GenerateTableIndex();
		has_fixes = condition.fixed.Any();
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
		has_score_message = ScoreCanFail();
		if (condition.scope.Scoped()) {
			class_key = DirectClassKeyOf(condition.scope.keys, condition.scope.when || condition.scope.gated);
			eligible_binding = ColumnBinding(score_index, next_slot++);
			for (idx_t i = 0; i < class_key.types.size(); i++) {
				key_bindings.push_back(ColumnBinding(score_index, next_slot++));
			}
		}
		if (has_fixes) {
			fixed_one_binding = ColumnBinding(score_index, next_slot++);
			fixed_zero_binding = ColumnBinding(score_index, next_slot++);
		}
		for (auto &bound : condition.source_bounds) {
			// A bound of the group's aggregates is computed above the rank window, not per row.
			bound_bindings.push_back(bound.group ? ColumnBinding() : ColumnBinding(score_index, next_slot++));
			for (idx_t r = 0; bound.group && r < bound.group->inputs.size(); r++) {
				limit_message_count +=
				    DirectGroupLayout(bound.group->inputs[r], condition.scope.Scoped()).invalid ? 1 : 0;
			}
		}
		// The checks of the row (HasRowChecks): one column that raises them.
		if (HasRowChecks()) {
			checks_binding = ColumnBinding(score_index, next_slot++);
		}
	}

	//! `classes` is the plan of the group bounds' aggregates (BuildGroupBounds), or null when no bound reads them.
	DirectSubstitute Build(unique_ptr<LogicalOperator> source, unique_ptr<LogicalOperator> classes) const {
		ScopeBindings scope;
		source = ProjectScopeState(std::move(source), scope);
		source = ProjectScore(std::move(source), scope);
		auto ranked = RankRows(std::move(source), std::move(classes));
		auto result = ProjectAssignment(std::move(ranked));

		DirectSubstitute substitute;
		substitute.prunable_decisions.push_back(1); // x is pure once score validation and ranking remain live
		// The rank window reads every row before it emits one, so the hidden rank is this plan's validation breaker.
		// The column of the checks follows it, when the plan owes any.
		substitute.validated_columns.push_back(source_bindings.size() + 1);
		if (HasRowChecks() || NeedsVerdict()) {
			substitute.validated_columns.push_back(source_bindings.size() + 2);
		}
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			substitute.slot_map.push_back(i);
		}
		substitute.child = std::move(result);
		return substitute;
	}

	//! The class plan over the first read `rows`: a hash aggregate on the scope (the class key; one row for all the
	//! rows when the bounds have no scope) holding the aggregates of the group bounds. `group_values` are then
	//! expressions over its columns, which Build joins to every row.
	unique_ptr<LogicalOperator> BuildGroupBounds(unique_ptr<LogicalOperator> rows) const {
		ScopeBindings scope;
		auto plan = ProjectScopeState(std::move(rows), scope);
		auto into_state = [&](const Expression &expression) {
			return CopyIntoState(expression, scope);
		};
		auto eligible = condition.scope.Scoped() ? calc.Col(LogicalType::BOOLEAN, scope.eligible_binding) : nullptr;
		DirectProjectionStage inputs(binder.GenerateTableIndex());
		vector<vector<DirectGroupInputColumns>> columns(condition.source_bounds.size());
		vector<ColumnBinding> key;
		for (idx_t i = 0; i < scope.key_bindings.size(); i++) {
			key.push_back(inputs.Add(calc.Out(calc.Col(KeyType(i), scope.key_bindings[i]))));
		}
		for (idx_t i = 0; i < condition.source_bounds.size(); i++) {
			auto &bound = condition.source_bounds[i];
			if (!bound.group) {
				continue;
			}
			// The messages are per row (ProjectScore), not the first read's.
			columns[i] = DirectLayoutGroupInputs(optimizer, calc, inputs, *bound.group, eligible, into_state, nullptr);
		}
		plan = inputs.Finish(std::move(plan));

		DirectAggregateStage stage(binder);
		vector<ColumnBinding> groups;
		vector<ExprPtr> group_columns;
		for (idx_t i = 0; i < key.size(); i++) {
			groups.push_back(stage.Group(calc.Out(calc.Col(class_key.types[i], key[i]))));
			group_columns.push_back(calc.Col(class_key.types[i], groups.back()));
		}
		DirectAggregateSink sink(stage);
		auto in_group = condition.scope.Scoped() ? DirectClassPresent(calc, class_key, group_columns) : nullptr;
		group_values.assign(condition.source_bounds.size(), DirectGroupValues());
		for (idx_t i = 0; i < condition.source_bounds.size(); i++) {
			auto &bound = condition.source_bounds[i];
			if (bound.group) {
				group_values[i] = DirectAddGroupBound(calc, *bound.group, columns[i], sink, in_group);
			}
		}
		return stage.Finish(std::move(plan));
	}

private:
	//! Where the source columns and per-row flags live once the optional state projection has run.
	struct ScopeBindings {
		vector<ColumnBinding> input_bindings;
		idx_t state_index = DConstants::INVALID_INDEX;
		ColumnBinding eligible_binding;
		//! The group key's elements.
		vector<ColumnBinding> key_bindings;
		ColumnBinding fixed_one_binding;
		ColumnBinding fixed_zero_binding;
	};

	//! Ranked rows and the window slots the assignment reads.
	struct RankedRows {
		unique_ptr<LogicalOperator> plan;
		ColumnBinding rank_binding;
		idx_t fixed_count_slot = DConstants::INVALID_INDEX;
		//! Whether every group is feasible, in the verdict window; unset without one (NeedsVerdict).
		ColumnBinding verdict_binding;
	};

	const Condition &condition;
	Optimizer &optimizer;
	Binder &binder;
	ExpressionBuilder calc;
	vector<ColumnBinding> source_bindings;
	vector<LogicalType> source_types;
	const DirectStateErrors &errors;
	idx_t score_index;
	idx_t window_index;
	idx_t verdict_index;
	idx_t result_index;
	bool has_fixes = false;
	bool source_lower = false;
	bool source_upper = false;
	bool has_lower = false;
	bool has_upper = false;
	ColumnBinding score_binding;
	//! Whether a score can fail: be NaN or infinite, or the sum of the parts' scores pass a DOUBLE. A score made of
	//! values of exact types, whose bounds sum below the range of a DOUBLE (DirectScaledMagnitude), cannot, and the
	//! plan has no check for it.
	bool has_score_message = false;
	ColumnBinding eligible_binding;
	//! The class key of the scope, and its columns in the score projection. Empty without a scope.
	DirectClassKey class_key;
	vector<ColumnBinding> key_bindings;
	ColumnBinding fixed_one_binding;
	ColumnBinding fixed_zero_binding;
	//! Each source-valued count bound's value, in the score projection; unset for a bound of the group's aggregates.
	vector<ColumnBinding> bound_bindings;
	//! What each group bound gives once the class plan has computed its aggregates (BuildGroupBounds), over its
	//! columns.
	mutable vector<DirectGroupValues> group_values;
	//! The checks of the row, in the score projection: true when they all pass, and the error of the first that fails
	//! otherwise. Set only when the plan owes any (HasRowChecks).
	ColumnBinding checks_binding;
	//! How many inputs of the group bounds can raise a NaN limit.
	idx_t limit_message_count = 0;

	//! Whether a row can fail a check of its own: a NaN in what a group bound reads, a value the row fixes, the score,
	//! or a row fixed to both values.
	bool HasRowChecks() const {
		return limit_message_count > 0 || !condition.fixed.source_values.empty() || has_score_message || has_fixes;
	}

	//! Whether a group can fail a check of its own. Those checks read the rank window's counts and the group's bounds,
	//! so they stand above the rank window, and the verdict window must read them before the first row leaves: whether
	//! the group's rows and fixed values can meet its bounds, and what a group bound gives (a NaN limit, an `=` bound
	//! that takes two values in a group), which only a source-valued bound has, and which always makes the limits'
	//! check below necessary.
	bool NeedsVerdict() const {
		return condition.impossible || has_lower || ((source_lower || source_upper) && has_upper) ||
		       (has_fixes && has_upper);
	}

	//! The type of class key column `i`.
	const LogicalType &KeyType(idx_t i) const {
		return class_key.types[i];
	}

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
			fixed_flags.push_back(DirectAnyCondition(calc, condition.fixed.one_conditions));
			fixed_flags.push_back(DirectAnyCondition(calc, condition.fixed.zero_conditions));
		}
		unique_ptr<Expression> instance;
		if (condition.scope.gated) {
			ColumnBinding flag;
			source = DirectAddInstanceFlag(optimizer, std::move(source), condition.scope.when.get(),
			                               condition.scope.classes, flag);
			instance = calc.Out(calc.Col(LogicalType::BOOLEAN, flag));
		}
		auto state = DirectProjectScope(binder, std::move(source), condition.scope.Scoped(), condition.scope.when.get(),
		                                condition.scope.keys, std::move(fixed_flags), std::move(instance));
		scope.input_bindings = std::move(state.input_bindings);
		scope.state_index = state.state_index;
		scope.eligible_binding = state.eligible;
		scope.key_bindings = state.keys;
		if (has_fixes) {
			scope.fixed_one_binding = state.extra[0];
			scope.fixed_zero_binding = state.extra[1];
		}
		return std::move(state.plan);
	}

	//! Whether the score of a row can fail: the terms of the parts, with their factors, are bounded by a sum below the
	//! range of a DOUBLE only if none can.
	bool ScoreCanFail() const {
		double total = 0.0;
		for (auto &term : condition.objective_parts) {
			total += DirectScaledMagnitude(optimizer, *term.coefficient, term.factor);
		}
		return !DirectSumIsFinite(total);
	}

	//! Whether reducer `part` scores a finite constant on every row, which no row can fail (DirectIsFiniteWhereKept).
	bool PartIsFiniteConstant(idx_t part) const {
		vector<DirectSignedValue> values;
		const Condition::ObjectivePart *scaling = nullptr;
		for (auto &term : condition.objective_parts) {
			if (term.part == part) {
				values.push_back({term.sign, term.coefficient->Copy()});
				scaling = &term;
			}
		}
		D_ASSERT(scaling);
		return DirectIsFiniteWhereKept(optimizer, values, scaling->factor);
	}

	//! Stage 3: the score projection. It forwards the source columns, the combined linear score, the scope and
	//! fixed-value flags, the source-valued bounds and the checks of the row, so every later stage reads one
	//! projection. The checks stand here, below the rank window, which holds every row before it gives one. The
	//! constructor laid out the slots; each is appended here at exactly that position.
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
		vector<ExprPtr> part_scores(condition.part_names.size());
		for (auto &part : condition.objective_parts) {
			auto coefficient = CopyIntoState(*part.coefficient, scope);
			auto term = calc.Wrap(DirectScaledTerm(optimizer, std::move(coefficient), part.sign, part.factor));
			if (auto &kept = condition.part_kept[part.part]) {
				// A row the part drops reads no coefficient, so nothing in it can raise, and scores 0.
				auto over_state = CopyIntoState(*kept, scope);
				term = calc.If(calc.Wrap(DirectActiveWhen(calc, over_state.get())), term, calc.Num(0.0));
			}
			combined_score = combined_score ? calc.Add(combined_score, term) : term;
			part_scores[part.part] = part_scores[part.part] ? calc.Add(part_scores[part.part], term) : term;
		}
		append_at(score_binding, calc.Out(combined_score));
		// The checks of the row: the NaN check of what each group bound reads, each value the row fixes, the score, then
		// a row fixed to both values.
		vector<unique_ptr<Expression>> row_checks;
		// A reducer whose coefficient fails is named before the score they make together, which names the objective.
		auto into_score = [&](const Expression &input) {
			return calc.Wrap(CopyIntoState(input, scope));
		};
		ExprPtr score_message;
		if (has_score_message) {
			vector<ExprPtr> candidates;
			for (idx_t p = 0; p < part_scores.size(); p++) {
				// Every part has a term (PartIsFiniteConstant finds it).
				D_ASSERT(part_scores[p]);
				if (!PartIsFiniteConstant(p)) {
					candidates.push_back(
					    DirectNumberMessage(calc, part_scores[p], condition.part_names[p], into_score));
				}
			}
			candidates.push_back(DirectNumberMessage(calc, combined_score, condition.objective_written));
			score_message = calc.Coalesce(candidates);
		}
		if (condition.scope.Scoped()) {
			append_at(eligible_binding, calc.Out(calc.Col(LogicalType::BOOLEAN, scope.eligible_binding)));
			for (idx_t i = 0; i < key_bindings.size(); i++) {
				append_at(key_bindings[i], calc.Out(calc.Col(KeyType(i), scope.key_bindings[i])));
			}
		}
		if (has_fixes) {
			append_at(fixed_one_binding, calc.Out(calc.Col(LogicalType::BOOLEAN, scope.fixed_one_binding)));
			append_at(fixed_zero_binding, calc.Out(calc.Col(LogicalType::BOOLEAN, scope.fixed_zero_binding)));
		}
		// A bound is read on the eligible rows only. On a row its clause's `when` drops, the windows it reads hold NULL
		// or a count of 0, so computing it there raises nothing.
		auto eligible = condition.scope.Scoped() ? calc.Col(LogicalType::BOOLEAN, scope.eligible_binding) : nullptr;
		auto into_state = [&](const Expression &expression) {
			return CopyIntoState(expression, scope);
		};
		vector<vector<DirectGroupInput>> group_columns;
		for (idx_t i = 0; i < condition.source_bounds.size(); i++) {
			auto &bound = condition.source_bounds[i];
			group_columns.emplace_back();
			if (bound.group) {
				// Only the messages are per row; the aggregates read the first read (BuildGroupBounds).
				group_columns.back() = DirectGroupInputs(calc, *bound.group, eligible, into_state);
				continue;
			}
			auto value = CopyIntoState(*bound.value, scope);
			if (bound.negated) {
				value = DirectScaledTerm(optimizer, std::move(value), -1);
			}
			append_at(bound_bindings[i], calc.Out(calc.Cast(calc.Wrap(std::move(value)), LogicalType::DOUBLE)));
		}
		for (auto &columns : group_columns) {
			for (auto &input : columns) {
				if (input.invalid) {
					row_checks.push_back(DirectRaiseMessage(optimizer, calc, input.invalid));
				}
			}
		}
		for (auto &fixed : condition.fixed.source_values) {
			auto into_state = [&](const Expression &value) {
				return CopyIntoState(value, scope);
			};
			row_checks.push_back(DirectRaiseMessage(
			    optimizer, calc, DirectFixedValueMessage(optimizer.context, calc, fixed, into_state)));
		}
		if (score_message) {
			row_checks.push_back(DirectRaiseMessage(optimizer, calc, score_message));
		}
		if (has_fixes) {
			// A row fixed to both values.
			auto both = calc.If(calc.Col(LogicalType::BOOLEAN, scope.fixed_one_binding),
			                    calc.Col(LogicalType::BOOLEAN, scope.fixed_zero_binding), calc.Bool(false));
			row_checks.push_back(
			    DirectRaiseIf(calc, calc.Out(both), DirectInfeasibleErrorPredicate(optimizer, errors)));
		}
		if (HasRowChecks()) {
			append_at(checks_binding, calc.Out(DirectAllHold(calc, std::move(row_checks))));
		}
		auto score = make_uniq<LogicalProjection>(score_index, std::move(score_expressions));
		score->children.push_back(std::move(source));
		return std::move(score);
	}

	//! The value of source-valued bound `index` on a side, as DOUBLE: the group's value when the plan computes its
	//! aggregates, else the bound's own column. A negated bound (`SUM(-x) <= B` is `SUM(x) >= -B`) holds B, which is
	//! the limit the bound gives on the other side as written, negated.
	ExprPtr BoundValue(idx_t index, bool upper) const {
		auto &bound = condition.source_bounds[index];
		if (!bound.group) {
			return calc.Col(LogicalType::DOUBLE, bound_bindings[index]);
		}
		auto &limit = upper != bound.negated ? group_values[index].upper : group_values[index].lower;
		D_ASSERT(limit);
		if (bound.negated) {
			return calc.Wrap(DirectScaledTerm(optimizer, calc.Out(limit), -1));
		}
		return calc.Cast(limit, LogicalType::DOUBLE);
	}

	//! Inclusive count limit of one source-valued bound, as BIGINT: its value rounded, clamped to the BIGINT range,
	//! and -1 (upper) or 0 (lower) when the value is below zero. A NULL value skips its instance: no limit.
	ExprPtr SourceLimit(idx_t index, bool upper) const {
		auto &bound = condition.source_bounds[index];
		// Every caller asks for a side the bound has: an upper limit of an upper bound, a lower of a lower bound.
		D_ASSERT(upper ? DirectIsUpperBound(bound.comparison) : DirectIsLowerBound(bound.comparison));
		auto cap = [&]() {
			return BoundValue(index, upper);
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
		auto limit = calc.If(below_zero, calc.Constant(Value::BIGINT(upper ? -1 : 0)), bounded);
		auto open = calc.Constant(Value::BIGINT(upper ? std::numeric_limits<int64_t>::max() : 0));
		return calc.If(calc.IsNull(cap()), open, limit);
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

	//! Partition a rank or count window by class key: a row in no class has a NULL where the key is compared with `=`,
	//! so it ranks apart from every class.
	void AddScope(BoundWindowExpression &expression) const {
		if (!condition.scope.Scoped()) {
			return;
		}
		for (idx_t i = 0; i < key_bindings.size(); i++) {
			expression.partitions.push_back(calc.Out(calc.Col(KeyType(i), key_bindings[i])));
		}
	}

	//! A row with no fixed value, neither to one nor to zero.
	unique_ptr<Expression> FreeRow() const {
		return calc.Out(
		    calc.If(calc.Col(LogicalType::BOOLEAN, fixed_one_binding), calc.Bool(false),
		            calc.If(calc.Col(LogicalType::BOOLEAN, fixed_zero_binding), calc.Bool(false), calc.Bool(true))));
	}

	//! Order a window: free rows first when fixed values exist, then best score first (the least: the score is
	//! minimized).
	void AddOrder(BoundWindowExpression &expression) const {
		if (has_fixes) {
			expression.orders.emplace_back(OrderType::DESCENDING, OrderByNullType::NULLS_LAST, FreeRow());
		}
		expression.orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST,
		                               calc.Out(calc.Col(LogicalType::DOUBLE, score_binding)));
	}

	//! Stage 4: rank rows within their scope and give each row its group's bounds. Where a group can fail a check of
	//! its own (NeedsVerdict), a window over all the rows reads whether every group is feasible, and the rows leave
	//! through it.
	RankedRows RankRows(unique_ptr<LogicalOperator> score, unique_ptr<LogicalOperator> classes) const {
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
		window->children.push_back(std::move(score));
		result.rank_binding = ColumnBinding(window_index, 0);
		result.plan = std::move(window);
		if (classes) {
			// Each row takes its group's bounds, whose class is the scope: its class key.
			result.plan =
			    DirectJoinClassAnswers(binder, std::move(result.plan), key_bindings, class_key, std::move(classes))
			        .plan;
		}
		if (NeedsVerdict()) {
			auto verdict = make_uniq<LogicalWindow>(verdict_index);
			verdict->expressions.push_back(
			    DirectWindowExtremum(optimizer, "bool_and", calc.Out(GroupsAreFeasible(result, free_count_slot))));
			verdict->children.push_back(std::move(result.plan));
			result.verdict_binding = ColumnBinding(verdict_index, 0);
			result.plan = std::move(verdict);
		}
		return result;
	}

	//! Whether the group of the row is feasible: a list of checks, each true when it passes and false when it fails,
	//! after the ones that raise a message of their own (what the group's bounds give: a NaN limit, an `=` bound that
	//! takes two values in a group). The checks on the rows of a group apply to eligible rows only. The verdict window
	//! reads it over every row, so every group has passed the raising checks when the first row leaves; a false raises
	//! DECIDE's infeasible error then.
	ExprPtr GroupsAreFeasible(const RankedRows &ranked, idx_t free_count_slot) const {
		// `ok` must hold; a NULL `ok` fails.
		auto must_hold = [&](const ExprPtr &ok) {
			return calc.Out(calc.If(ok, calc.Bool(true), calc.Bool(false)));
		};
		// `bad` must not hold; a NULL `bad` passes.
		auto must_not_hold = [&](const ExprPtr &bad) {
			return calc.Out(calc.If(bad, calc.Bool(false), calc.Bool(true)));
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
					group_checks.push_back(
					    must_not_hold(calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, BoundValue(i, false),
					                           calc.Num(DECIDE_FIRST_DOUBLE_BEYOND_BIGINT))));
				}
			}
			if (has_fixes && has_upper) {
				group_checks.push_back(
				    must_hold(calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, fixed_count(), UpperLimit())));
			}
		}
		D_ASSERT(group_checks.empty() != NeedsVerdict());
		// The checks of the rows of a group apply to eligible rows only.
		auto in_group = [&](vector<unique_ptr<Expression>> list) {
			auto group = DirectAllHold(calc, std::move(list));
			return condition.scope.Scoped() ? calc.Out(calc.If(calc.Col(LogicalType::BOOLEAN, eligible_binding),
			                                                   calc.Wrap(std::move(group)), calc.Bool(true)))
			                                : std::move(group);
		};
		// What the group bounds give (a NaN limit, an `=` bound that takes two values in a group) comes before the
		// feasibility check: that one returns false, which stops the chain, and would hide a message after it.
		vector<unique_ptr<Expression>> checks;
		for (auto &values : group_values) {
			for (auto &message : values.messages) {
				checks.push_back(DirectRaiseMessage(optimizer, calc, message));
			}
		}
		if (!group_checks.empty()) {
			checks.push_back(in_group(std::move(group_checks)));
		}
		return calc.Wrap(DirectAllHold(calc, std::move(checks)));
	}

	//! Stage 5: the 0/1 assignment (fixed values, then scope, then upper and lower rank limits, then score sign), the
	//! source columns, the hidden rank that keeps the rank window live, and the column of the checks.
	unique_ptr<LogicalOperator> ProjectAssignment(RankedRows ranked) const {
		auto &rank_binding = ranked.rank_binding;
		auto fixed_count_slot = ranked.fixed_count_slot;
		vector<unique_ptr<Expression>> result_expressions;
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			result_expressions.push_back(calc.Out(calc.Col(source_types[i], ColumnBinding(score_index, i))));
		}
		auto improving =
		    calc.Cmp(ExpressionType::COMPARE_LESSTHAN, calc.Col(LogicalType::DOUBLE, score_binding), calc.Num(0.0));
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
		if (condition.scope.Scoped()) {
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
		// The checks of the rows, which the score projection raised, and the verdict of the groups. The column keeps
		// them live (validated_columns).
		vector<unique_ptr<Expression>> checks;
		if (HasRowChecks()) {
			checks.push_back(calc.Out(calc.Col(LogicalType::BOOLEAN, checks_binding)));
		}
		if (NeedsVerdict()) {
			checks.push_back(DirectRaiseUnless(calc, calc.Out(calc.Col(LogicalType::BOOLEAN, ranked.verdict_binding)),
			                                   DirectInfeasibleErrorPredicate(optimizer, errors)));
		}
		if (!checks.empty()) {
			result_expressions.push_back(DirectAllHold(calc, std::move(checks)));
		}
		auto result = make_uniq<LogicalProjection>(result_index, std::move(result_expressions));
		result->children.push_back(std::move(ranked.plan));
		return std::move(result);
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	unique_ptr<LogicalOperator> classes;
	if (condition.HasGroupBound()) {
		classes = DirectBuildOverRead(reader, reader.ReadSource(), [&](unique_ptr<LogicalOperator> rows) {
			return plan_builder.BuildGroupBounds(std::move(rows));
		});
	}
	return DirectBuildOverOneRead(reader, [&](unique_ptr<LogicalOperator> rows) {
		return plan_builder.Build(std::move(rows), std::move(classes));
	});
}

} // namespace direct_s1_uniform_matroid

unique_ptr<DirectSolveRule> MakeS1UniformMatroidRule() {
	return make_uniq<direct_s1_uniform_matroid::Rule>();
}

} // namespace duckdb
