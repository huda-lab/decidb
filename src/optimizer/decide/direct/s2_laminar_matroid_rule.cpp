#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <algorithm>

#include "duckdb/common/exception.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"

namespace duckdb {

//! Each rule keeps its helpers in its own named namespace: unity builds compile every rule file in one translation
//! unit, where two anonymous namespaces would merge and same-named helpers would collide.
namespace direct_s2_laminar_matroid {

constexpr const char *RULE_NAME = "S2_LAMINAR_MATROID";

//! One level of the quota hierarchy: the rows and groups its clauses count, and the room each group has for rows.
struct Level {
	DirectSharedScope scope;
	//! The most rows a group may select: the least of the constant upper limits of the clauses that share the scope,
	//! each rounded to a whole number of rows (DirectInclusiveRows).
	idx_t cap = 0;
	//! Some clause's limit is below zero: no group of the level can be met, since selecting no row still counts 0.
	bool impossible = false;
};

//! What CheckCondition proved: one row-scoped BOOL decision under upper count quotas of two or more scopes, each scope
//! inside the next, and a linear objective. The levels are listed finest first.
struct Condition final : DirectApplicabilityCondition {
	//! The constraints of the IR, which outlives the condition: the count clauses, and the fixed Boolean values.
	vector<const DirectIRConstraint *> bound_factors;
	vector<const DirectIRConstraint *> local_fixes;
	vector<Level> levels;
	//! The score: what S2 ranks the rows by.
	DirectLinearScore score;
	DirectFixedValues fixed;
};

//! Whether `coarse` contains `fine`: every group of `fine` lies in one group of `coarse`, proved from the text of the
//! two clauses (the IR carries no functional dependencies, so the data is not consulted). The `by` key of `coarse` is
//! a subset of `fine`'s, `coarse` keeps every row `fine` keeps (it has no `when`, or the same one), and both are
//! gated by the same `per`.
bool Contains(const DirectSharedScope &coarse, const DirectSharedScope &fine) {
	return DirectKeyWithin(coarse.keys, fine.keys) && (!coarse.when || Expression::Equals(coarse.when, fine.when)) &&
	       coarse.per == fine.per && DirectSameKey(coarse.classes, fine.classes);
}

class Rule final : public DirectSolveRule {
public:
	const char *Name() const override {
		return RULE_NAME;
	}
	unique_ptr<DirectApplicabilityCondition> CheckCondition(const DirectIR &ir, ClientContext &context) const override {
		if (!DirectIsOneRowBool(ir)) {
			return nullptr;
		}
		auto condition = make_uniq<Condition>();
		if (!DirectSplitFixes(ir, condition->local_fixes, condition->bound_factors) ||
		    condition->bound_factors.empty()) {
			return nullptr;
		}
		if (!ProveLevels(ir, context, *condition) ||
		    !DirectReadFixedValues(ir, context, condition->local_fixes, condition->fixed) ||
		    !condition->score.Prove(ir, context)) {
			return nullptr;
		}
		return std::move(condition);
	}
	string Summary() const override {
		return "min/max SUM(s*x), x in {0,1}, nested SUM(x) <= k quotas";
	}
	DirectSubstitute Apply(DirectSourceReader &reader, Optimizer &optimizer,
	                       const DirectApplicabilityCondition &candidate,
	                       const DirectStateErrors &errors) const override;

private:
	bool ProveLevels(const DirectIR &ir, ClientContext &context, Condition &condition) const;
};

//! Each clause is an upper count limit on one scope: `SUM(x) <= k` (or `SUM(-x) >= -k`) with a constant k. Clauses of
//! one scope merge into one level, as S1 merges its bounds, and the levels must nest.
bool Rule::ProveLevels(const DirectIR &ir, ClientContext &context, Condition &condition) const {
	vector<Level> merged;
	for (auto factor : condition.bound_factors) {
		Level clause;
		bool negated;
		DirectCountLimit count;
		// A limit read from the data (a group bound) stays with the solver.
		if (!DirectReadScope(ir, *factor, clause.scope) ||
		    !DirectReadCountBody(context, factor->PlainSum()->terms, negated) ||
		    !DirectReadCountLimit(ir, context, *factor, negated, count) || !count.limit.constant) {
			return false;
		}
		// A lower or an equal limit can force a selection, which the cascade of upper quotas cannot honor.
		if (count.comparison != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
		    count.comparison != ExpressionType::COMPARE_LESSTHAN) {
			return false;
		}
		if (!DirectInclusiveRows(count.limit.number, count.comparison, true, clause.cap, clause.impossible)) {
			return false;
		}
		auto same = std::find_if(merged.begin(), merged.end(),
		                         [&](const Level &level) { return DirectSameScope(level.scope, clause.scope); });
		if (same == merged.end()) {
			merged.push_back(std::move(clause));
			continue;
		}
		same->cap = std::min(same->cap, clause.cap);
		same->impossible |= clause.impossible;
	}
	if (merged.size() < 2) {
		return false;
	}
	// The levels form a chain: repeatedly take the finest, the level every other one contains.
	while (!merged.empty()) {
		auto finest = std::find_if(merged.begin(), merged.end(), [&](const Level &level) {
			return std::all_of(merged.begin(), merged.end(), [&](const Level &other) {
				return &other == &level || Contains(other.scope, level.scope);
			});
		});
		if (finest == merged.end()) {
			return false;
		}
		condition.levels.push_back(std::move(*finest));
		merged.erase(finest);
	}
	return true;
}

DirectSubstitute Rule::Apply(DirectSourceReader &, Optimizer &, const DirectApplicabilityCondition &,
                             const DirectStateErrors &) const {
	throw InternalException("S2_LAMINAR_MATROID has no plan yet");
}

} // namespace direct_s2_laminar_matroid

unique_ptr<DirectSolveRule> MakeS2LaminarMatroidRule() {
	return make_uniq<direct_s2_laminar_matroid::Rule>();
}

} // namespace duckdb
