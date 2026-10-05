#include "duckdb/optimizer/decide/direct/s1_rule.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/decide/direct/direct_builder.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_s1 {

constexpr const char *S1_RULE = "S1_CARDINALITY_INTERVAL";

struct S1Match final : DirectRuleMatch {
	vector<const DirectConstraintFact *> bound_factors;
	vector<const DirectConstraintFact *> local_fixes;
};

struct S1Proof final : DirectRuleProof {
	struct SourceBound {
		idx_t source_clause_id;
		idx_t source_slot;
		LogicalType source_type;
		ExpressionType comparison;
		unique_ptr<Expression> value;
		bool rhs_all_group_rows;
		//! Name of the source column when the bound is exactly one; empty for a computed bound.
		string column_name;
	};
	//! A pin whose value comes from the row: it must be neither NULL nor, for a floating point value, NaN.
	struct SourcePin {
		idx_t source_clause_id;
		unique_ptr<Expression> value;
		string invalid_message;
		bool reject_nan;
	};
	struct ObjectivePart {
		int sign;
		unique_ptr<Expression> coefficient;
	};

	idx_t decide_index;
	idx_t lower = 0;
	idx_t upper = 0;
	bool has_upper = false;
	bool impossible = false;
	DecideSense sense;
	vector<ObjectivePart> objective_parts;
	//! The source columns the score reads, for a NULL score to name; and, for a single-term score, the term as the user
	//! wrote it, which a NULL that no column explains quotes instead.
	vector<DirectNullSource> score_sources;
	string score_text;
	vector<idx_t> group_key_slots;
	unique_ptr<Expression> when_condition;
	vector<unique_ptr<Expression>> fixed_one_conditions;
	vector<unique_ptr<Expression>> fixed_zero_conditions;
	vector<SourceBound> source_bounds;
	vector<SourcePin> source_pins;
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
	if (!OnlyDecisionTerms(terms)) {
		return false;
	}
	double total = 0;
	for (auto &term : terms) {
		double value;
		if (!term.coefficient || !DirectFiniteFoldableDouble(context, *term.coefficient, value)) {
			return false;
		}
		total += term.sign * value;
	}
	return total == 1.0;
}

//! The decision's terms on a per-row left side, as in a pin `x = 1`; null otherwise. Prove checks that they add up
//! to exactly one `x`.
const vector<DecideSplitTerm> *PinTerms(const DirectConstraintFact &fact) {
	if (fact.aggregate || fact.comparison == ExpressionType::COMPARE_IN || fact.lhs.size() != 1) {
		return nullptr;
	}
	auto &part = fact.lhs[0];
	if (part.sign != 1 || part.reducer != DirectReducer::NONE || part.scale || !OnlyDecisionTerms(part.terms)) {
		return nullptr;
	}
	return &part.terms;
}

//! Turns `SUM(x) <comparison> expr` into an inclusive count limit in [0, 2^53]. A bound below zero on an upper
//! side is infeasible; one at or below zero on a lower side is no bound. False when `expr` is not an exactly
//! representable finite constant.
bool NormalizeCapacity(ClientContext &context, const Expression &expr, ExpressionType comparison, idx_t &capacity,
                       bool &impossible) {
	double as_double;
	if (!DirectFiniteFoldableDouble(context, expr, as_double)) {
		return false;
	}
	double inclusive;
	switch (comparison) {
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		inclusive = std::ceil(as_double);
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		if (as_double >= 9007199254740992.0) {
			return false;
		}
		inclusive = std::floor(as_double) + 1.0;
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		inclusive = std::floor(as_double);
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		inclusive = std::ceil(as_double) - 1.0;
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
	if (!std::isfinite(inclusive) || inclusive > 9007199254740992.0) {
		return false;
	}
	capacity = static_cast<idx_t>(inclusive);
	return true;
}

//! Which of the values 0 and 1 a per-row bound on `x` leaves allowed. Neither allowed means no row it covers can
//! satisfy it.
struct PinDomain {
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
		throw InternalException("S1 direct solve received a pin comparison it did not match");
	}
}

//! Reads `x <comparison> bound` as the values of {0, 1} it allows.
//!
//! The solver reads a bound with a WHEN as a per-row constraint and applies plain arithmetic to it. A bound without a
//! WHEN is a bound on the variable itself, and the solver reads some spellings of those differently from arithmetic on
//! a Boolean: a negative lower bound makes `x` signed, so the result can hold -3; a strict bound against a fraction
//! moves by a whole unit (`x > 0.5` becomes `x >= 1.5`); and a bound that cannot hold raises a message naming the
//! clause rather than DECIDE's infeasible error. Those spellings stay on the solver. False for them.
bool ConstantPinDomain(const DirectConstraintFact &fact, double bound, PinDomain &domain) {
	auto comparison = fact.comparison;
	if (!fact.scope.when && comparison != ExpressionType::COMPARE_NOTEQUAL) {
		bool strict =
		    comparison == ExpressionType::COMPARE_GREATERTHAN || comparison == ExpressionType::COMPARE_LESSTHAN;
		if (strict && bound != std::floor(bound)) {
			return false;
		}
		bool has_lower = comparison == ExpressionType::COMPARE_GREATERTHAN ||
		                 comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
		                 comparison == ExpressionType::COMPARE_EQUAL;
		bool has_upper = comparison == ExpressionType::COMPARE_LESSTHAN ||
		                 comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
		                 comparison == ExpressionType::COMPARE_EQUAL;
		double lower = comparison == ExpressionType::COMPARE_GREATERTHAN ? bound + 1.0 : bound;
		double upper = comparison == ExpressionType::COMPARE_LESSTHAN ? bound - 1.0 : bound;
		if ((has_lower && (lower < 0.0 || lower > 1.0)) || (has_upper && upper < 0.0)) {
			return false;
		}
	}
	domain.zero = SatisfiesComparison(comparison, 0.0, bound);
	domain.one = SatisfiesComparison(comparison, 1.0, bound);
	return true;
}

