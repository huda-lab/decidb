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
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_s1 {

constexpr const char *S1_RULE = "S1_CARDINALITY_INTERVAL";

struct S1Match final : DirectRuleMatch {
	vector<const DirectConstraintFactor *> bound_factors;
	vector<const DirectConstraintFactor *> local_fixes;
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
	struct SourcePin {
		idx_t source_clause_id;
		unique_ptr<Expression> value;
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
	//! Name of the source column the score is exactly (one term, a bare column); empty for a computed score.
	string score_column;
	vector<idx_t> group_key_slots;
	unique_ptr<Expression> when_condition;
	vector<unique_ptr<Expression>> fixed_one_conditions;
	vector<unique_ptr<Expression>> fixed_zero_conditions;
	vector<SourceBound> source_bounds;
	vector<SourcePin> source_pins;
	bool scoped = false;
};

//! A `norm(e, p)` is bound as a `sum` aggregate whose alias names the norm, so the function name alone does not
//! make an aggregate a sum.
const BoundAggregateExpression *PlainSum(const Expression &expr, idx_t decide_index, bool allow_filter = false) {
	auto root = UnwrapDecideCasts(expr, decide_index);
	if (root->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return nullptr;
	}
	auto &aggregate = root->Cast<BoundAggregateExpression>();
	if (HasDecideTag(aggregate.GetAlias(), NORM_MARKER_TAG_PREFIX)) {
		return nullptr;
	}
	if (StringUtil::Lower(aggregate.function.name) != "sum" || aggregate.children.size() != 1 ||
	    (!allow_filter && aggregate.filter) || aggregate.order_bys || aggregate.IsDistinct()) {
		return nullptr;
	}
	return &aggregate;
}

bool IsExactlyVariable(const Expression &expr, idx_t decide_index) {
	auto variable = GetBareDecideColumnRef(expr, decide_index);
	return variable && variable->binding.column_index == 0 && variable->depth == 0;
}

bool SafeCoefficient(const Expression &expr, idx_t decide_index) {
	return expr.return_type.IsNumeric() && DirectIsDecisionFreeDeterministic(expr, decide_index);
}

//! A source column used exactly as a count bound: its slot in the source, its type, and the name the user wrote.
bool SourceNumericColumnBound(const Expression &expr, const vector<ColumnBinding> &source_bindings, idx_t &slot,
                              LogicalType &type, string &name) {
	auto ref = DirectBareNumericColumn(expr);
	if (!ref) {
		return false;
	}
	auto found = std::find(source_bindings.begin(), source_bindings.end(), ref->binding);
	if (found == source_bindings.end()) {
		return false;
	}
	slot = found - source_bindings.begin();
	type = ref->return_type;
	name = ref->GetAlias();
	return true;
}

bool SourceNumericExpressionBound(const Expression &expr, idx_t decide_index,
                                  const vector<ColumnBinding> &source_bindings) {
	return expr.return_type.IsNumeric() && !DirectMayThrow(expr) &&
	       DirectIsDecisionFreeDeterministic(expr, decide_index) && DirectReferencesOnlySource(expr, source_bindings) &&
	       DirectHasColumnReference(expr);
}

bool IsUnitDecisionTerm(ClientContext &context, const Expression &expr, idx_t decide_index) {
	auto root = UnwrapDecideCasts(expr, decide_index);
	if (IsExactlyVariable(*root, decide_index)) {
		return true;
	}
	if (root->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &product = root->Cast<BoundFunctionExpression>();
	if (product.function.name != "*" || product.children.size() != 2) {
		return false;
	}
	const Expression *coefficient = nullptr;
	if (IsExactlyVariable(*product.children[0], decide_index)) {
		coefficient = product.children[1].get();
	} else if (IsExactlyVariable(*product.children[1], decide_index)) {
		coefficient = product.children[0].get();
	}
	double value;
	return coefficient && DirectFiniteFoldableDouble(context, *coefficient, value) && value == 1.0;
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

bool FixedBooleanValue(ClientContext &context, const BoundComparisonExpression &comparison, idx_t &fixed_value) {
	double bound;
	if (!DirectFiniteFoldableDouble(context, *comparison.right, bound) || (bound != 0.0 && bound != 1.0)) {
		return false;
	}
	switch (comparison.GetExpressionType()) {
	case ExpressionType::COMPARE_EQUAL:
		fixed_value = bound == 1.0 ? 1 : 0;
		return true;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		if (bound == 0.0) {
			fixed_value = 0;
			return true;
		}
		return false;
	case ExpressionType::COMPARE_LESSTHAN:
		if (bound == 1.0) {
			fixed_value = 0;
			return true;
		}
		return false;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		if (bound == 1.0) {
			fixed_value = 1;
			return true;
		}
		return false;
	case ExpressionType::COMPARE_GREATERTHAN:
		if (bound == 0.0) {
			fixed_value = 1;
			return true;
		}
		return false;
	default:
		return false;
	}
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
const Expression *BoundMembership(const DirectConstraintFactor &factor, idx_t decide_index, bool &ok) {
	auto &comparison = factor.expression->Cast<BoundComparisonExpression>();
	auto sum = PlainSum(*comparison.left, decide_index, true);
	ok = sum && !(sum->filter && factor.when_condition);
	if (!ok) {
		return nullptr;
	}
	return sum->filter ? sum->filter.get() : factor.when_condition;
}

class S1CardinalityRule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return S1_RULE;
	}
	unique_ptr<DirectRuleMatch> Match(const DirectProblemFacts &facts, string &reason) const override {
		if (facts.decisions_status != DirectFactStatus::KNOWN || facts.decisions.size() != 1 ||
		    facts.auxiliary_variables != 0 || !facts.decisions[0].boolean_domain ||
		    facts.decisions[0].scope != DecideVarScope::ROW || facts.decisions[0].output_type != LogicalType::INTEGER ||
		    facts.has_entity_scopes || facts.has_entity_keys || facts.source_status != DirectFactStatus::KNOWN) {
			reason = "variable_shape: expected one row-scoped BOOL";
			return nullptr;
		}
		if (!std::isfinite(facts.objective_offset) ||
		    (facts.sense != DecideSense::MAXIMIZE && facts.sense != DecideSense::MINIMIZE)) {
			reason = "problem_shape: expected one linear objective and cardinality bounds";
			return nullptr;
		}
		if (facts.constraints_status != DirectFactStatus::KNOWN) {
			reason = "constraint_facts_unknown: DECIDE constraint wrapper is not modeled";
			return nullptr;
		}
		if (facts.constraint_factors.empty() || facts.source_clause_count != facts.constraint_factors.size()) {
			reason = "constraint_shape: expected attributed cardinality bounds and optional Boolean pins";
			return nullptr;
		}
		auto result = make_uniq<S1Match>();
		vector<uint8_t> seen_sources(facts.source_clause_count, 0);
		for (auto &factor : facts.constraint_factors) {
			if (factor.source_status != DirectFactStatus::KNOWN ||
			    factor.source_clause_id >= seen_sources.size() || seen_sources[factor.source_clause_id]++) {
				reason = "constraint_provenance: expected one distinct attributed source clause per factor";
				return nullptr;
			}
			if (factor.expression->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
				reason = "constraint_shape: expected SUM(x) bounds or Boolean pins";
				return nullptr;
			}
			auto &comparison = factor.expression->Cast<BoundComparisonExpression>();
			if (IsExactlyVariable(*comparison.left, facts.decide_index)) {
				result->local_fixes.push_back(&factor);
				continue;
			}
			if (!DirectIsLowerBound(comparison.GetExpressionType()) &&
			    !DirectIsUpperBound(comparison.GetExpressionType())) {
				reason = "constraint_shape: expected SUM(x) cardinality bounds";
				return nullptr;
			}
			result->bound_factors.push_back(&factor);
		}
		if (result->bound_factors.empty()) {
			reason = "constraint_shape: expected a SUM(x) cardinality bound";
			return nullptr;
		}
		if (facts.objective_status != DirectFactStatus::KNOWN) {
			reason = "objective_facts_unknown: DECIDE objective wrapper is not modeled";
			return nullptr;
		}
		if (facts.objective_terms.empty()) {
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
		proof->sense = facts.sense;
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
	const Expression *first_when = BoundMembership(first_scope, facts.decide_index, ok);
	if (!ok) {
		reason = "constraint_shape: expected one SUM(x) membership filter";
		return false;
	}
	for (auto factor : match.bound_factors) {
		const Expression *when = BoundMembership(*factor, facts.decide_index, ok);
		if (!ok) {
			reason = "constraint_shape: expected one SUM(x) membership filter";
			return false;
		}
		if (factor->per_keys.size() != first_scope.per_keys.size() ||
		    static_cast<bool>(when) != static_cast<bool>(first_when)) {
			reason = "constraint_scope: cardinality bounds use different PER or WHEN membership";
			return false;
		}
		if (when && !Expression::Equals(*when, *first_when)) {
			reason = "constraint_scope: cardinality bounds use different WHEN membership";
			return false;
		}
		for (idx_t i = 0; i < factor->per_keys.size(); i++) {
			if (!Expression::Equals(*factor->per_keys[i], *first_scope.per_keys[i])) {
				reason = "constraint_scope: cardinality bounds use different PER membership";
				return false;
			}
		}
	}
	for (auto key : first_scope.per_keys) {
		if (key->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			reason = "constraint_scope: PER keys must be source columns";
			return false;
		}
		auto &ref = key->Cast<BoundColumnRefExpression>();
		auto found = std::find(facts.source_bindings.begin(), facts.source_bindings.end(), ref.binding);
		if (ref.depth != 0 || found == facts.source_bindings.end()) {
			reason = "constraint_scope: PER keys must be source columns";
			return false;
		}
		proof.group_key_slots.push_back(found - facts.source_bindings.begin());
	}
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
		if (!factor->per_keys.empty()) {
			reason = "constraint_scope: Boolean pins must be per-row without PER keys";
			return false;
		}
		auto &comparison = factor->expression->Cast<BoundComparisonExpression>();
		if (factor->when_condition &&
		    !DirectIsSourceOnlyPredicate(*factor->when_condition, facts.decide_index, facts.source_bindings)) {
			reason = "constraint_scope: Boolean pin WHEN must be deterministic and source-only";
			return false;
		}
		idx_t fixed_value;
		if (FixedBooleanValue(context, comparison, fixed_value)) {
			auto condition = factor->when_condition ? factor->when_condition->Copy()
			                                        : make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
			if (fixed_value) {
				proof.fixed_one_conditions.push_back(std::move(condition));
			} else {
				proof.fixed_zero_conditions.push_back(std::move(condition));
			}
			continue;
		}
		auto source_value = SourceBooleanPinValue(*comparison.right, facts.decide_index, facts.source_bindings);
		auto type = comparison.GetExpressionType();
		if (!source_value || (type != ExpressionType::COMPARE_EQUAL &&
		                      type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
		                      type != ExpressionType::COMPARE_GREATERTHANOREQUALTO)) {
			reason = "constraint_shape: expected a Boolean pin at zero or one or a source Boolean equality/bound";
			return false;
		}
		auto active = [&](unique_ptr<Expression> value) -> unique_ptr<Expression> {
			return factor->when_condition
			           ? make_uniq<BoundCaseExpression>(factor->when_condition->Copy(), std::move(value),
			                                            make_uniq<BoundConstantExpression>(Value::BOOLEAN(false)))
			           : std::move(value);
		};
		if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
			proof.fixed_one_conditions.push_back(active(source_value->Copy()));
		}
		if (type == ExpressionType::COMPARE_EQUAL || type == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
			proof.fixed_zero_conditions.push_back(active(make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_EQUAL, source_value->Copy(),
			    make_uniq<BoundConstantExpression>(Value::BOOLEAN(false)))));
		}
		proof.source_pins.push_back({factor->source_clause_id, source_value->Copy()});
	}
	return true;
}

//! The count bounds: constant ones fold into one inclusive interval, source-valued ones are kept for the plan to
//! reduce per group. Every bound counts `x` with a unit contribution.
bool S1CardinalityRule::ProveBounds(const DirectProblemFacts &facts, const S1Match &match, ClientContext &context,
                                    S1Proof &proof, string &reason) const {
	auto fold_bound = [&](const BoundComparisonExpression &comparison, ExpressionType side, idx_t &target) {
		bool impossible = false;
		if (!NormalizeCapacity(context, *comparison.right, side, target, impossible)) {
			reason = "constraint_shape: cardinality bound must be a finite consistent foldable numeric expression "
			         "with an inclusive limit in [0, 2^53], or a deterministic nonthrowing source-only "
			         "numeric expression";
			return false;
		}
		proof.impossible |= impossible;
		return true;
	};
	for (auto factor : match.bound_factors) {
		auto &comparison = factor->expression->Cast<BoundComparisonExpression>();
		auto sum = PlainSum(*comparison.left, facts.decide_index, true);
		if (!sum || !IsUnitDecisionTerm(context, *sum->children[0], facts.decide_index)) {
			reason = "constraint_shape: expected SUM(x) with unit contribution";
			return false;
		}
		auto type = comparison.GetExpressionType();
		idx_t source_slot = DConstants::INVALID_INDEX;
		LogicalType source_type;
		string source_name;
		bool source_column = SourceNumericColumnBound(*comparison.right, facts.source_bindings, source_slot,
		                                              source_type, source_name);
		if (!comparison.right->IsFoldable() &&
		    (source_column ||
		     SourceNumericExpressionBound(*comparison.right, facts.decide_index, facts.source_bindings))) {
			proof.source_bounds.push_back({factor->source_clause_id, source_slot,
			                               source_column ? source_type : comparison.right->return_type, type,
			                               comparison.right->Copy(), sum->filter != nullptr,
			                               source_column ? source_name : string()});
			continue;
		}
		if (DirectIsLowerBound(type)) {
			idx_t lower;
			if (!fold_bound(comparison, type == ExpressionType::COMPARE_EQUAL
			                                ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
			                                : type,
			                lower)) {
				return false;
			}
			proof.lower = std::max(proof.lower, lower);
		}
		if (DirectIsUpperBound(type)) {
			idx_t upper;
			if (!fold_bound(comparison, type == ExpressionType::COMPARE_EQUAL
			                                ? ExpressionType::COMPARE_LESSTHANOREQUALTO
			                                : type,
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
	for (auto &objective_term : facts.objective_terms) {
		if (objective_term.sign != 1 && objective_term.sign != -1) {
			reason = "objective_shape: expected signed linear objective terms";
			return false;
		}
		auto objective_sum = PlainSum(*objective_term.expression, facts.decide_index);
		if (!objective_sum) {
			reason = "objective_shape: expected unfiltered SUM(coefficient * x) terms";
			return false;
		}
		auto term = UnwrapDecideCasts(*objective_sum->children[0], facts.decide_index);
		unique_ptr<Expression> coefficient;
		if (IsExactlyVariable(*term, facts.decide_index)) {
			coefficient = make_uniq<BoundConstantExpression>(Value::INTEGER(1));
		} else {
			if (term->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
				reason = "objective_shape: expected a linear product with x";
				return false;
			}
			auto &product = term->Cast<BoundFunctionExpression>();
			if (product.function.name != "*" || product.children.size() != 2) {
				reason = "objective_shape: expected a coefficient times x";
				return false;
			}
			const Expression *source_coefficient = nullptr;
			if (IsExactlyVariable(*product.children[0], facts.decide_index)) {
				source_coefficient = product.children[1].get();
			} else if (IsExactlyVariable(*product.children[1], facts.decide_index)) {
				source_coefficient = product.children[0].get();
			}
			if (!source_coefficient || !SafeCoefficient(*source_coefficient, facts.decide_index)) {
				reason = "coefficient_shape: expected a deterministic numeric expression without decisions";
				return false;
			}
			coefficient = source_coefficient->Copy();
		}
		if (!DirectReferencesOnlySource(*coefficient, facts.source_bindings) ||
		    (facts.objective_terms.size() > 1 && DirectMayThrow(*coefficient))) {
			reason = "coefficient_shape: multiple terms need nonthrowing source-only numeric coefficients";
			return false;
		}
		proof.objective_parts.push_back({objective_term.sign, std::move(coefficient)});
	}
	// A single term is NULL exactly when its coefficient is, so a bare column can be named in the error.
	if (proof.objective_parts.size() == 1) {
		if (auto column = DirectBareNumericColumn(*proof.objective_parts[0].coefficient)) {
			proof.score_column = column->GetAlias();
		}
	}
	return true;
}

//! The error for a NULL or NaN source-valued count bound, worded like the solver path: name the column when the
//! bound is one, otherwise point at the bound expression. Only floating point values can be NaN.
string InvalidSourceBoundMessage(const S1Proof::SourceBound &bound) {
	auto type = bound.source_slot == DConstants::INVALID_INDEX ? bound.value->return_type : bound.source_type;
	bool may_be_nan = type == LogicalType::FLOAT || type == LogicalType::DOUBLE;
	auto problem = may_be_nan ? "NULL or NaN" : "NULL";
	auto impute = may_be_nan ? "Impute NULLs" : "Impute it";
	if (bound.column_name.empty()) {
		return StringUtil::Format("DECIDE: the bound expression is %s. %s with COALESCE(), or filter those rows "
		                          "out with a WHERE clause.",
		                          problem, impute);
	}
	return StringUtil::Format("DECIDE: column \"%s\" is %s. %s with COALESCE(%s, 0) or filter those rows out "
	                          "with a WHERE clause.",
	                          bound.column_name, problem, impute, bound.column_name);
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
		BoundChecks bounds(proof.source_bounds.size(), proof.source_pins.size());
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
		idx_t invalid_window = DConstants::INVALID_INDEX;
		idx_t min_window = DConstants::INVALID_INDEX;
		idx_t max_window = DConstants::INVALID_INDEX;
		//! Slots of the same extrema once the score projection forwards them.
		idx_t min_score = DConstants::INVALID_INDEX;
		idx_t max_score = DConstants::INVALID_INDEX;
	};

	//! Windows computed for the source-valued bounds and pins, and the slots later stages read from them.
	struct BoundChecks {
		BoundChecks(idx_t bound_count, idx_t pin_count)
		    : slots(bound_count), pin_invalid_windows(pin_count, DConstants::INVALID_INDEX),
		      equality_slots(bound_count, DConstants::INVALID_INDEX) {
		}
		idx_t window_index = DConstants::INVALID_INDEX;
		idx_t equality_window_index = DConstants::INVALID_INDEX;
		vector<SourceBoundSlots> slots;
		vector<idx_t> pin_invalid_windows;
		vector<idx_t> equality_slots;
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
		scope.input_bindings = source_bindings;
		if (!proof.scoped && !has_fixes) {
			return source;
		}
		scope.state_index = binder.GenerateTableIndex();
		vector<unique_ptr<Expression>> state_expressions;
		for (idx_t i = 0; i < source_bindings.size(); i++) {
			state_expressions.push_back(DirectColumn(source_types[i], source_bindings[i]));
			scope.input_bindings[i] = ColumnBinding(scope.state_index, i);
		}
		if (proof.scoped) {
			unique_ptr<Expression> eligible = proof.when_condition
			                                      ? make_uniq<BoundCaseExpression>(proof.when_condition->Copy(),
			                                                                       DirectConstantBool(true),
			                                                                       DirectConstantBool(false))
			                                      : DirectConstantBool(true);
			for (idx_t i = 0; i < proof.group_key_slots.size(); i++) {
				auto slot = proof.group_key_slots[i];
				if (slot >= source_bindings.size()) {
					throw InternalException("S1 direct solve received an invalid PER key slot");
				}
				auto is_null =
				    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
				is_null->children.push_back(DirectColumn(source_types[slot], source_bindings[slot]));
				eligible =
				    make_uniq<BoundCaseExpression>(std::move(is_null), DirectConstantBool(false), std::move(eligible));
			}
			state_expressions.push_back(std::move(eligible));
			scope.eligible_binding = ColumnBinding(scope.state_index, source_bindings.size());
		}
		if (has_fixes) {
			auto fixed_one_slot = state_expressions.size();
			state_expressions.push_back(DirectAnyCondition(proof.fixed_one_conditions));
			state_expressions.push_back(DirectAnyCondition(proof.fixed_zero_conditions));
			scope.fixed_one_binding = ColumnBinding(scope.state_index, fixed_one_slot);
			scope.fixed_zero_binding = ColumnBinding(scope.state_index, fixed_one_slot + 1);
		}
		auto state = make_uniq<LogicalProjection>(scope.state_index, std::move(state_expressions));
		state->children.push_back(std::move(source));
		return std::move(state);
	}

	//! Stage 2: a scoped aggregate over no eligible row is DECIDE's empty-aggregate error.
	unique_ptr<LogicalOperator> GuardEmptyAggregate(unique_ptr<LogicalOperator> source, const ScopeState &scope) const {
		if (!proof.scoped) {
			return source;
		}
		auto active_window_index = binder.GenerateTableIndex();
		auto active_count = DirectWindowMatchingCount(DirectColumn(LogicalType::BOOLEAN, scope.eligible_binding));
		auto active_window = make_uniq<LogicalWindow>(active_window_index);
		active_window->expressions.push_back(std::move(active_count));
		active_window->children.push_back(std::move(source));
		auto has_active_rows = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHAN,
		    DirectColumn(LogicalType::BIGINT, ColumnBinding(active_window_index, 0)),
		    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
		auto active_predicate = make_uniq<BoundCaseExpression>(
		    std::move(has_active_rows), DirectConstantBool(true),
		    DirectErrorPredicate(optimizer, "DECIDE empty row set for aggregate in constraint. "
		                                    "An empty aggregate has no well-defined value; check your WHEN clause."));
		auto active_guard = make_uniq<LogicalFilter>(std::move(active_predicate));
		active_guard->children.push_back(std::move(active_window));
		return std::move(active_guard);
	}

	//! Stage 3: validate source-valued bounds and pins. One window computes, per group, the invalid-value count and the
	//! extrema of every bound; a filter then raises the first failing clause in source-clause order.
	unique_ptr<LogicalOperator> ValidateBounds(unique_ptr<LogicalOperator> source, const ScopeState &scope,
	                                           BoundChecks &bounds) const {
		if (proof.source_bounds.empty() && proof.source_pins.empty()) {
			return source;
		}
		bounds.window_index = binder.GenerateTableIndex();
		auto bound_window = BuildBoundWindow(scope, bounds);
		bound_window->children.push_back(std::move(source));
		unique_ptr<LogicalOperator> bounds_ready = std::move(bound_window);
		bounds_ready = AddEqualityWindow(std::move(bounds_ready), scope, bounds);
		auto bound_guard = make_uniq<LogicalFilter>(BuildBoundValidity(bounds));
		bound_guard->children.push_back(std::move(bounds_ready));
		return std::move(bound_guard);
	}

	//! Stage 3a: per-bound invalid count and group extrema, per-pin invalid count.
	unique_ptr<LogicalWindow> BuildBoundWindow(const ScopeState &scope, BoundChecks &bounds) const {
		auto bound_window = make_uniq<LogicalWindow>(bounds.window_index);
		for (idx_t i = 0; i < proof.source_bounds.size(); i++) {
			auto &bound = proof.source_bounds[i];
			auto &slots = bounds.slots[i];
			if (!bound.value || (bound.source_slot != DConstants::INVALID_INDEX &&
			                     (bound.source_slot >= source_types.size() ||
			                      source_types[bound.source_slot] != bound.source_type))) {
				throw InternalException("S1 direct solve received an invalid source bound slot");
			}
			auto cap = [&]() {
				auto value = CopyIntoState(*bound.value, scope, "bound");
				return BoundCastExpression::AddCastToType(optimizer.context, std::move(value), LogicalType::DOUBLE);
			};
			auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
			is_null->children.push_back(bound.source_slot == DConstants::INVALID_INDEX
			                                ? cap()
			                                : DirectColumn(bound.source_type, scope.input_bindings[bound.source_slot]));
			unique_ptr<Expression> is_invalid = std::move(is_null);
			if (bound.source_slot == DConstants::INVALID_INDEX || bound.source_type == LogicalType::FLOAT ||
			    bound.source_type == LogicalType::DOUBLE) {
				is_invalid = make_uniq<BoundCaseExpression>(std::move(is_invalid), DirectConstantBool(true),
				                                            optimizer.BindScalarFunction("isnan", cap()));
			}
			slots.invalid_window = bound_window->expressions.size();
			bound_window->expressions.push_back(DirectWindowMatchingCount(std::move(is_invalid)));
			auto add_extremum = [&](const char *name) {
				unique_ptr<Expression> value = cap();
				if (proof.scoped && !bound.rhs_all_group_rows) {
					value = make_uniq<BoundCaseExpression>(
					    DirectColumn(LogicalType::BOOLEAN, scope.eligible_binding), std::move(value),
					    make_uniq<BoundConstantExpression>(Value(LogicalType::DOUBLE)));
				}
				auto extremum = DirectWindowExtremum(optimizer, name, std::move(value));
				if (proof.scoped && !bound.rhs_all_group_rows) {
					extremum->partitions.push_back(DirectColumn(LogicalType::BOOLEAN, scope.eligible_binding));
				}
				for (auto slot : proof.group_key_slots) {
					extremum->partitions.push_back(DirectColumn(source_types[slot], scope.input_bindings[slot]));
				}
				auto result_slot = bound_window->expressions.size();
				bound_window->expressions.push_back(std::move(extremum));
				return result_slot;
			};
			if (DirectIsUpperBound(bound.comparison)) {
				slots.min_window = add_extremum("min");
			}
			if (DirectIsLowerBound(bound.comparison)) {
				slots.max_window = add_extremum("max");
			}
		}
		for (idx_t i = 0; i < proof.source_pins.size(); i++) {
			auto value = CopyIntoState(*proof.source_pins[i].value, scope, "Boolean pin");
			auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
			is_null->children.push_back(std::move(value));
			bounds.pin_invalid_windows[i] = bound_window->expressions.size();
			bound_window->expressions.push_back(DirectWindowMatchingCount(std::move(is_null)));
		}
		return bound_window;
	}

	//! Stage 3b: count rows where an equality bound's value differs from its group maximum.
	unique_ptr<LogicalOperator> AddEqualityWindow(unique_ptr<LogicalOperator> bounds_ready, const ScopeState &scope,
	                                              BoundChecks &bounds) const {
		unique_ptr<LogicalWindow> equality_window;
		for (idx_t i = 0; i < proof.source_bounds.size(); i++) {
			if (proof.source_bounds[i].comparison != ExpressionType::COMPARE_EQUAL) {
				continue;
			}
			if (!equality_window) {
				bounds.equality_window_index = binder.GenerateTableIndex();
				equality_window = make_uniq<LogicalWindow>(bounds.equality_window_index);
			}
			auto &slots = bounds.slots[i];
			unique_ptr<Expression> varies = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_NOTEQUAL,
			    DirectColumn(LogicalType::DOUBLE, ColumnBinding(bounds.window_index, slots.min_window)),
			    DirectColumn(LogicalType::DOUBLE, ColumnBinding(bounds.window_index, slots.max_window)));
			if (proof.scoped) {
				varies = make_uniq<BoundCaseExpression>(DirectColumn(LogicalType::BOOLEAN, scope.eligible_binding),
				                                        std::move(varies), DirectConstantBool(false));
			}
			bounds.equality_slots[i] = equality_window->expressions.size();
			equality_window->expressions.push_back(DirectWindowMatchingCount(std::move(varies)));
		}
		if (!equality_window) {
			return bounds_ready;
		}
		equality_window->children.push_back(std::move(bounds_ready));
		return std::move(equality_window);
	}

	//! Stage 3c: one predicate that raises the first failing clause check, in source-clause order.
	unique_ptr<Expression> BuildBoundValidity(const BoundChecks &bounds) const {
		vector<pair<idx_t, unique_ptr<Expression>>> clause_checks;
		for (idx_t i = 0; i < proof.source_bounds.size(); i++) {
			auto &bound = proof.source_bounds[i];
			auto &slots = bounds.slots[i];
			auto no_invalid = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_EQUAL,
			    DirectColumn(LogicalType::BIGINT, ColumnBinding(bounds.window_index, slots.invalid_window)),
			    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
			unique_ptr<Expression> equality_valid = DirectConstantBool(true);
			if (bound.comparison == ExpressionType::COMPARE_EQUAL) {
				auto no_variation = make_uniq<BoundComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL,
				    DirectColumn(LogicalType::BIGINT,
				                 ColumnBinding(bounds.equality_window_index, bounds.equality_slots[i])),
				    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
				equality_valid = make_uniq<BoundCaseExpression>(
				    std::move(no_variation), DirectConstantBool(true),
				    DirectErrorPredicate(optimizer, "DECIDE source-valued equality bound varies within a group"));
			}
			auto clause_valid = make_uniq<BoundCaseExpression>(
			    std::move(no_invalid), std::move(equality_valid),
			    DirectErrorPredicate(optimizer, InvalidSourceBoundMessage(bound)));
			clause_checks.emplace_back(bound.source_clause_id, std::move(clause_valid));
		}
		for (idx_t i = 0; i < proof.source_pins.size(); i++) {
			auto no_invalid = make_uniq<BoundComparisonExpression>(
			    ExpressionType::COMPARE_EQUAL,
			    DirectColumn(LogicalType::BIGINT, ColumnBinding(bounds.window_index, bounds.pin_invalid_windows[i])),
			    make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
			auto clause_valid = make_uniq<BoundCaseExpression>(
			    std::move(no_invalid), DirectConstantBool(true),
			    DirectErrorPredicate(optimizer, "DECIDE per-row Boolean bound contains NULL"));
			clause_checks.emplace_back(proof.source_pins[i].source_clause_id, std::move(clause_valid));
		}
		std::sort(clause_checks.begin(), clause_checks.end(),
		          [](const pair<idx_t, unique_ptr<Expression>> &left,
		             const pair<idx_t, unique_ptr<Expression>> &right) { return left.first < right.first; });
		unique_ptr<Expression> valid_bound = DirectConstantBool(true);
		for (idx_t i = clause_checks.size(); i > 0; i--) {
			valid_bound = make_uniq<BoundCaseExpression>(std::move(clause_checks[i - 1].second), std::move(valid_bound),
			                                             DirectConstantBool(false));
		}
		return valid_bound;
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
		auto guard = make_uniq<LogicalFilter>(DirectValidScorePredicate(optimizer, LogicalType::DOUBLE, score_binding,
		                                          proof.score_column));
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
