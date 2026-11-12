#include "duckdb/optimizer/decide/direct/direct_rules.hpp"

#include <algorithm>

#include "duckdb/common/exception.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/decide/direct/direct_ranked_prefix.hpp"
#include "duckdb/optimizer/decide/direct/direct_source_reader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

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

//! Builds the relational plan for one S2 problem whose condition holds, on the shared ranked-prefix plan
//! (DirectRankedPrefix) with one level per scope, finest first: a level ranks, in its own groups and in order of the
//! score, the rows the levels before it keep, and keeps those ranked within its group's remaining cap, the cap less
//! the group's rows fixed to 1. A row outside a level's groups (a `when` drops it, a key is NULL) passes the level. The
//! rows the last level keeps whose score improves the objective are chosen. This is the exact greedy for a laminar
//! family: the survivors of a finer level contain an optimal selection of the coarser one.
//!
//!   S    the scope projection: each level's eligible flag and group key, and the fixed-value flags
//!   P1   the score; the check of the score; the checks of the fixed values (the builder's)
//!   W1   for each level, per group, in order of the score: the rank among the rows the finer levels keep (the
//!        builder's) and the count of the rows fixed to 1, where a clause fixes rows
//!   P2   the score, the rows the last level keeps, and for each level whether its groups can be met
//!   H    the rows held in a materialized CTE, and whether every group of every level can be met, raised in the hold;
//!        only where a group can fail
//!   A    the 0/1 assignment, the source columns and the column of the checks
class PlanBuilder final : public DirectRankedPrefix {
public:
	PlanBuilder(const Condition &condition_p, Optimizer &optimizer_p, const vector<ColumnBinding> &source_bindings_p,
	            const vector<LogicalType> &source_types_p, const DirectStateErrors &errors_p)
	    : DirectRankedPrefix(optimizer_p, source_bindings_p, source_types_p, errors_p, ScopesOf(condition_p),
	                         condition_p.fixed),
	      condition(condition_p), fixed_count(condition_p.levels.size()), group_ok(condition_p.levels.size()) {
		has_score_message = condition.score.CanFail(optimizer);
	}

private:
	const Condition &condition;
	//! Whether a score can fail: be NaN or infinite, or the sum of the parts' scores pass a DOUBLE.
	bool has_score_message = false;
	//! P1: the row's score.
	ColumnBinding score_column;
	//! W1, per level: the count of the group's rows fixed to 1; set only with fixed rows.
	vector<ColumnBinding> fixed_count;
	//! P2: the score, the rows the last level keeps, and per level whether its groups can be met (set where a group
	//! can fail).
	ColumnBinding score_after;
	ColumnBinding kept;
	vector<ColumnBinding> group_ok;

	static vector<reference<const DirectSharedScope>> ScopesOf(const Condition &condition) {
		vector<reference<const DirectSharedScope>> scopes;
		for (auto &level : condition.levels) {
			scopes.push_back(level.scope);
		}
		return scopes;
	}

	//! Whether a group of the level can fail to be met: a clause fixes rows that may exceed its cap, or its cap is
	//! below zero.
	bool CanFail(idx_t level) const {
		return HasFixes() || condition.levels[level].impossible;
	}

	//! The best score is the least: the score is minimized.
	OrderType RankOrder() const override {
		return OrderType::ASCENDING;
	}

	//! P1: the score and the check of it. What the fixed values read is the builder's.
	DirectRowLayout LayoutRows(DirectProjectionStage &p1, const DirectScopeState &state) override {
		DirectRowLayout layout;
		vector<ExprPtr> part_scores;
		auto over_state = [&](const Expression &value) {
			return OverState(state, value);
		};
		auto combined_score = condition.score.Combined(calc, optimizer, over_state, part_scores);
		score_column = p1.Add(calc.Out(combined_score));
		if (has_score_message) {
			layout.checks_below.push_back(DirectRaiseMessage(
			    optimizer, calc, condition.score.Message(calc, optimizer, over_state, combined_score, part_scores)));
		}
		layout.rank_key = score_column;
		return layout;
	}