const Expression *SourceBooleanPinValue(const Expression &expr, idx_t decide_index,
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

//! The membership filter of one bound factor: the aggregate-local WHEN on `SUM(x)` or the clause-level WHEN, never
//! both. Null when the bound has none; `ok` is false when the factor is not a plain `SUM(x)` with one filter.
const Expression *BoundMembership(const DirectConstraintFact &fact, bool &ok) {
	auto sum = fact.PlainSum(true);
	ok = sum && !(sum->filter && fact.scope.when);
	if (!ok) {
		return nullptr;
	}
	return sum->filter ? sum->filter.get() : fact.scope.when.get();
}

class S1CardinalityRule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return S1_RULE;
	}
	unique_ptr<DirectRuleMatch> Match(const DirectProblemFacts &facts, string &reason) const override {
		if (facts.decisions_status != DirectFactStatus::KNOWN || facts.decisions.size() != 1 ||
		    facts.decisions[0].domain != DirectDomain::BOOL || facts.decisions[0].scope != DecideVarScope::ROW ||
		    facts.decisions[0].output_type != LogicalType::INTEGER || !facts.entity_scopes.empty() ||
		    facts.source_status != DirectFactStatus::KNOWN) {
			reason = "variable_shape: expected one row-scoped BOOL";
			return nullptr;
		}
		if (!std::isfinite(facts.objective.offset) ||
		    (facts.objective.sense != DecideSense::MAXIMIZE && facts.objective.sense != DecideSense::MINIMIZE)) {
			reason = "problem_shape: expected one linear objective and cardinality bounds";
			return nullptr;
		}
		if (facts.constraints_status != DirectFactStatus::KNOWN) {
			reason = "constraint_facts_unknown: " + facts.constraints_reason;
			return nullptr;
		}
		if (facts.constraints.empty() || facts.source_clause_count != facts.constraints.size()) {
			reason = "constraint_shape: expected attributed cardinality bounds and optional Boolean pins";
			return nullptr;
		}
		auto result = make_uniq<S1Match>();
		vector<uint8_t> seen_sources(facts.source_clause_count, 0);
		for (auto &fact : facts.constraints) {
			if (fact.source_clause_id >= seen_sources.size() || seen_sources[fact.source_clause_id]++) {
				reason = "constraint_provenance: expected one distinct attributed source clause per factor";
				return nullptr;
			}
			if (fact.status != DirectFactStatus::KNOWN) {
				reason = "constraint_facts_unknown: " + facts.FirstUnknownReason();
				return nullptr;
			}
			if (fact.comparison == ExpressionType::COMPARE_IN) {
				reason = "constraint_shape: expected SUM(x) bounds or Boolean pins";
				return nullptr;
			}
			if (PinTerms(fact)) {
				result->local_fixes.push_back(&fact);
				continue;
			}
			if (!DirectIsLowerBound(fact.comparison) && !DirectIsUpperBound(fact.comparison)) {
				reason = "constraint_shape: expected SUM(x) cardinality bounds";
				return nullptr;
			}
			result->bound_factors.push_back(&fact);
		}
		if (result->bound_factors.empty()) {
			reason = "constraint_shape: expected a SUM(x) cardinality bound";
			return nullptr;
		}
		if (facts.objective.status != DirectFactStatus::KNOWN) {
			reason = "objective_facts_unknown: " + facts.objective.reason;
			return nullptr;
		}
		if (facts.objective.scope.when || !facts.objective.scope.per_key_slots.empty()) {
			reason = "objective_scope: expected an objective without WHEN or PER";
			return nullptr;
		}
		if (facts.objective.parts.empty()) {
			reason = "objective_shape: expected additive SUM(coefficient * x) terms";
			return nullptr;
		}
		return std::move(result);
	}
	unique_ptr<DirectRuleProof> Prove(const DirectProblemFacts &facts, const DirectRuleMatch &candidate,
	                                  ClientContext &context, string &reason) const override {
		auto &match = static_cast<const S1Match &>(candidate);
		auto proof = make_uniq<S1Proof>();
		proof->decide_index = facts.decide_index;
		proof->sense = facts.objective.sense;
		if (!ProveScope(facts, match, *proof, reason) || !ProvePins(facts, match, context, *proof, reason) ||
		    !ProveBounds(facts, match, context, *proof, reason) || !ProveObjective(facts, *proof, reason)) {
			return nullptr;
		}
		return std::move(proof);
	}
	double Cost(const DirectRuleProof &, const DirectCostContext &) const override {
		// The first rule has no competing proved alternative yet.
		return 0.0;
	}
	void Explain(const DirectRuleProof &candidate, DirectSolveDecisionRecord &record) const override {
		auto &proof = static_cast<const S1Proof &>(candidate);
		auto has_fixes = !proof.fixed_one_conditions.empty() || !proof.fixed_zero_conditions.empty();
		auto scope = proof.group_key_slots.empty() ? (proof.when_condition ? "WHEN-scoped" : "global")
		                                           : (proof.when_condition ? "grouped WHEN-scoped" : "grouped");
		auto direction = proof.sense == DecideSense::MAXIMIZE ? "maximizing" : "minimizing";
		const char *pin_guard = "; pin conflicts checked";
		if (!proof.source_bounds.empty()) {
			record.proof = StringUtil::Format("one row-scoped Boolean; exact %s source-numeric cardinality "
			                                  "bounds with any constant interval", scope);
			for (auto &bound : proof.source_bounds) {
				if (bound.rhs_all_group_rows) {
					record.proof += "; aggregate-local count filter with full-group RHS reduction";
					break;
				}
			}
			record.guards = "active membership checked; every source bound checked for NULL/NaN before score; "
			                "rank and feasibility retained";
		} else if (proof.impossible) {
			record.proof = StringUtil::Format("one row-scoped Boolean; exact %s cardinality bounds "
			                                  "infeasible for a nonempty eligible group", scope);
			record.guards = "score checked on all rows; active membership checked; rank retained";
		} else {
			auto lower = static_cast<unsigned long long>(proof.lower);
			record.proof = proof.has_upper
			                   ? StringUtil::Format("one row-scoped Boolean; exact %s cardinality interval "
			                                        "[%llu, %llu]; %s linear score", scope, lower,
			                                        static_cast<unsigned long long>(proof.upper), direction)
			                   : StringUtil::Format("one row-scoped Boolean; exact %s lower bound %llu; "
			                                        "%s linear score", scope, lower, direction);
			record.guards = proof.lower ? "score checked on all rows; active count checked; rank retained"
			                            : "score NULL/non-finite checked on all input rows; rank retained";
			pin_guard = "; pin conflicts and residual counts checked";
		}
		if (has_fixes) {
			record.proof += "; source-only Boolean pins";
			record.guards += pin_guard;
		}
	}
	DirectRelationalProposal Rewrite(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
	                                 const DirectRuleProof &candidate) const override;

