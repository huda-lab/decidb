//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/optimizer/decide/decide_rewrite_frames.cpp
//
// Range frames over decisions (language/guide.md §5.6, D78). A range `agg(from F1 to F2 ...: e) over (...)` is a
// reducer whose groups are the frame's windows -- the range several anchors share: a timeline when neither end
// depends on the anchor, else a position on one -- so it becomes a helper decision with one instance per window,
// defined once from what the window reads (LogicalDecide::helper_definitions, with its frame; stage 08 lists each
// window's members from the frame map). It runs first among the rewrites, so every pass after it reads an ordinary
// keyed decision and none copies a range. A point frame is read in place (decide_linear_form.cpp) -- one a product of
// decisions or a square reads is the range of its one position first -- and a range over data is a value of the row.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_shape_check.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/decide/decide_product_form.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

namespace {

//! The frame `expr` is the marker of, when it is a range over decisions; else INVALID_INDEX.
idx_t RangeOverDecisions(const LogicalDecide &decide, const Expression &expr) {
	idx_t frame;
	if (!TryGetDecideFrame(expr, frame) || frame >= decide.bound.frames.size() || !decide.bound.frames[frame].range ||
	    !BoundExpressionReferencesDecide(expr, decide.decide_index)) {
		return DConstants::INVALID_INDEX;
	}
	return frame;
}

bool HoldsRangeOverDecisions(const LogicalDecide &decide, const Expression &expr) {
	if (!BoundExpressionReferencesDecide(expr, decide.decide_index)) {
		return false;
	}
	if (RangeOverDecisions(decide, expr) != DConstants::INVALID_INDEX) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { found = found || HoldsRangeOverDecisions(decide, child); });
	return found;
}

//! The sign each range over decisions in `expr` carries once `expr` is read as a sum (WalkPolarity): through SUM and
//! AVG, whose members it keeps, and through a MIN or a MAX, which does not decrease when a member grows; 0 under a
//! norm, which pins its helper from both sides (D79).
void CollectRangeSigns(ClientContext &context, const LogicalDecide &decide, unique_ptr<Expression> &expr,
                       unordered_map<const Expression *, int> &signs) {
	const BooleanDecisions booleans {decide.decide_index, decide.bound.is_boolean_var};
	PolarityHolds holds = [&](const Expression &node) {
		return HoldsRangeOverDecisions(decide, node);
	};
	PolarityVisit visit;
	visit = [&](unique_ptr<Expression> &node, int node_sign, const PolarityPath &) {
		if (RangeOverDecisions(decide, *node) != DConstants::INVALID_INDEX) {
			signs[node.get()] = node_sign;
			return true;
		}
		if (node->GetExpressionClass() != ExpressionClass::BOUND_DECIDE_REDUCER) {
			return false;
		}
		auto &reducer = node->Cast<BoundDecideReducerExpression>();
		// A norm has no function: it is no sum to look through and no extremum.
		auto name = reducer.function ? StringUtil::Lower(reducer.function->name) : string();
		if (name == "sum" || name == "avg") {
			return false;
		}
		const int member_sign = name == "min" || name == "max" ? node_sign : 0;
		for (auto &child : reducer.children) {
			WalkPolarity(context, booleans, child, member_sign, holds, visit);
		}
		return true;
	};
	WalkPolarity(context, booleans, expr, 1, holds, visit);
}

//! Each point frame over decisions a product of decisions or a square reads -- a term of degree 2 -- becomes the range
//! of its one position, `sum(from F to F: e)`, which reads the same value: that position's payload, its `else` where
//! the position is missing, no value where it has none (D84), one row per position (D73). The lifter then makes it a
//! helper per window, which the product or square reads as a keyed decision (deck p. 51): read in place it would be a
//! decision at another row inside a quadratic term, which no term states.
void PointsInQuadraticTermsAsRanges(ClientContext &context, LogicalDecide &decide, Expression &expr, bool quadratic) {
	idx_t frame;
	if (TryGetDecideFrame(expr, frame)) {
		if (quadratic && frame < decide.bound.frames.size() && !decide.bound.frames[frame].range &&
		    BoundExpressionReferencesDecide(expr, decide.decide_index)) {
			auto &spec = decide.bound.frames[frame];
			spec.range = true;
			spec.reducer = "sum";
			spec.to = spec.from;
			spec.to_k = spec.from_k;
		}
		return;
	}
	bool below = quadratic;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &name = expr.Cast<BoundFunctionExpression>().function.name;
		below = below || (name != "+" && name != "-" && !DecideDegreeOf(context, expr, decide.decide_index).IsLinear());
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](Expression &child) { PointsInQuadraticTermsAsRanges(context, decide, child, below); });
}

//! Replaces each range over decisions in `slot` by its frame's helper decision, made the first time the frame is met,
//! pushed `outer` times the sign `signs` read for it.
class RangeLifter {
public:
	RangeLifter(ClientContext &context, LogicalDecide &decide) : context(context), decide(decide) {
	}