	//! W1: the count of the rows of the level's group fixed to 1, which its remaining cap subtracts.
	void AddGroupWindows(idx_t level, LogicalWindow &w1, const DirectRankedColumns &rows) override {
		if (!HasFixes()) {
			return;
		}
		auto count = DirectWindowMatchingCount(calc, calc.Out(Column(LogicalType::BOOLEAN, rows.fixed_one)));
		OverRank(rows.levels[level], *count);
		w1.expressions.push_back(std::move(count));
		fixed_count[level] = ColumnBinding(w1.window_index, w1.expressions.size() - 1);
	}

	//! How many rows of the level's group the free rows may take: its cap, less the rows fixed to 1. Below zero when
	//! the group cannot be met. A clause with a limit below zero makes the cap -1.
	ExprPtr Remaining(idx_t level) const {
		auto &at = condition.levels[level];
		auto cap = calc.Constant(Value::BIGINT(at.impossible ? -1 : NumericCast<int64_t>(at.cap)));
		return HasFixes() ? calc.Sub(cap, Column(LogicalType::BIGINT, fixed_count[level])) : cap;
	}

	//! Whether the level keeps the row, given that the levels before it do: a row outside its groups does, and so does
	//! a row ranked within the group's remaining cap.
	ExprPtr WithinLimit(idx_t level, const DirectRankedColumns &rows) const override {
		auto within = calc.Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                       Column(LogicalType::BIGINT, rows.levels[level].rank), Remaining(level));
		if (!levels[level].scoped) {
			return within;
		}
		return calc.If(Column(LogicalType::BOOLEAN, rows.levels[level].eligible), within, calc.Bool(true));
	}

	//! P2: the score, whether every level keeps the row, and for each level that can fail whether its group can be
	//! met.
	vector<unique_ptr<Expression>> LayoutPrefix(DirectProjectionStage &p2, const DirectRankedColumns &input) override {
		score_after = p2.Add(Column(LogicalType::DOUBLE, score_column));
		kept = p2.Add(calc.Out(Keeps(levels.size() - 1, input)));
		for (idx_t i = 0; i < levels.size(); i++) {
			if (CanFail(i)) {
				group_ok[i] = p2.Add(calc.Out(calc.Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Remaining(i),
				                                       calc.Constant(Value::BIGINT(0)))));
			}
		}
		return {};
	}

	ExprPtr GroupVerdict(idx_t level, const DirectRankedColumns &rows) override {
		return CanFail(level) ? Column(LogicalType::BOOLEAN, group_ok[level]) : nullptr;
	}

	//! The 0/1 assignment of a row no clause fixes: every level keeps it and its score improves the objective.
	ExprPtr Chosen(const DirectRankedColumns &rows) override {
		auto improving =
		    calc.Cmp(ExpressionType::COMPARE_LESSTHAN, Column(LogicalType::DOUBLE, score_after), calc.Num(0.0));
		return calc.And(Column(LogicalType::BOOLEAN, kept), improving);
	}
};

DirectSubstitute Rule::Apply(DirectSourceReader &reader, Optimizer &optimizer,
                             const DirectApplicabilityCondition &candidate, const DirectStateErrors &errors) const {
	auto &condition = static_cast<const Condition &>(candidate);
	PlanBuilder plan_builder(condition, optimizer, reader.SourceBindings(), reader.SourceTypes(), errors);
	return DirectBuildOverOneRead(
	    reader, [&](unique_ptr<LogicalOperator> rows) { return plan_builder.Build(std::move(rows)); });
}

} // namespace direct_s2_laminar_matroid

unique_ptr<DirectSolveRule> MakeS2LaminarMatroidRule() {
	return make_uniq<direct_s2_laminar_matroid::Rule>();
}

} // namespace duckdb