private:
	bool ProveScope(const DirectProblemFacts &facts, const S1Match &match, S1Proof &proof, string &reason) const;
	bool ProvePins(const DirectProblemFacts &facts, const S1Match &match, ClientContext &context, S1Proof &proof,
	               string &reason) const;
	bool ProveBounds(const DirectProblemFacts &facts, const S1Match &match, ClientContext &context, S1Proof &proof,
	                 string &reason) const;
	bool ProveObjective(const DirectProblemFacts &facts, S1Proof &proof, string &reason) const;
};

//! Every bound counts the same rows: one `SUM(x)` per bound with the same PER keys and the same WHEN membership.
//! Records the group keys and membership the plan scopes by.
bool S1CardinalityRule::ProveScope(const DirectProblemFacts &facts, const S1Match &match, S1Proof &proof,
                                   string &reason) const {
	const auto &first_scope = *match.bound_factors.front();
	bool ok;
	const Expression *first_when = BoundMembership(first_scope, ok);
	if (!ok) {
		reason = "constraint_shape: expected one SUM(x) membership filter";
		return false;
	}
	for (auto factor : match.bound_factors) {
		const Expression *when = BoundMembership(*factor, ok);
		if (!ok) {
			reason = "constraint_shape: expected one SUM(x) membership filter";
			return false;
		}
		if (factor->scope.per_key_slots != first_scope.scope.per_key_slots ||
		    static_cast<bool>(when) != static_cast<bool>(first_when)) {
			reason = "constraint_scope: cardinality bounds use different PER or WHEN membership";
			return false;
		}
		if (when && !Expression::Equals(*when, *first_when)) {
			reason = "constraint_scope: cardinality bounds use different WHEN membership";
			return false;
		}
	}
	proof.group_key_slots = first_scope.scope.per_key_slots;
	if (first_when) {
		if (!DirectIsSourceOnlyPredicate(*first_when, facts.decide_index, facts.source_bindings)) {
			reason = "constraint_scope: WHEN must be a deterministic source-only predicate";
			return false;
		}
		proof.when_condition = first_when->Copy();
	}
	proof.scoped = !proof.group_key_slots.empty() || proof.when_condition != nullptr;
	return true;
}

//! Per-row Boolean pins: `x = 0/1` and its one-sided forms against a constant, or against a source Boolean. Each
//! becomes a condition under which the row is fixed to one or to zero.
bool S1CardinalityRule::ProvePins(const DirectProblemFacts &facts, const S1Match &match, ClientContext &context,
                                  S1Proof &proof, string &reason) const {
	for (auto factor : match.local_fixes) {
		if (!factor->scope.per_key_slots.empty()) {
			reason = "constraint_scope: Boolean pins must be per-row without PER keys";
			return false;
		}
		if (!IsUnitContribution(context, *PinTerms(*factor))) {
			reason = "constraint_shape: expected a Boolean pin on x itself";
			return false;
		}
		auto &when = factor->scope.when;
		if (when && !DirectIsSourceOnlyPredicate(*when, facts.decide_index, facts.source_bindings)) {
			reason = "constraint_scope: Boolean pin WHEN must be deterministic and source-only";
			return false;
		}
		double constant;
		if (DirectFiniteFoldableDouble(context, *factor->rhs, constant)) {
			PinDomain domain;
			if (!ConstantPinDomain(*factor, constant, domain)) {
				reason =
				    "constraint_shape: an unconditional bound on x that the solver reads differently from a Boolean "
				    "(negative lower bound, fractional strict bound, or a bound that cannot hold)";
				return false;
			}
			// Both values allowed is no constraint. Neither allowed fixes the row to both, which the plan reports as
			// DECIDE's infeasible error.
			if (!domain.zero || !domain.one) {
				auto condition = [&]() {
					return when ? when->Copy() : make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
				};
				if (!domain.zero) {
					proof.fixed_one_conditions.push_back(condition());
				}
				if (!domain.one) {
					proof.fixed_zero_conditions.push_back(condition());
				}
			}
			continue;
		}
		auto type = factor->comparison;
		auto active = [&](unique_ptr<Expression> value) -> unique_ptr<Expression> {
			return when ? make_uniq<BoundCaseExpression>(when->Copy(), std::move(value),
			                                             make_uniq<BoundConstantExpression>(Value::BOOLEAN(false)))
			            : std::move(value);
		};
		auto source_value = SourceBooleanPinValue(*factor->rhs, facts.decide_index, facts.source_bindings);
		if (source_value &&
		    (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
		     type == ExpressionType::COMPARE_GREATERTHANOREQUALTO)) {
			if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
				proof.fixed_one_conditions.push_back(active(source_value->Copy()));
			}
			if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
				proof.fixed_zero_conditions.push_back(active(
				    make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, source_value->Copy(),
				                                         make_uniq<BoundConstantExpression>(Value::BOOLEAN(false)))));
			}
			proof.source_pins.push_back(
			    {factor->source_clause_id, source_value->Copy(), "DECIDE per-row Boolean bound contains NULL", false});
			continue;
		}
		// A numeric value from the row: the row allows 0 when `0 <comparison> value` holds and 1 when `1 <comparison>
		// value` holds, as the solver reads it (with a WHEN or not), so the row is fixed to the one value it allows
		// and is infeasible when it allows neither.
		auto &value = *factor->rhs;
		if (value.IsFoldable() || !DirectIsSourceOnlyNumeric(value, facts.decide_index, facts.source_bindings)) {
			reason = "constraint_shape: a bound on x must be a finite constant, a source column, or a deterministic "
			         "nonthrowing numeric expression over source columns";
			return false;
		}
		idx_t column_slot;
		LogicalType column_type;
		string column_name;
		bool is_column = DirectSourceNumericColumn(value, facts.source_bindings, column_slot, column_type, column_name);
		auto invalid_type = is_column ? column_type : value.return_type;
		auto disallows = [&](double allowed_value) -> unique_ptr<Expression> {
			auto as_double = BoundCastExpression::AddCastToType(context, value.Copy(), LogicalType::DOUBLE);
			auto allowed = make_uniq<BoundComparisonExpression>(
			    type, make_uniq<BoundConstantExpression>(Value::DOUBLE(allowed_value)), std::move(as_double));
			return make_uniq<BoundCaseExpression>(std::move(allowed), DirectConstantBool(false),
			                                      DirectConstantBool(true));
		};
		proof.fixed_one_conditions.push_back(active(disallows(0.0)));
		proof.fixed_zero_conditions.push_back(active(disallows(1.0)));
		proof.source_pins.push_back({factor->source_clause_id, value.Copy(),
		                             DirectInvalidBoundMessage(is_column ? column_name : string(), invalid_type),
		                             invalid_type == LogicalType::FLOAT || invalid_type == LogicalType::DOUBLE});
	}
	return true;
}

