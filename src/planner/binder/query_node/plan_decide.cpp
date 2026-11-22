#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_product_form.hpp"
#include "duckdb/planner/decide/decide_shape_check.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/common/enums/decide.hpp"
#include <functional>

namespace duckdb {

unique_ptr<LogicalOperator> Binder::PlanDecide(BoundSelectNode &statement, unique_ptr<LogicalOperator> root) {
	// Which column refs will be ONE value for the whole query once flattening is
	// done. Canonicalization needs this to tell a legal factor on a reducer
	// (`(SELECT k FROM p) * SUM(x)`) from an illegal one (`weight * SUM(x)`, which
	// has no answer to "which row's weight?"). After PlanSubqueries all three of
	// `weight`, an uncorrelated subquery and a CORRELATED subquery are plain column
	// refs — the flattened subquery is even named "SUBQUERY" — so only correlation
	// information captured HERE separates them, and it exists only before
	// flattening. Hold the owning slots: PlanSubqueries replaces the node in place,
	// so the slot is what survives to be read afterward.
	struct SourceSubquerySlot {
		unique_ptr<Expression> *slot;
		//! Where the subquery was written: flattening replaces the node, and the replacement is the same value.
		optional_idx location;
	};
	vector<SourceSubquerySlot> uncorrelated_subquery_slots;
	vector<SourceSubquerySlot> correlated_subquery_slots;
	{
		std::function<void(unique_ptr<Expression> &)> collect_subqueries = [&](unique_ptr<Expression> &slot) {
			if (slot->GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
				auto &subq = slot->Cast<BoundSubqueryExpression>();
				if (subq.subquery_type == SubqueryType::SCALAR) {
					// Correlated ones are recorded only so a rejection can call
					// them a subquery; they stay out of the query-wide set and
					// are refused by the fail-safe default either way.
					(subq.IsCorrelated() ? correlated_subquery_slots : uncorrelated_subquery_slots)
					    .push_back({&slot, slot->GetQueryLocation()});
				}
				return;
			}
			ExpressionIterator::EnumerateChildren(*slot, collect_subqueries);
		};
		if (statement.decide.constraints) {
			collect_subqueries(statement.decide.constraints);
		}
		// The objective passes through the same canonicalization boundary, so it
		// needs the same correlation evidence. Without this an UNCORRELATED
		// subquery scaling a reducer (`(SELECT k) * MAX(x)`) is indistinguishable
		// from a row-varying column after flattening, hits the fail-safe default,
		// and is rejected for varying per row when it does not.
		if (statement.decide.objective) {
			collect_subqueries(statement.decide.objective);
		}
		// A frame's order key and its fallback are data expressions read beside the
		// clause, so a subquery in one carries the same evidence.
		for (auto &frame : statement.decide.frames) {
			for (auto *expr : {&frame.order, &frame.fallback}) {
				if (*expr) {
					collect_subqueries(*expr);
				}
			}
		}
	}

	// Flatten now, even when this select node is itself being planned inside an
	// outer subquery's flattening. Ordinary clauses tolerate PlanSubqueries'
	// `is_outside_flattened` deferral because nothing reads their expressions
	// again until RecursiveDependentJoinPlanner has peeled the outer subquery.
	// A DECIDE clause does: canonicalization runs a few lines below, in this same
	// function, and copies the constraint tree -- and copying a
	// BoundSubqueryExpression throws. Deferring here therefore fails any subquery
	// written inside a nested DECIDE, whether that subquery is itself a DECIDE or
	// a plain SELECT. Layer 4's input contract is a subquery-free bound tree, so
	// the flattening is not optional at this point.
	const bool outer_flatten_state = is_outside_flattened;
	is_outside_flattened = true;
	// The clauses, and the data expressions beside them: a `by` key, a frame's order key and its fallback.
	statement.decide.EnumerateExpressions(
	    [&](unique_ptr<Expression> &expression) { PlanSubqueries(expression, root); });
	is_outside_flattened = outer_flatten_state;

	// Read back what every scalar subquery became. The table-index sets give the
	// strict canonicalizer its planning-time evidence. The expression tags carry
	// the same semantic fact with the flattened value so it survives copies and
	// optimizer-generated re-canonicalization.
	auto collect_flattened_indexes = [](const vector<SourceSubquerySlot> &slots, unordered_set<idx_t> &out) {
		for (auto &entry : slots) {
			auto *slot = entry.slot;
			if (!slot || !*slot) {
				continue;
			}
			// Identity, not value: a flattened subquery is the same column whatever
			// wraps it, so this peels past any cast (see StripCastsForIdentity).
			const Expression *flat = StripCastsForIdentity(**slot);
			if (flat->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				out.insert(flat->Cast<BoundColumnRefExpression>().binding.table_index);
			}
		}
	};
	unordered_set<idx_t> query_wide_table_indexes;
	collect_flattened_indexes(uncorrelated_subquery_slots, query_wide_table_indexes);

	auto tag_flattened_values = [](const vector<SourceSubquerySlot> &slots, const string &tag) {
		for (auto &entry : slots) {
			auto *slot = entry.slot;
			if (!slot || !*slot) {
				continue;
			}
			auto alias = (*slot)->GetAlias();
			if (!HasDecideTag(alias, tag)) {
				AddDecideTag(alias, tag);
			}
			(*slot)->SetAlias(std::move(alias));
			(*slot)->SetQueryLocation(entry.location);
		}
	};
	tag_flattened_values(uncorrelated_subquery_slots, QUERY_WIDE_VALUE_TAG);
	tag_flattened_values(correlated_subquery_slots, ROW_VARYING_SUBQUERY_TAG);
	// Canonicalization: the single point at which the shape of a user-written
	// constraint is decided (decision terms left, data right). Runs here rather
	// than during binding because it needs bound expressions -- a decision
	// variable is exactly a BoundColumnRefExpression on decide_index -- and
	// because it must see the flattened form PlanSubqueries just produced.
	// Everything the optimizer emits later is canonicalized by
	// LogicalDecide::AddConstraint. Those two are the only call sites.
	string written_objective;
	string canonical_objective;
	bool objective_canonical_differs = false;
	DecideSourceNames message_names {statement.decide.source_text, statement.decide.keyed_scopes,
	                                 statement.decide.keys};
	// The variant signatures (D70), from the canonical clauses: one instance per class and distinct value of each
	// scoped expression's fields.
	DecideSignatures signatures {statement.decide.keyed_scopes, {}};
	// Where each frame stands, from the canonical clauses: the rows its timelines read.
	DecideFramePlacement frame_placement {
	    statement.decide.frames, statement.decide.keys, statement.decide.keyed_scopes, {}};
	{
		DecideCanonicalizer canonicalizer(context, statement.decide_index, statement.decide.variable_scopes,
		                                  statement.decide.keyed_scopes, statement.decide.keys,
		                                  statement.decide.is_boolean_var, std::move(query_wide_table_indexes));
		canonicalizer.QuoteMessagesWith(message_names);
		canonicalizer.frames = &statement.decide.frames;
		// The algebra of every clause, judged once on the one spelling canonicalization wrote.
		DecideShapeCheck shape_check(context, statement.decide_index, message_names, statement.decide.frames);
		if (statement.decide.constraints) {
			statement.decide.constraints =
			    canonicalizer.CanonicalizeTree(*statement.decide.constraints, &statement.decide.constraint_sources);
			// Before the walks below, which read a comparison that holds a decision.
			shape_check.CheckConstraints(*statement.decide.constraints);
			canonicalizer.PlaceFrames(*statement.decide.constraints, frame_placement);
			canonicalizer.BuildConstraintSignatures(*statement.decide.constraints, signatures);
			canonicalizer.VerifyCanonical(*statement.decide.constraints);
			FinalizeConstraintSourceInfo(*statement.decide.constraints, statement.decide.constraint_sources,
			                             statement.decide.source_text, statement.decide.keyed_scopes,
			                             statement.decide_index, &statement.decide.keys, statement.decide.frames);
			canonicalizer.SkipNullData(*statement.decide.constraints, statement.decide.keyed_scopes,
			                           statement.decide.keys);
		}
		// The objective is one side of a comparison, so it belongs to the same
		// boundary. It carries no relation to orient, which is the only part of
		// the constraint contract that does not apply to it.
		if (statement.decide.objective) {
			// The objective's counterpart to InitializeConstraintSourceInfo /
			// FinalizeConstraintSourceInfo: written on the way in, canonical on the
			// way out, captured around the one call that can change it. There is
			// exactly one objective, so these are two strings rather than a registry.
			DecideSourceNames objective_names {statement.decide.source_text, statement.decide.keyed_scopes,
			                                   statement.decide.keys};
			objective_names.decide_index = statement.decide_index;
			// Both renderings are the plan's layers: the objective as bound and as canonicalized.
			objective_names.written = false;
			objective_names.frames = &statement.decide.frames;
			// As for a clause: the text the user wrote, cut at the parser's OBJECTIVE span, and DeciDB's
			// renderings before and after canonicalization to tell whether it changed. The rendering stands in
			// for the text only for a statement built without the parser's text.
			auto rendered_objective = RenderDecideObjective(*statement.decide.objective, objective_names);
			if (!statement.decide.source_text.Cut(statement.decide.source_text.tail_start, DecideSpanRole::OBJECTIVE,
			                                      written_objective)) {
				written_objective = rendered_objective;
			}
			statement.decide.objective = canonicalizer.CanonicalizeObjective(
			    *statement.decide.objective, statement.decide.objective_constant_offset, statement.decide.sense);
			shape_check.CheckObjective(*statement.decide.objective);
			canonicalizer.PlaceObjectiveFrames(*statement.decide.objective, frame_placement);
			canonicalizer.BuildObjectiveSignatures(*statement.decide.objective, signatures);
			canonicalizer.VerifyCanonicalObjective(*statement.decide.objective);
			// The canonical objective is a minimization; its layer reads in the sense the user wrote, so the flip
			// alone is no difference to show.
			canonical_objective = RenderDecideObjective(*statement.decide.objective, objective_names,
			                                            statement.decide.sense == DecideSense::MAXIMIZE);
			objective_canonical_differs = canonical_objective != rendered_objective;
			canonicalizer.SkipNullData(*statement.decide.objective, statement.decide.keyed_scopes,
			                           statement.decide.keys);
		}
		// A frame stands in one clause or in the objective, never nowhere: one the walks above missed would read
		// every row as its timeline.
		frame_placement.placed.resize(statement.decide.frames.size(), false);
		for (idx_t i = 0; i < statement.decide.frames.size(); i++) {
			if (!frame_placement.placed[i]) {
				throw InternalException("DECIDE: frame %s is in no clause", statement.decide.frames[i].text);
			}
		}
	}

	statement.decide.signatures = std::move(signatures.signatures);
	statement.decide.objective_signature = signatures.objective;
	auto decide_op = make_uniq<LogicalDecide>(statement.decide_index, std::move(statement.decide));
	decide_op->written_objective = std::move(written_objective);
	decide_op->canonical_objective = std::move(canonical_objective);
	decide_op->objective_canonical_differs = objective_canonical_differs;
	// Capture the user's column names off the BindContext, which is still live here
	// and is the authority name resolution itself used. Every source kind is covered
	// uniformly: a base table's catalog names, a `t(a, b, c)` alias list over
	// `(VALUES ...)`, a subquery's or a CTE's output names. Taken here rather than at
	// physical-plan time because the binding — and with it the only copy of an alias
	// list — is gone by then, leaving the plan's positional `col0` placeholders.
	for (auto &binding : bind_context.GetBindingsList()) {
		for (idx_t i = 0; i < binding->names.size(); i++) {
			decide_op->source_columns.push_back({ColumnBinding(binding->index, i), binding->names[i]});
		}
	}
	// ABS aux vars, MIN/MAX indicator links and objective types are created by DecideOptimizer (runs after plan
	// creation)
	decide_op->AddChild(std::move(root));
	root = std::move(decide_op);
	return root;
}

} // namespace duckdb
