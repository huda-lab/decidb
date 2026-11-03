//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/optimizer/decide/decide_rewrite_frames.cpp
//
// Range frames over decisions (language/guide.md §5.6, D78). A range `agg(from F1 to F2 ...: e) over (...)` is a
// reducer whose groups are the frame's windows -- the range several anchors share: a timeline when neither end
// depends on the anchor, else a position on one -- so it becomes a helper decision with one instance per window,
// defined once from what the window reads (LogicalDecide::reducer_value_links, with its frame; stage 08 lists each
// window's members from the frame map). It runs first among the rewrites, so every pass after it reads an ordinary
// keyed decision and none copies a range. A point frame is read in place (decide_linear_form.cpp), and a range over
// data is a value of the row.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

namespace {

//! The frame `expr` is the marker of, when it is a range over decisions; else INVALID_INDEX.
idx_t RangeOverDecisions(const LogicalDecide &decide, const Expression &expr) {
	idx_t frame;
	if (!TryGetDecideFrame(expr, frame) || frame >= decide.frames.size() || !decide.frames[frame].range ||
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
//! AVG, whose members it keeps, and 0 under a MIN, a MAX or a norm, which pins its helper from both sides (D79).
void CollectRangeSigns(ClientContext &context, const LogicalDecide &decide, Expression &expr, int sign,
                       unordered_map<const Expression *, int> &signs) {
	PolarityHolds holds = [&](const Expression &node) {
		return HoldsRangeOverDecisions(decide, node);
	};
	PolarityVisit visit;
	visit = [&](Expression &node, int node_sign) {
		if (RangeOverDecisions(decide, node) != DConstants::INVALID_INDEX) {
			signs[&node] = node_sign;
			return true;
		}
		if (node.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return false;
		}
		auto &reducer = node.Cast<BoundAggregateExpression>();
		auto name = StringUtil::Lower(reducer.function.name);
		string norm;
		if ((name == "sum" || name == "avg") && !TryParseNormMarker(reducer.GetAlias(), norm)) {
			return false;
		}
		for (auto &child : reducer.children) {
			WalkPolarity(context, *child, 0, holds, visit);
		}
		return true;
	};
	WalkPolarity(context, expr, sign, holds, visit);
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
			if (slot->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
				for (auto &child : slot->Cast<BoundAggregateExpression>().children) {
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
		auto &spec = decide.frames[frame];
		if (spec.reducer == "count") {
			throw InternalException("DECIDE: a count over decisions reached the range rewrite: %s", spec.text);
		}
		bool need_envelope, need_closing;
		DecideHelperSides(spec.reducer, push, need_envelope, need_closing);
		// One helper per frame: a BETWEEN's two comparisons, and every rewrite's copy after this pass, read one value
		// per window. It needs every side any of them does.
		auto made = helper_of_frame.find(frame);
		if (made != helper_of_frame.end()) {
			auto &link = decide.reducer_value_links[made->second];
			link.need_envelope = link.need_envelope || need_envelope;
			link.need_closing = link.need_closing || need_closing;
			ReplaceWithDecision(context, decide, link.value_var, slot);
			return;
		}
		auto &marker = slot->Cast<BoundAggregateExpression>();
		if (marker.children.size() != 1) {
			throw InternalException("DECIDE: a range frame marker reads no payload: %s", spec.text);
		}
		LogicalDecide::ReducerValueLink link;
		link.agg_name = spec.reducer;
		link.label = spec.text;
		link.inner_expr = marker.children[0]->Copy();
		link.frame = frame;
		link.need_envelope = need_envelope;
		link.need_closing = need_closing;
		// One instance per window: a timeline, or a position on one. The key names the instances; execution numbers
		// them from the frame map.
		KeyedScopeInfo scope;
		scope.key = spec.timeline_key;
		using Kind = LogicalDecide::RewriteAccount::Kind;
		const auto account = spec.Relative() ? Kind::FRAME_HELPER_PER_POSITION : Kind::FRAME_HELPER_PER_TIMELINE;
		// Whole by the type payload and `else` share (DecideValueIsWholeNumbered reads a frame marker so): what the
		// binder's strict-comparison check read too.
		const bool whole = DecideValueIsWholeNumbered(marker, decide.decide_index);
		const idx_t link_idx = AddValueHelper(decide, std::move(link), &scope, whole, source_clause_id, account);
		helper_of_frame[frame] = link_idx;
		ReplaceWithDecision(context, decide, decide.reducer_value_links[link_idx].value_var, slot);
	}

private:
	ClientContext &context;
	LogicalDecide &decide;
	unordered_map<idx_t, idx_t> helper_of_frame;
};

} // namespace

void DecideOptimizer::RewriteFrameRanges(LogicalDecide &decide) {
	bool any = false;
	for (auto &frame : decide.frames) {
		any = any || frame.range;
	}
	if (!any) {
		return;
	}
	auto &context = optimizer.context;
	RangeLifter lifter(context, decide);
	std::function<void(unique_ptr<Expression> &)> rewrite = [&](unique_ptr<Expression> &expr) {
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			for (idx_t i = 0; i < conj.children.size(); i++) {
				if (IsConstraintChild(conj, i)) {
					rewrite(conj.children[i]);
				}
			}
			return;
		}
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON ||
		    !HoldsRangeOverDecisions(decide, *expr)) {
			return;
		}
		// Canonicalization put every decision on the left.
		auto &comparison = expr->Cast<BoundComparisonExpression>();
		if (HoldsRangeOverDecisions(decide, *comparison.right)) {
			throw InternalException("DECIDE: a canonical comparison holds a range over decisions on its bound: %s",
			                        comparison.ToString());
		}
		unordered_map<const Expression *, int> signs;
		CollectRangeSigns(context, decide, *comparison.left, 1, signs);
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		TryParseSourceClauseTag(comparison.GetAlias(), source_clause_id);
		lifter.Lift(comparison.left, ExtremumPushOf(comparison.type), source_clause_id, signs);
	};
	if (decide.decide_constraints) {
		rewrite(decide.decide_constraints);
	}
	if (!decide.decide_objective || !HoldsRangeOverDecisions(decide, *decide.decide_objective)) {
		return;
	}
	// The objective's ranges, pushed the way its sense pushes it; a `when` carries no sign.
	auto objective = std::move(decide.decide_objective);
	auto *body = &objective;
	while ((*body)->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = (*body)->Cast<BoundConjunctionExpression>();
		if (!IsConstraintWrapper(conjunction) || conjunction.children.empty()) {
			break;
		}
		body = &conjunction.children[0];
	}
	unordered_map<const Expression *, int> signs;
	CollectRangeSigns(context, decide, **body, 1, signs);
	const auto sense = decide.decide_sense == DecideSense::MINIMIZE ? ExtremumPush::DOWN : ExtremumPush::UP;
	lifter.Lift(*body, sense, DConstants::INVALID_INDEX, signs);
	decide.SetObjective(context, std::move(objective));
}

} // namespace duckdb