//! The count bounds: constant ones fold into one inclusive interval, source-valued ones are kept for the plan to
//! reduce per group. Every bound counts `x` with a unit contribution.
bool S1CardinalityRule::ProveBounds(const DirectProblemFacts &facts, const S1Match &match, ClientContext &context,
                                    S1Proof &proof, string &reason) const {
	auto fold_bound = [&](const Expression &bound, ExpressionType side, idx_t &target) {
		bool impossible = false;
		if (!NormalizeCapacity(context, bound, side, target, impossible)) {
			reason = "constraint_shape: cardinality bound must be a finite consistent foldable numeric expression "
			         "with an inclusive limit in [0, 2^53], or a deterministic nonthrowing source-only "
			         "numeric expression";
			return false;
		}
		proof.impossible |= impossible;
		return true;
	};
	for (auto factor : match.bound_factors) {
		auto sum = factor->PlainSum(true);
		if (!sum || !IsUnitContribution(context, sum->terms)) {
			reason = "constraint_shape: expected SUM(x) with unit contribution";
			return false;
		}
		auto &bound = *factor->rhs;
		auto type = factor->comparison;
		idx_t source_slot = DConstants::INVALID_INDEX;
		LogicalType source_type;
		string source_name;
		bool source_column = DirectSourceNumericColumn(bound, facts.source_bindings, source_slot, source_type,
		                                              source_name);
		if (!bound.IsFoldable() &&
		    (source_column || DirectIsSourceOnlyNumeric(bound, facts.decide_index, facts.source_bindings))) {
			proof.source_bounds.push_back({factor->source_clause_id, source_slot,
			                               source_column ? source_type : bound.return_type, type, bound.Copy(),
			                               sum->filter != nullptr, source_column ? source_name : string()});
			continue;
		}
		if (DirectIsLowerBound(type)) {
			idx_t lower;
			if (!fold_bound(bound,
			                type == ExpressionType::COMPARE_EQUAL ? ExpressionType::COMPARE_GREATERTHANOREQUALTO : type,
			                lower)) {
				return false;
			}
			proof.lower = std::max(proof.lower, lower);
		}
		if (DirectIsUpperBound(type)) {
			idx_t upper;
			if (!fold_bound(bound,
			                type == ExpressionType::COMPARE_EQUAL ? ExpressionType::COMPARE_LESSTHANOREQUALTO : type,
			                upper)) {
				return false;
			}
			proof.upper = proof.has_upper ? std::min(proof.upper, upper) : upper;
			proof.has_upper = true;
		}
	}
	if (proof.has_upper && proof.lower > proof.upper) {
		proof.impossible = true;
	}
	return true;
}

//! The objective: a signed sum of `SUM(coefficient * x)` terms. Each coefficient must be evaluable per source row.
bool S1CardinalityRule::ProveObjective(const DirectProblemFacts &facts, S1Proof &proof, string &reason) const {
	// Every linear term in x scores separately: `SUM((p + q) * x)` splits into `p * x` and `q * x`, and the score
	// adds the signed coefficients in order, as the solver's like-term collection does.
	auto &parts = facts.objective.parts;
	idx_t term_count = 0;
	for (auto &part : parts) {
		if (part.reducer != DirectReducer::SUM || part.filter || part.scale || part.inner ||
		    part.qualifier != DConstants::INVALID_INDEX) {
			reason = "objective_shape: expected unfiltered SUM(coefficient * x) terms";
			return false;
		}
		for (auto &term : part.terms) {
			if (term.kind != DecideTermKind::LINEAR || term.var_a != 0) {
				reason = "objective_shape: expected a coefficient times x";
				return false;
			}
		}
		term_count += part.terms.size();
	}
	for (auto &part : parts) {
		for (auto &term : part.terms) {
			if (!DirectIsNumericDecisionFree(*term.coefficient, facts.decide_index)) {
				reason = "coefficient_shape: expected a deterministic numeric expression without decisions";
				return false;
			}
			if (!DirectReferencesOnlySource(*term.coefficient, facts.source_bindings) ||
			    (term_count > 1 && DirectMayThrow(*term.coefficient))) {
				reason = "coefficient_shape: multiple terms need nonthrowing source-only numeric coefficients";
				return false;
			}
			proof.objective_parts.push_back({part.sign * term.sign, term.coefficient->Copy()});
		}
	}
	// The score is NULL when any term is, so a NULL score names whichever of these columns is NULL on that row. With
	// several terms the solver quotes the failing term; there is no single term to quote here, so the message stays
	// generic when no column is NULL.
	for (auto &part : proof.objective_parts) {
		DirectCollectNullSources(*part.coefficient, facts.source_bindings, proof.score_sources);
	}
	if (proof.objective_parts.size() == 1) {
		proof.score_text = proof.objective_parts[0].coefficient->ToString();
	}
	return true;
}