	void Lift(unique_ptr<Expression> &slot, ExtremumPush outer, idx_t source_clause_id,
	          const unordered_map<const Expression *, int> &signs) {
		const idx_t frame = RangeOverDecisions(decide, *slot);
		if (frame == DConstants::INVALID_INDEX) {
			// A reducer's own `when` holds no decision but the NULL test of a range in its argument
			// (DecideCanonicalizer::SkipNullData), which stays the frame's: stage 05 reads it as the frame's DEFINED
			// column.
			if (slot->GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER) {
				for (auto &child : slot->Cast<BoundDecideReducerExpression>().children) {
					Lift(child, outer, source_clause_id, signs);
				}
				return;
			}
			ExpressionIterator::EnumerateChildren(
			    *slot, [&](unique_ptr<Expression> &child) { Lift(child, outer, source_clause_id, signs); });
			return;
		}
		auto sign = signs.find(slot.get());
		const auto push = sign == signs.end() ? ExtremumPush::NONE : ExtremumPushOf(outer, sign->second);
		auto &spec = decide.bound.frames[frame];
		if (spec.reducer == "count") {
			throw InternalException("DECIDE: a count over decisions reached the range rewrite: %s", spec.text);
		}
		bool need_envelope, need_closing;
		DecideHelperSides(spec.reducer, push, need_envelope, need_closing);
		// One helper per frame: a BETWEEN's two comparisons, and every rewrite's copy after this pass, read one value
		// per window. It needs every side any of them does.
		auto made = helper_of_frame.find(frame);
		if (made != helper_of_frame.end()) {
			auto &link = *decide.helper_definitions[made->second];
			link.need_envelope = link.need_envelope || need_envelope;
			link.need_closing = link.need_closing || need_closing;
			ReplaceWithDecision(context, decide, link.helper, slot);
			return;
		}
		auto &marker = slot->Cast<BoundDecideReducerExpression>();
		if (marker.children.size() != 1) {
			throw InternalException("DECIDE: a range frame marker reads no payload: %s", spec.text);
		}
		auto link = make_uniq<DecideHelperDefinition>(DecideHelperKind::REDUCER_VALUE);
		link->agg_name = spec.reducer;
		link->label = spec.spelling;
		link->body = marker.Copy();
		link->need_envelope = need_envelope;
		link->need_closing = need_closing;
		// One instance per window: a timeline, or a position on one. The key names the instances; execution numbers
		// them from the frame map.
		KeyedScopeInfo scope;
		scope.key = spec.timeline_key;
		using Kind = LogicalDecide::RewriteAccount::Kind;
		const auto account = spec.Relative() ? Kind::FRAME_HELPER_PER_POSITION : Kind::FRAME_HELPER_PER_TIMELINE;
		// Whole by the type payload and `else` share (DecideValueIsWholeNumbered reads a frame marker so): what the
		// binder's strict-comparison check read too.
		const bool whole = DecideValueIsWholeNumbered(marker, decide.decide_index);
		const idx_t link_idx = AddValueHelper(decide, std::move(link), scope, whole, source_clause_id, account);
		helper_of_frame[frame] = link_idx;
		ReplaceWithDecision(context, decide, decide.helper_definitions[link_idx]->helper, slot);
	}

private:
	ClientContext &context;
	LogicalDecide &decide;
	unordered_map<idx_t, idx_t> helper_of_frame;
};

} // namespace

void DecideOptimizer::RewriteFrameRanges(LogicalDecide &decide) {
	using Owner = DecideOwnedBody::Owner;
	auto &context = optimizer.context;
	if (!decide.bound.frames.empty()) {
		decide.EnumerateBodies([&](DecideOwnedBody &body) {
			if (body.owner == Owner::CLAUSE || body.owner == Owner::OBJECTIVE) {
				PointsInQuadraticTermsAsRanges(context, decide, *body.expr, false);
			}
		});
	}
	bool any = false;
	for (auto &frame : decide.bound.frames) {
		any = any || frame.range;
	}
	if (!any) {
		return;
	}
	RangeLifter lifter(context, decide);
	bool lifted_objective = false;
	decide.EnumerateBodies([&](DecideOwnedBody &body) {
		if (body.owner == Owner::OBJECTIVE) {
			if (!HoldsRangeOverDecisions(decide, *body.expr)) {
				return;
			}
			// The objective's ranges, pushed down through their signs: the canonicalizer wrote the objective as a
			// minimization, a MAXIMIZE's flip in the signs of its terms. A `when` carries no sign.
			unordered_map<const Expression *, int> signs;
			CollectRangeSigns(context, decide, body.expr, signs);
			lifter.Lift(body.expr, ExtremumPush::DOWN, DConstants::INVALID_INDEX, signs);
			lifted_objective = true;
			return;
		}
		if (body.owner != Owner::CLAUSE || body.expr->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON ||
		    !HoldsRangeOverDecisions(decide, *body.expr)) {
			return;
		}
		// Canonicalization put every decision on the left.
		auto &comparison = body.expr->Cast<BoundComparisonExpression>();
		if (HoldsRangeOverDecisions(decide, *comparison.right)) {
			throw InternalException("DECIDE: a canonical comparison holds a range over decisions on its bound: %s",
			                        comparison.ToString());
		}
		unordered_map<const Expression *, int> signs;
		CollectRangeSigns(context, decide, comparison.left, signs);
		lifter.Lift(comparison.left, ExtremumPushOf(comparison.type), body.Origin().source_clause_id, signs);
	});
	if (lifted_objective) {
		decide.SetObjective(context, std::move(decide.bound.objective));
	}
}

} // namespace duckdb
