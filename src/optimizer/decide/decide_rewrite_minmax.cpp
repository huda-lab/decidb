//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/optimizer/decide/decide_rewrite_minmax.cpp
//
// The easy identity of a MIN or MAX in a constraint. See decide_optimizer.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/planner/decide/decide_cast_policy.hpp"

#include <cstdlib>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_solver_gate.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

// ---------------------------------------------------------------------------
// The easy identity
// ---------------------------------------------------------------------------
//
// A MIN or MAX over decisions is a helper decision (RewriteScopedReducers) with one side of its definition, the side
// its reader pushes it against (the epigraph auxiliary, Plaisted-Greenbaum). One identity is applied before that,
// because it is no route but a rewrite to other clauses: `max(e) <= K` holds when every member `e <= K` does, and
// `min(e) >= K` the mirror, so the clause keeps its reducer, marked (BoundDecideClause::minmax_easy), and the linear
// form states one row per member, which bound absorption folds into the column's box (D41). An equality is the pair of
// its two relations, as BETWEEN is: the easy one by this identity, the other left in place for the helper.
//
// The factor on a reducer stays outside it (the canonicalizer peels it outward): MIN and MAX are order statistics, so
// they commute with a positive factor only, and the sign is part of the direction. A factor whose sign is not known
// until the query runs gives the term no direction, and the clause takes the helper pinned from both sides.

//! The aggregate of a left side that is one signed MIN or MAX term, `[-] [factor *] min/max(e)`, else null. Its sign
//! is `sign` and its factor `match.scale`, if any: the term's direction is the comparison's times both.
static const BoundDecideReducerExpression *MatchLoneSignedExtremum(const Expression &lhs, idx_t decide_index, int &sign,
                                                                   ScaledAggregateMatch &match) {
	auto atoms = ReadCanonicalAtoms(lhs, decide_index);
	if (atoms.size() != 1) {
		return nullptr;
	}
	auto &atom = atoms[0];
	auto *root = UnwrapDecideCasts(*atom.term, decide_index);
	if (!atom.scaled && root->GetExpressionClass() != ExpressionClass::BOUND_DECIDE_REDUCER) {
		return nullptr;
	}
	auto &agg = atom.scaled ? *atom.scale.aggregate : root->Cast<BoundDecideReducerExpression>();
	if (!IsExtremumName(StringUtil::Lower(agg.function.name))) {
		return nullptr;
	}
	sign = atom.sign;
	match = atom.scale;
	return &agg;
}

void DecideOptimizer::RewriteMinMax(LogicalDecide &decide) {
	vector<BoundDecideClause> new_clauses;
	for (auto &clause : decide.bound.clauses) {
		RewriteMinMaxInClause(clause, decide, new_clauses);
	}
	for (auto &clause : new_clauses) {
		decide.AddClause(optimizer.context, std::move(clause));
	}
}

void DecideOptimizer::RewriteMinMaxInClause(BoundDecideClause &clause, LogicalDecide &decide,
                                            vector<BoundDecideClause> &new_clauses) {
	// A clause in the VALUE shape holds its reducers in helpers, whatever their direction.
	if (clause.body->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON ||
	    clause.shape == CanonicalConstraintClass::VALUE) {
		return;
	}
	auto &comp = clause.body->Cast<BoundComparisonExpression>();

	ScaledAggregateMatch scale_match;
	int term_sign;
	auto *found = MatchLoneSignedExtremum(*comp.left, decide.decide_index, term_sign, scale_match);
	if (!found) {
		return;
	}
	auto &agg = *found;
	const bool is_max = StringUtil::CIEquals(agg.function.name, "max");
	// A negative sign or factor reverses the relation the aggregate faces: `-2 * MAX(e) >= -8` and `-MAX(e) >= -8` are
	// `MAX(e) <= 4` and `MAX(e) <= 8`, the cheap direction, not the expensive one. A factor whose sign is not known
	// has no direction.
	const int scale_sign = ScaleSignAtPlanTime(optimizer.context, scale_match.scale, scale_match.divides);
	if (scale_sign == 0) {
		return;
	}
	const int direction = term_sign * scale_sign;
	const auto cmp_type = comp.type;
	const bool is_easy = IsEasyExtremum(is_max, ExtremumPushOf(ExtremumPushOf(cmp_type), direction));
	if (!is_easy && cmp_type != ExpressionType::COMPARE_EQUAL) {
		return;
	}
	AddRewriteAccount(decide, clause.origin.source_clause_id, LogicalDecide::RewriteAccount::Kind::EXTREMUM_EACH_MEMBER,
	                  RenderForPlan(agg, decide));

	if (cmp_type == ExpressionType::COMPARE_EQUAL) {
		// MAX(expr) = K is (expr <= K) AND (MAX(expr) >= K), and MIN(expr) = K is (expr >= K) AND (MIN(expr) <= K). A
		// negative sign or factor reverses both, for the same reason it reverses the classification above.
		auto easy_cmp_type =
		    is_max ? ExpressionType::COMPARE_LESSTHANOREQUALTO : ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		auto other_cmp_type =
		    is_max ? ExpressionType::COMPARE_GREATERTHANOREQUALTO : ExpressionType::COMPARE_LESSTHANOREQUALTO;
		if (direction < 0) {
			easy_cmp_type = FlipComparisonExpression(easy_cmp_type);
			other_cmp_type = FlipComparisonExpression(other_cmp_type);
		}
		// The easy half is a copy of the clause with the easy relation; the other keeps the original, which the helper
		// takes.
		BoundDecideClause easy;
		easy.body = make_uniq<BoundComparisonExpression>(easy_cmp_type, comp.left->Copy(), comp.right->Copy());
		easy.body->alias = comp.alias;
		easy.origin = clause.origin;
		easy.structural = clause.structural;
		easy.defines = clause.defines;
		easy.minmax_easy = true;
		easy.query_wide_bound = clause.query_wide_bound;
		// Under the clause's own `per K:`, the easy half stands for the same classes (D28) and prints in front as
		// written.
		ScopeRowPerClause(optimizer.context, decide, easy, clause);
		new_clauses.push_back(std::move(easy));
		comp.type = other_cmp_type;
		return;
	}
	// The reducer stays: the linear form bounds each member (its `when` chooses the members, its `by` the rows the
	// bound is reduced over); bound absorption folds a bare `max(x) <= K` into the column box (D41).
	clause.minmax_easy = true;
}

} // namespace duckdb