//! Builds the relational plan for one proved S1 problem. The constructor derives what every stage shares; each stage
//! takes the plan built so far and returns it extended. Data a later stage needs travels in explicit structs.
//!
//!   ProjectScopeState -> GuardEmptyAggregate -> ValidateBounds -> ProjectScore
//!     -> RankAndFilterFeasibility -> ProjectAssignment
class S1Rewriter {
public:
	S1Rewriter(const S1Proof &proof_p, Optimizer &optimizer_p, LogicalOperator &source)
	    : proof(proof_p), optimizer(optimizer_p), binder(optimizer_p.binder) {
		// The coordinator hands the input over with its types resolved.
		source_bindings = source.GetColumnBindings();
		source_types = source.types;
		if (source_bindings.size() != source_types.size()) {
			throw InternalException("Direct solve source bindings and types differ");
		}
		score_index = binder.GenerateTableIndex();
		window_index = binder.GenerateTableIndex();
		result_index = binder.GenerateTableIndex();
		has_fixes = !proof.fixed_one_conditions.empty() || !proof.fixed_zero_conditions.empty();
		for (auto &bound : proof.source_bounds) {
			source_lower |= DirectIsLowerBound(bound.comparison);
			source_upper |= DirectIsUpperBound(bound.comparison);
		}
		has_lower = proof.lower || source_lower;
		has_upper = proof.has_upper || source_upper;
		// Slots of the score projection, which every later stage reads. This is the one place that lays them out;
		// ProjectScore appends its columns in this order and checks each against it.
		idx_t next_slot = source_bindings.size();
		score_binding = ColumnBinding(score_index, next_slot++);
		if (proof.scoped) {
			eligible_binding = ColumnBinding(score_index, next_slot++);
		}
		if (has_fixes) {
			fixed_one_binding = ColumnBinding(score_index, next_slot++);
			fixed_zero_binding = ColumnBinding(score_index, next_slot++);
		}
	}

	DirectRelationalProposal Build(unique_ptr<LogicalOperator> source) const {
		ScopeState scope;
		source = ProjectScopeState(std::move(source), scope);
		source = GuardEmptyAggregate(std::move(source), scope);
		BoundChecks bounds(proof.source_bounds.size());
		source = ValidateBounds(std::move(source), scope, bounds);
		source = ProjectScore(std::move(source), scope, bounds);
		auto ranked = RankAndFilterFeasibility(std::move(source), bounds);
		auto result = ProjectAssignment(std::move(ranked), bounds);

		DirectRelationalProposal proposal;
		proposal.prunable_decisions.push_back(1); // x is pure once score validation and ranking remain live
		// The rank window reads every row before it emits one, so the hidden rank is this plan's validation barrier.
		proposal.validation_slots.push_back(source_bindings.size() + 1);
		for (idx_t i = 0; i < source_bindings.size() + 1; i++) {
			proposal.output_slots.push_back(i);
		}
		proposal.child = std::move(result);
		return proposal;
	}

private:
	//! Where the source columns and per-row flags live once the optional state projection has run.
	struct ScopeState {
		vector<ColumnBinding> input_bindings;
		idx_t state_index = DConstants::INVALID_INDEX;
		ColumnBinding eligible_binding;
		ColumnBinding fixed_one_binding;
		ColumnBinding fixed_zero_binding;
	};

	//! Window slots of one source-valued count bound.
	struct SourceBoundSlots {
		idx_t min_window = DConstants::INVALID_INDEX;
		idx_t max_window = DConstants::INVALID_INDEX;
		//! Slots of the same extrema once the score projection forwards them.
		idx_t min_score = DConstants::INVALID_INDEX;
		idx_t max_score = DConstants::INVALID_INDEX;
	};

	//! The window the source-valued bounds were validated in, and the slots later stages read from it.
	struct BoundChecks {
		explicit BoundChecks(idx_t bound_count) : slots(bound_count) {
		}
		idx_t window_index = DConstants::INVALID_INDEX;
		vector<SourceBoundSlots> slots;
	};

	//! Ranked, feasibility-checked rows and the window slots the assignment reads.
	struct RankedRows {
		unique_ptr<LogicalOperator> plan;
		ColumnBinding rank_binding;
		idx_t fixed_count_slot = DConstants::INVALID_INDEX;
	};

	const S1Proof &proof;
	Optimizer &optimizer;
	Binder &binder;
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

	//! Copy of a proved expression whose source references follow the state projection, when there is one.
	unique_ptr<Expression> CopyIntoState(const Expression &expr, const ScopeState &scope, const char *what) const {
		auto value = expr.Copy();
		if (scope.state_index != DConstants::INVALID_INDEX &&
		    !DirectRemapSourceReferences(*value, source_bindings, scope.state_index)) {
			throw InternalException(string("S1 direct solve could not remap its proved ") + what);
		}
		return value;
	}

	//! Stage 1: pass the source through and append the per-row scope flags (group eligibility, pins).
	unique_ptr<LogicalOperator> ProjectScopeState(unique_ptr<LogicalOperator> source, ScopeState &scope) const {
		vector<unique_ptr<Expression>> pins;
		if (has_fixes) {
			pins.push_back(DirectAnyCondition(proof.fixed_one_conditions));
			pins.push_back(DirectAnyCondition(proof.fixed_zero_conditions));
		}
		auto state = DirectProjectScope(binder, std::move(source), proof.scoped, proof.when_condition.get(),
		                                proof.group_key_slots, std::move(pins));
		scope.input_bindings = std::move(state.input_bindings);
		scope.state_index = state.state_index;
		scope.eligible_binding = state.eligible;
		if (has_fixes) {
			scope.fixed_one_binding = state.extra[0];
			scope.fixed_zero_binding = state.extra[1];
		}
		return std::move(state.plan);
	}

	//! Stage 2: a scoped aggregate over no eligible row is DECIDE's empty-aggregate error.
	unique_ptr<LogicalOperator> GuardEmptyAggregate(unique_ptr<LogicalOperator> source, const ScopeState &scope) const {
		if (!proof.scoped) {
			return source;
		}
		return DirectGuardEmptyAggregate(optimizer, std::move(source), scope.eligible_binding);
	}

