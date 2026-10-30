//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_rewrite_frames.cpp
//
// Range frames over decisions (syntax_reference.md §5.6; the ANR plan's phase 5). A range
// `agg(from F1 to F2 ...: e) over (...)` is a reducer whose groups are the frame's windows
// -- the range several anchors share: a timeline when neither end depends on the anchor,
// else a position on one -- so it becomes a helper decision with one instance per window,
// defined once from what the window reads, through the value rewrite's door
// (AddValueHelper, decide_rewrite_scoped.cpp). It runs first among the rewrites: every pass
// after it reads an ordinary keyed decision, and none copies a range. A point frame is read
// in place (decide_linear_form.cpp), and a range over data is a value of the row.
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
#include "duckdb/planner/operator/decide/logical_decide.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

namespace {

//! Replace the range at `site`, pushed `push`, by its helper decision, recording what it
//! holds: its payload, and through its frame its `else` and its windows.
void LiftRange(LogicalDecide &decide, const DecisionSite &site, ExtremumPush push, idx_t source_clause_id,
               idx_t removal_group_id) {
	auto &marker = (*site.slot)->Cast<BoundAggregateExpression>();
	idx_t frame_idx;
	if (!TryGetDecideFrame(marker, frame_idx) || marker.children.size() != 1) {
		throw InternalException("DECIDE: a range frame site holds no range marker");
	}
	auto &frame = decide.frames[frame_idx];
	if (frame.reducer == "count") {
		throw InternalException("DECIDE: a count over decisions reached the range rewrite: %s", frame.text);
	}
	// Placement read the clause id off the same comparison, before any rewrite ran.
	if (frame.source_clause_id != source_clause_id) {
		throw InternalException("DECIDE: %s stands in clause %llu but was placed in clause %llu", frame.text,
		                        source_clause_id, frame.source_clause_id);
	}

	LogicalDecide::ReducerValueLink link;
	link.agg_name = frame.reducer;
	link.label = frame.text;
	link.inner_expr = marker.children[0]->Copy();
	link.frame = frame_idx;
	link.source_clause_id = source_clause_id;
	link.removal_group_id = removal_group_id;
	ExtremumSides(link.agg_name, push, link.need_envelope, link.need_closing);
	// One entity per window, named by what splits the windows: the timeline's keys, and the
	// order key when a window is a position on one.
	auto scope = make_uniq<EntityScopeInfo>();
	scope->table_alias = frame.text;
	scope->key_names = frame.timeline_names;
	if (frame.Relative()) {
		scope->key_names.push_back(frame.order_name);
	}
	// Whole by the type payload and `else` share (DecideValueIsWholeNumbered reads a frame
	// marker so): what the binder's strict-comparison check read too.
	const bool whole = DecideValueIsWholeNumbered(marker, decide.decide_index);
	const auto account = frame.Relative() ? LogicalDecide::RewriteAccount::Kind::FRAME_HELPER_PER_POSITION
	                                      : LogicalDecide::RewriteAccount::Kind::FRAME_HELPER_PER_TIMELINE;
	const auto link_idx = AddValueHelper(decide, std::move(link), std::move(scope), whole, account);
	*site.slot = MakeDecisionRef(decide, decide.reducer_value_links[link_idx].value_var);
}

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
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		idx_t removal_group_id = DConstants::INVALID_INDEX;
		TryParseSourceClauseTag(expr->GetAlias(), source_clause_id);
		TryParseRemovalGroupTag(expr->GetAlias(), removal_group_id);
		vector<DecisionSite> sites;
		auto push = ExtremumPush::NONE;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			// Canonicalization put every decision on the left.
			auto &cmp = expr->Cast<BoundComparisonExpression>();
			push = ExtremumPushOf(cmp.type);
			CollectDecisionSites(cmp.left, 1, DecisionSiteKind::RANGES, context, decide, sites);
			vector<DecisionSite> bound_sites;
			CollectDecisionSites(cmp.right, 1, DecisionSiteKind::RANGES, context, decide, bound_sites);
			if (!bound_sites.empty()) {
				throw InternalException("DECIDE: a canonical comparison holds a range over decisions on its bound: %s",
				                        cmp.ToString());
			}
		} else {
			CollectDecisionSites(expr, 1, DecisionSiteKind::RANGES, context, decide, sites);
		}
		for (auto &site : sites) {
			LiftRange(decide, site, ExtremumPushOf(push, site.sign), source_clause_id, removal_group_id);
		}
	};
	if (decide.decide_constraints) {
		rewrite(decide.decide_constraints);
	}
	if (!decide.decide_objective) {
		return;
	}
	// The objective's ranges, pushed the way its sense pushes it.
	auto *body = &decide.decide_objective;
	if ((*body)->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = (*body)->Cast<BoundConjunctionExpression>();
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			body = &conj.children[0];
		}
	}
	vector<DecisionSite> sites;
	CollectDecisionSites(*body, 1, DecisionSiteKind::RANGES, context, decide, sites);
	const auto sense = decide.decide_sense == DecideSense::MAXIMIZE ? ExtremumPush::UP : ExtremumPush::DOWN;
	for (auto &site : sites) {
		LiftRange(decide, site, ExtremumPushOf(sense, site.sign), DConstants::INVALID_INDEX, DConstants::INVALID_INDEX);
	}
	if (!sites.empty()) {
		auto objective = std::move(decide.decide_objective);
		decide.SetObjective(context, std::move(objective));
	}
}

} // namespace duckdb