	//! Stage 3: validate source-valued bounds and pins on every row, in source-clause order, and reduce each bound
	//! to its group extremum.
	unique_ptr<LogicalOperator> ValidateBounds(unique_ptr<LogicalOperator> source, const ScopeState &scope,
	                                           BoundChecks &bounds) const {
		vector<DirectBoundSpec> specs;
		for (auto &bound : proof.source_bounds) {
			if (!bound.value || (bound.source_slot != DConstants::INVALID_INDEX &&
			                     (bound.source_slot >= source_types.size() ||
			                      source_types[bound.source_slot] != bound.source_type))) {
				throw InternalException("S1 direct solve received an invalid source bound slot");
			}
			DirectBoundSpec spec;
			spec.source_clause_id = bound.source_clause_id;
			spec.comparison = bound.comparison;
			spec.value = CopyIntoState(*bound.value, scope, "bound");
			if (bound.source_slot != DConstants::INVALID_INDEX) {
				spec.is_column = true;
				spec.column = scope.input_bindings[bound.source_slot];
				spec.column_type = bound.source_type;
			}
			spec.all_group_rows = bound.rhs_all_group_rows;
			spec.invalid_message = DirectInvalidBoundMessage(
			    bound.column_name,
			    bound.source_slot == DConstants::INVALID_INDEX ? bound.value->return_type : bound.source_type);
			specs.push_back(std::move(spec));
		}
		vector<DirectNotNullSpec> pins;
		for (auto &pin : proof.source_pins) {
			pins.push_back({pin.source_clause_id, CopyIntoState(*pin.value, scope, "per-row pin"), pin.invalid_message,
			                pin.reject_nan});
		}
		vector<ColumnBinding> keys;
		vector<LogicalType> key_types;
		for (auto slot : proof.group_key_slots) {
			keys.push_back(scope.input_bindings[slot]);
			key_types.push_back(source_types[slot]);
		}
		auto validation = DirectValidateBounds(optimizer, std::move(source), specs, pins,
		                                       proof.scoped ? &scope.eligible_binding : nullptr, keys, key_types);
		bounds.window_index = validation.window_index;
		for (idx_t i = 0; i < bounds.slots.size(); i++) {
			bounds.slots[i].min_window = validation.min_slots[i];
			bounds.slots[i].max_window = validation.max_slots[i];
		}
		return std::move(validation.plan);
	}

	//! Stage 4: the score projection. It forwards the source columns, the combined linear score, the scope and pin
	//! flags, and the per-group bound extrema, so every later stage reads one projection. The constructor laid out
	//! the score and flag slots; each is appended here at exactly that position.
	unique_ptr<LogicalOperator> ProjectScore(unique_ptr<LogicalOperator> source, const ScopeState &scope,
	                                         BoundChecks &bounds) const {
		vector<unique_ptr<Expression>> score_expressions;
		auto append_at = [&](const ColumnBinding &slot, unique_ptr<Expression> expression) {
			if (slot.table_index != score_index || slot.column_index != score_expressions.size()) {
				throw InternalException("S1 direct solve score projection does not match its layout");
			}
			score_expressions.push_back(std::move(expression));
		};
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			score_expressions.push_back(DirectColumn(source_types[i], scope.input_bindings[i]));
		}
		unique_ptr<Expression> combined_score;
		if (proof.objective_parts.size() > 1 || proof.objective_parts[0].sign < 0) {
			combined_score = make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0));
		}
		for (auto &part : proof.objective_parts) {
			auto coefficient = CopyIntoState(*part.coefficient, scope, "coefficient");
			auto term =
			    BoundCastExpression::AddCastToType(optimizer.context, std::move(coefficient), LogicalType::DOUBLE);
			if (part.sign < 0) {
				term = optimizer.BindScalarFunction("*", std::move(term),
				                                    make_uniq<BoundConstantExpression>(Value::DOUBLE(-1.0)));
			}
			combined_score = combined_score ? optimizer.BindScalarFunction("+", std::move(combined_score), std::move(term))
			                                : std::move(term);
		}
		append_at(score_binding, std::move(combined_score));
		if (proof.scoped) {
			append_at(eligible_binding, DirectColumn(LogicalType::BOOLEAN, scope.eligible_binding));
		}
		if (has_fixes) {
			append_at(fixed_one_binding, DirectColumn(LogicalType::BOOLEAN, scope.fixed_one_binding));
			append_at(fixed_zero_binding, DirectColumn(LogicalType::BOOLEAN, scope.fixed_zero_binding));
		}
		for (idx_t i = 0; i < bounds.slots.size(); i++) {
			auto &slots = bounds.slots[i];
			if (slots.min_window != DConstants::INVALID_INDEX) {
				slots.min_score = score_expressions.size();
				score_expressions.push_back(
				    DirectColumn(LogicalType::DOUBLE, ColumnBinding(bounds.window_index, slots.min_window)));
			}
			if (slots.max_window != DConstants::INVALID_INDEX) {
				slots.max_score = score_expressions.size();
				score_expressions.push_back(
				    DirectColumn(LogicalType::DOUBLE, ColumnBinding(bounds.window_index, slots.max_window)));
			}
		}
		auto score = make_uniq<LogicalProjection>(score_index, std::move(score_expressions));
		score->children.push_back(std::move(source));
		return std::move(score);
	}

	//! Inclusive count limit of one source-valued bound, as BIGINT: its group extremum rounded, clamped to the
	//! BIGINT range, and -1 (upper) or 0 (lower) when the value is below zero.
	unique_ptr<Expression> SourceLimit(idx_t index, bool upper, const BoundChecks &bounds) const {
		auto &bound = proof.source_bounds[index];
		auto &slots = bounds.slots[index];
		auto slot = upper ? slots.min_score : slots.max_score;
		if (slot == DConstants::INVALID_INDEX) {
			throw InternalException("S1 direct solve lost its source-valued count limit");
		}
		auto cap = [&]() { return DirectColumn(LogicalType::DOUBLE, ColumnBinding(score_index, slot)); };
		bool strict =
		    bound.comparison == (upper ? ExpressionType::COMPARE_LESSTHAN : ExpressionType::COMPARE_GREATERTHAN);
		auto rounded = optimizer.BindScalarFunction(upper == strict ? "ceil" : "floor", cap());
		unique_ptr<Expression> inclusive =
		    BoundCastExpression::AddCastToType(optimizer.context, std::move(rounded), LogicalType::BIGINT);
		if (strict) {
			inclusive = optimizer.BindScalarFunction(upper ? "-" : "+", std::move(inclusive),
			                                         make_uniq<BoundConstantExpression>(Value::BIGINT(1)));
		}
		auto beyond_bigint = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHANOREQUALTO, cap(),
		    make_uniq<BoundConstantExpression>(Value::DOUBLE(9223372036854775808.0)));
		auto bounded = make_uniq<BoundCaseExpression>(
		    std::move(beyond_bigint),
		    make_uniq<BoundConstantExpression>(Value::BIGINT(std::numeric_limits<int64_t>::max())),
		    std::move(inclusive));
		auto below_zero = make_uniq<BoundComparisonExpression>(
		    strict == upper ? ExpressionType::COMPARE_LESSTHANOREQUALTO : ExpressionType::COMPARE_LESSTHAN, cap(),
		    make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
		return make_uniq<BoundCaseExpression>(std::move(below_zero),
		                                      make_uniq<BoundConstantExpression>(Value::BIGINT(upper ? -1 : 0)),
		                                      std::move(bounded));
	}

	//! Largest lower limit across the constant and source-valued bounds.
	unique_ptr<Expression> LowerLimit(const BoundChecks &bounds) const {
		vector<unique_ptr<Expression>> limits;
		limits.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(proof.lower))));
		for (idx_t i = 0; i < bounds.slots.size(); i++) {
			if (bounds.slots[i].max_score != DConstants::INVALID_INDEX) {
				limits.push_back(SourceLimit(i, false, bounds));
			}
		}
		return limits.size() == 1 ? std::move(limits[0]) : optimizer.BindScalarFunction("greatest", std::move(limits));
	}

	//! Smallest upper limit across the constant and source-valued bounds; null when there is none.
	unique_ptr<Expression> UpperLimit(const BoundChecks &bounds) const {
		vector<unique_ptr<Expression>> limits;
		if (proof.has_upper) {
			limits.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(proof.upper))));
		}
		for (idx_t i = 0; i < bounds.slots.size(); i++) {
			if (bounds.slots[i].min_score != DConstants::INVALID_INDEX) {
				limits.push_back(SourceLimit(i, true, bounds));
			}
		}
		if (limits.empty()) {
			return nullptr;
		}
		return limits.size() == 1 ? std::move(limits[0]) : optimizer.BindScalarFunction("least", std::move(limits));
	}

	//! Partition a rank or count window by eligibility and PER key.
	void AddScope(BoundWindowExpression &expression) const {
		if (!proof.scoped) {
			return;
		}
		expression.partitions.push_back(DirectColumn(LogicalType::BOOLEAN, eligible_binding));
		for (auto slot : proof.group_key_slots) {
			expression.partitions.push_back(DirectColumn(source_types[slot], ColumnBinding(score_index, slot)));
		}
	}

	//! A row no pin fixes to either value.
	unique_ptr<Expression> FreeRow() const {
		return make_uniq<BoundCaseExpression>(
		    DirectColumn(LogicalType::BOOLEAN, fixed_one_binding), DirectConstantBool(false),
		    make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, fixed_zero_binding),
		                                   DirectConstantBool(false), DirectConstantBool(true)));
	}

	//! Order a window: free rows first when pins exist, then best score first.
	void AddOrder(BoundWindowExpression &expression) const {
		if (has_fixes) {
			expression.orders.emplace_back(OrderType::DESCENDING, OrderByNullType::NULLS_LAST, FreeRow());
		}
		expression.orders.emplace_back(
		    proof.sense == DecideSense::MAXIMIZE ? OrderType::DESCENDING : OrderType::ASCENDING,
		    OrderByNullType::NULLS_LAST, DirectColumn(LogicalType::DOUBLE, score_binding));
	}

	//! Stage 5: validate the score, rank rows within their scope, and raise the infeasibility error when the
	//! bounds, pins, and row counts cannot all hold.
	RankedRows RankAndFilterFeasibility(unique_ptr<LogicalOperator> score, const BoundChecks &bounds) const {
		// The score projection forwards every source column at its own slot, so a source column is read from there.
		vector<DirectNullColumn> null_columns;
		for (auto &source : proof.score_sources) {
			null_columns.push_back(
			    {ColumnBinding(score_index, source.slot), source_types[source.slot], source.name});
		}
		auto guard = make_uniq<LogicalFilter>(
		    DirectValidScorePredicate(optimizer, LogicalType::DOUBLE, score_binding, null_columns, proof.score_text));
		guard->children.push_back(std::move(score));
		auto rank = make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr,
		                                             nullptr);
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
			auto count = DirectWindowMatchingCount(DirectColumn(LogicalType::BOOLEAN, fixed_one_binding));
			AddOrder(*count);
			AddScope(*count);
			window->expressions.push_back(std::move(count));
			if (has_lower && !proof.impossible) {
				free_count_slot = window->expressions.size();
				auto free_count = DirectWindowMatchingCount(FreeRow());
				AddOrder(*free_count);
				AddScope(*free_count);
				window->expressions.push_back(std::move(free_count));
			}
		} else if (has_lower && !proof.impossible) {
			// Match the rank's order so the full-partition count can share its sort.
			auto count = make_uniq<BoundWindowExpression>(
			    ExpressionType::WINDOW_AGGREGATE, LogicalType::BIGINT,
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
		if (proof.impossible || has_lower || has_fixes || source_upper) {
			auto feasible = make_uniq<LogicalFilter>(FeasiblePredicate(result, free_count_slot, bounds));
			feasible->children.push_back(std::move(result.plan));
			result.plan = std::move(feasible);
		}
		return result;
	}

	//! The nested predicate of stage 5. Checks wrap outward, so the outermost one (a pin conflict) fires first.
	unique_ptr<Expression> FeasiblePredicate(const RankedRows &ranked, idx_t free_count_slot,
	                                         const BoundChecks &bounds) const {
		auto infeasible_error = [&]() {
			return DirectErrorPredicate(optimizer, "DECIDE optimization is infeasible. Prefix the query with DIAGNOSE "
			                                       "to see which clause to change.");
		};
		auto &rank_binding = ranked.rank_binding;
		auto fixed_count_slot = ranked.fixed_count_slot;
		unique_ptr<Expression> feasible_predicate = DirectConstantBool(true);
		if (proof.impossible) {
			auto has_row = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_GREATERTHANOREQUALTO, DirectColumn(LogicalType::BIGINT, rank_binding),
			    make_uniq<BoundConstantExpression>(Value::BIGINT(1)));
			feasible_predicate =
			    make_uniq<BoundCaseExpression>(std::move(has_row), infeasible_error(), DirectConstantBool(true));
		} else if (has_lower) {
			unique_ptr<Expression> available_rows =
			    has_fixes
			        ? optimizer.BindScalarFunction(
			              "+", DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, fixed_count_slot)),
			              DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, free_count_slot)))
			        : DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, 1));
			auto enough_rows = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
			                                                        std::move(available_rows), LowerLimit(bounds));
			feasible_predicate =
			    make_uniq<BoundCaseExpression>(std::move(enough_rows), DirectConstantBool(true), infeasible_error());
		}
		if (!proof.impossible && (source_lower || source_upper) && has_upper) {
			auto interval_valid = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO,
			                                                           LowerLimit(bounds), UpperLimit(bounds));
			feasible_predicate = make_uniq<BoundCaseExpression>(std::move(interval_valid),
			                                                    std::move(feasible_predicate), infeasible_error());
		}
		if (!proof.impossible && source_lower) {
			for (auto &slots : bounds.slots) {
				if (slots.max_score == DConstants::INVALID_INDEX) {
					continue;
				}
				auto too_large = make_uniq<BoundComparisonExpression>(
				    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
				    DirectColumn(LogicalType::DOUBLE, ColumnBinding(score_index, slots.max_score)),
				    make_uniq<BoundConstantExpression>(Value::DOUBLE(9223372036854775808.0)));
				feasible_predicate = make_uniq<BoundCaseExpression>(std::move(too_large), infeasible_error(),
				                                                    std::move(feasible_predicate));
			}
		}
		if (!proof.impossible && has_fixes && has_upper) {
			auto fixed_within_upper = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_LESSTHANOREQUALTO,
			    DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, fixed_count_slot)), UpperLimit(bounds));
			feasible_predicate = make_uniq<BoundCaseExpression>(std::move(fixed_within_upper),
			                                                    std::move(feasible_predicate), infeasible_error());
		}
		if (proof.scoped) {
			feasible_predicate = make_uniq<BoundCaseExpression>(
			    DirectColumn(LogicalType::BOOLEAN, eligible_binding), std::move(feasible_predicate),
			    DirectConstantBool(true));
		}
		if (has_fixes) {
			auto conflict = make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, fixed_one_binding),
			                                               DirectColumn(LogicalType::BOOLEAN, fixed_zero_binding),
			                                               DirectConstantBool(false));
			feasible_predicate =
			    make_uniq<BoundCaseExpression>(std::move(conflict), infeasible_error(), std::move(feasible_predicate));
		}
		return feasible_predicate;
	}

	//! Stage 6: the 0/1 assignment (pins, then scope, then upper and lower rank limits, then score sign), the
	//! source columns, and the hidden rank that keeps score validation live.
	unique_ptr<LogicalOperator> ProjectAssignment(RankedRows ranked, const BoundChecks &bounds) const {
		auto &rank_binding = ranked.rank_binding;
		auto fixed_count_slot = ranked.fixed_count_slot;
		vector<unique_ptr<Expression>> result_expressions;
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			result_expressions.push_back(DirectColumn(source_types[i], ColumnBinding(score_index, i)));
		}
		auto improving = make_uniq<BoundComparisonExpression>(
		    proof.sense == DecideSense::MAXIMIZE ? ExpressionType::COMPARE_GREATERTHAN
		                                         : ExpressionType::COMPARE_LESSTHAN,
		    DirectColumn(LogicalType::DOUBLE, score_binding), make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
		auto free_choose = improving->Copy();
		unique_ptr<Expression> choose = std::move(improving);
		if (has_lower) {
			unique_ptr<Expression> remaining_lower = LowerLimit(bounds);
			if (has_fixes) {
				remaining_lower = optimizer.BindScalarFunction(
				    "-", std::move(remaining_lower),
				    DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, fixed_count_slot)));
			}
			auto within_lower = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO,
			                                                         DirectColumn(LogicalType::BIGINT, rank_binding),
			                                                         std::move(remaining_lower));
			choose =
			    make_uniq<BoundCaseExpression>(std::move(within_lower), DirectConstantBool(true), std::move(choose));
		}
		if (has_upper) {
			unique_ptr<Expression> remaining_upper = UpperLimit(bounds);
			if (has_fixes) {
				remaining_upper = optimizer.BindScalarFunction(
				    "-", std::move(remaining_upper),
				    DirectColumn(LogicalType::BIGINT, ColumnBinding(window_index, fixed_count_slot)));
			}
			auto within_upper = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO,
			                                                         DirectColumn(LogicalType::BIGINT, rank_binding),
			                                                         std::move(remaining_upper));
			choose =
			    make_uniq<BoundCaseExpression>(std::move(within_upper), std::move(choose), DirectConstantBool(false));
		}
		if (proof.scoped) {
			choose = make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, eligible_binding),
			                                        std::move(choose), std::move(free_choose));
		}
		if (has_fixes) {
			choose = make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, fixed_zero_binding),
			                                        DirectConstantBool(false), std::move(choose));
			choose = make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, fixed_one_binding),
			                                        DirectConstantBool(true), std::move(choose));
		}
		result_expressions.push_back(make_uniq<BoundCaseExpression>(
		    std::move(choose), make_uniq<BoundConstantExpression>(Value::INTEGER(1)),
		    make_uniq<BoundConstantExpression>(Value::INTEGER(0))));
		// The otherwise unused rank is a blocking dependency. It forces a complete
		// value read before an outer LIMIT, COUNT, or column-pruning parent can return.
		result_expressions.push_back(DirectColumn(LogicalType::BIGINT, rank_binding));
		auto result = make_uniq<LogicalProjection>(result_index, std::move(result_expressions));
		result->children.push_back(std::move(ranked.plan));
		return std::move(result);
	}
};

DirectRelationalProposal S1CardinalityRule::Rewrite(unique_ptr<LogicalOperator> source, Optimizer &optimizer,
                                                    const DirectRuleProof &candidate) const {
	auto &proof = static_cast<const S1Proof &>(candidate);
	if (proof.objective_parts.empty()) {
		throw InternalException("S1 direct solve received a proof without an objective");
	}
	S1Rewriter rewriter(proof, optimizer, *source);
	return rewriter.Build(std::move(source));
}

} // namespace direct_s1

unique_ptr<DirectSolveRule> MakeS1CardinalityRule() {
	return make_uniq<direct_s1::S1CardinalityRule>();
}

} // namespace duckdb
