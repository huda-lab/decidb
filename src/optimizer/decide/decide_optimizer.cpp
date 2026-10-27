#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/planner/decide/decide_cast_policy.hpp"

#include <cstdlib>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/decide_profile.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"
#include "duckdb/optimizer/decide/decide_solver_gate.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
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
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"

namespace duckdb {

// The rewrite passes below live in decide_rewrite_*.cpp / decide_singleton_row_folding.cpp;
// this file keeps the dispatcher and the helpers they share.
using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

ObjectiveAggregateType decide_rewrite::StrToAggType(const string &name) {
	if (name == "sum") return ObjectiveAggregateType::SUM;
	if (name == "min") return ObjectiveAggregateType::MIN_AGG;
	if (name == "max") return ObjectiveAggregateType::MAX_AGG;
	return ObjectiveAggregateType::NONE;
}

//! Defined below with the other NORM marker helpers; called at the end of OptimizeDecide.
static void VerifyNormMarkersLowered(LogicalDecide &decide);

DecideOptimizer::DecideOptimizer(Optimizer &optimizer) : optimizer(optimizer) {
}

unique_ptr<LogicalOperator> DecideOptimizer::Optimize(unique_ptr<LogicalOperator> plan) {
	OptimizeSubtree(plan, plan);
	return plan;
}

void DecideOptimizer::OptimizeSubtree(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &root) {
	// Recurse into children first (bottom-up)
	for (auto &child : op->children) {
		OptimizeSubtree(child, root);
	}

	// If this is a LogicalDecide node, apply DECIDE-specific optimizations
	if (op->type == LogicalOperatorType::LOGICAL_DECIDE) {
		RewriteRhsAggregates(op->Cast<LogicalDecide>());
		ApplyDirectSolveRules(op, root, optimizer, GetDirectSolveMode(optimizer.context));
		if (op->type == LogicalOperatorType::LOGICAL_DECIDE) {
			OptimizeDecide(op->Cast<LogicalDecide>());
		}
	}
}

void DecideOptimizer::OptimizeDecide(LogicalDecide &decide) {
	DecideProfileScope profile("optimizer.decide");
	bool bench = std::getenv("DECIDB_BENCH") != nullptr;
	Profiler timer;
	if (bench) {
		timer.Start();
	}

	// From here on this plan carries formulation state chosen for THIS host's solver,
	// which is not portable and so is not serialized. Setting the flag before the first
	// rewrite is what makes LogicalDecide::SupportSerialization() stop reporting true,
	// so the post-optimizer PRAGMA verify_serializer round trip skips the node instead
	// of silently replacing the plan with a copy missing everything below.
	decide.optimized = true;

	// Choose the solver, and with it the formulation, BEFORE any rewrite runs. Every
	// pass below decides how to express a construct, and the right answer depends on
	// what the backend can take natively — so both have to be settled first, and settled
	// only once. From here they ride the plan (LogicalDecide::solver_backend_name and
	// ::use_native_constructs → PhysicalDecide) all the way to the solve and to any
	// diagnostic re-solve, so nothing downstream ever decides a second time.
	DecideProfileScope phase("optimizer.choose_backend");
	ChooseDecideSolver(decide);
	phase.Next("optimizer.tag_groups");
	TagAtomicRemovalGroups(decide);

	RewriteNorm(decide);
	phase.Next("optimizer.in_domain");
	RewriteInDomain(decide);
	phase.Next("optimizer.abs");
	TagAbsForBigM(optimizer.context, decide); // Must run before RewriteAbs: marks ABS nodes that need Big-M
	RewriteAbs(decide);          // Must run first: creates aux vars replacing ABS nodes
	phase.Next("optimizer.bilinear");
	RewriteBilinear(decide);     // McCormick linearization for Boolean × anything bilinear products
	phase.Next("optimizer.scoped_reducers");
	RewriteScopedReducers(decide); // The value rewrite: a VALUE-shape clause's reducers become helper decisions
	phase.Next("optimizer.composed_minmax");
	RewriteComposedMinMax(decide); // Detect composed MIN/MAX before single-term MIN/MAX rewrite
	phase.Next("optimizer.minmax");
	RewriteMinMax(decide);       // Classify + rewrite min/max (creates indicators and SUM nodes)
	phase.Next("optimizer.not_equal");
	RewriteNotEqual(decide);
	phase.Next("optimizer.avg");
	RewriteAvgToSum(decide);
	// Must stay last among the rewrites: RewriteInDomain emits a floor-lowering bound
	// that is itself a singleton-row bound, and every auxiliary variable must exist before the
	// box is sized.
	phase.Next("optimizer.fold_singleton_rows");
	FoldSingletonRows(decide);

	VerifyNormMarkersLowered(decide);

	if (bench) {
		timer.End();
		fprintf(stderr, "DECIDB_BENCH: optimizer_ms=%.2f\n", timer.Elapsed() * 1000.0);
	}
}

// ---------------------------------------------------------------------------
// Bound DECIDE syntax markers: NORM and IN
// ---------------------------------------------------------------------------

bool decide_rewrite::TryParseNormMarker(const string &alias, string &payload) {
	auto start = alias.find(NORM_MARKER_TAG_PREFIX);
	if (start == string::npos) {
		return false;
	}
	start += strlen(NORM_MARKER_TAG_PREFIX);
	auto end = alias.find("__", start);
	if (end == string::npos) {
		return false;
	}
	payload = alias.substr(start, end - start);
	return true;
}

void decide_rewrite::CopyClauseProvenanceTags(const string &from_alias, Expression &to) {
	idx_t source_id;
	auto alias = to.GetAlias();
	if (TryParseSourceClauseTag(from_alias, source_id)) {
		AddDecideTag(alias, MakeSourceClauseTag(source_id));
	}
	idx_t removal_id;
	if (TryParseRemovalGroupTag(from_alias, removal_id)) {
		AddDecideTag(alias, MakeRemovalGroupTag(removal_id));
	}
	if (HasDecideTag(from_alias, OBJECTIVE_ROW_TAG)) {
		AddDecideTag(alias, OBJECTIVE_ROW_TAG);
	}
	to.SetAlias(std::move(alias));
}

void decide_rewrite::MarkFormulationConstraint(Expression &expr, const string &source_alias) {
	auto alias = expr.GetAlias();
	AddDecideTag(alias, STRUCTURAL_CONSTRAINT_TAG);
	expr.SetAlias(std::move(alias));
	CopyClauseProvenanceTags(source_alias, expr);
}

idx_t decide_rewrite::AddAuxiliaryDecision(LogicalDecide &decide, const string &name, const LogicalType &type,
                                           bool is_boolean, DecideVarScopeInfo scope, const string &label) {
	idx_t var_idx = decide.decide_variables.size();
	decide.decide_variables.push_back(
	    make_uniq<BoundColumnRefExpression>(name, type, ColumnBinding(decide.decide_index, var_idx)));
	decide.num_auxiliary_vars++;
	decide.is_boolean_var.push_back(is_boolean);
	if (!decide.variable_scopes.empty() || scope.scope != DecideVarScope::ROW) {
		while (decide.variable_scopes.size() < var_idx) {
			decide.variable_scopes.push_back(DecideVarScopeInfo::Row());
		}
		decide.variable_scopes.push_back(scope);
	}
	if (!label.empty()) {
		decide.aux_var_expressions.emplace_back(var_idx, label);
	}
	return var_idx;
}

unique_ptr<Expression> decide_rewrite::MakeDecisionRef(const LogicalDecide &decide, idx_t var_idx) {
	auto &ref = decide.decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
	return make_uniq<BoundColumnRefExpression>(ref.GetName(), ref.return_type, ref.binding);
}

string decide_rewrite::RenderForPlan(const Expression &expr, const LogicalDecide &decide) {
	// The labels of the auxiliaries earlier passes made, so a construct over one prints
	// it as what it stands for rather than by its internal name.
	return RenderDecideSource(expr, DecidePlanNames(decide.source_fragments, decide.entity_scopes, decide.group_keys,
	                                                decide.decide_index, decide.aux_var_expressions));
}

void decide_rewrite::AddRewriteAccount(LogicalDecide &decide, idx_t source_clause_id,
                                       LogicalDecide::RewriteAccount::Kind kind, string subject) {
	LogicalDecide::RewriteAccount account;
	account.source_clause_id = source_clause_id;
	account.kind = kind;
	account.subject = std::move(subject);
	decide.rewrite_accounts.push_back(std::move(account));
}

void decide_rewrite::AddRewriteAccount(LogicalDecide &decide, const string &clause_alias,
                                       LogicalDecide::RewriteAccount::Kind kind, string subject) {
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	TryParseSourceClauseTag(clause_alias, source_clause_id);
	AddRewriteAccount(decide, source_clause_id, kind, std::move(subject));
}

//! The user clause a rewrite is currently standing inside. A comparison carrying a
//! source id starts a new one; every other node inherits its parent's. This is what lets
//! a row emitted deep inside a clause still name the clause it came from.
string decide_rewrite::DescendSourceAlias(const Expression &expr, const string &inherited) {
	idx_t provenance_id;
	return TryParseSourceClauseTag(expr.GetAlias(), provenance_id) ||
	               TryParseRemovalGroupTag(expr.GetAlias(), provenance_id) ||
	               HasDecideTag(expr.GetAlias(), OBJECTIVE_ROW_TAG)
	           ? expr.GetAlias()
	           : inherited;
}

//! Does `expr` hold a NORM marker anywhere beneath it?
//!
//! `l0_only` narrows the answer to the L0 counts: a norm payload is `1`, `2`, `inf`,
//! `0_auto`, or `0_<double>`, so the `0_` prefix selects exactly those and nothing else.
static bool ContainsNormMarker(const Expression &expr, bool l0_only) {
	string payload;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE &&
	    TryParseNormMarker(expr.GetAlias(), payload) && (!l0_only || payload.rfind("0_", 0) == 0)) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found && ContainsNormMarker(child, l0_only)) {
			found = true;
		}
	});
	return found;
}

//! Layer 5's exit contract: a NORM marker is lowered here or nowhere.
//!
//! The marker is not a node downstream can recognise and reject -- the binder builds it
//! as a real `SUM` aggregate and records the order in the alias, so a marker that reaches
//! extraction is read as the plain `SUM(e)` it is made of. The norm silently disappears
//! and the query answers a different question, which is how a marker under arithmetic
//! (`norm(e, 1) + 0.5 * SUM(x)`) went unnoticed. Fail where the owning pass can be named
//! instead of returning a number nothing marks as wrong.
static void VerifyNormMarkersLowered(LogicalDecide &decide) {
	decide.EnumerateExpressions([](unique_ptr<Expression> *expr) {
		if (*expr && ContainsNormMarker(**expr, false)) {
			throw InternalException("DECIDE NORM marker survived optimization in '%s'", (*expr)->ToString());
		}
	});
}

void DecideOptimizer::TagAtomicRemovalGroups(LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	idx_t next_group = 0;
	ForEachConstraintLeaf(*decide.decide_constraints, [&](Expression &node) {
		bool removable = false;
		if (node.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			removable = node.type == ExpressionType::COMPARE_NOTEQUAL || ContainsNormMarker(node, true);
		} else if (node.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
			removable = node.type == ExpressionType::COMPARE_IN;
		}
		if (removable) {
			auto alias = node.GetAlias();
			AddDecideTag(alias, MakeRemovalGroupTag(next_group++));
			node.SetAlias(std::move(alias));
		}
	});
}

void DecideOptimizer::AppendConstraint(LogicalDecide &decide, unique_ptr<Expression> constraint) {
	// The constraint pool belongs to the operator, and so does the invariant that
	// everything in it is canonical. Rewrites keep emitting whatever shape is
	// natural for them (`aux >= inner`, `aux >= 0 - inner`, ...); LogicalDecide
	// puts it into canonical form on the way in.
	decide.AddConstraint(optimizer.context, std::move(constraint));
}

// ---------------------------------------------------------------------------
// Plan-time constant folding
// ---------------------------------------------------------------------------
//
// Why an unresolved scale factor costs performance rather than correctness, and
// where the factor is finally applied, is written up in decide_rewrite_minmax.cpp --
// MIN/MAX is the only construct whose linearization the sign actually changes.

//! A TRUE placeholder standing where a lifted clause used to be.
//!
//! `source_alias` is the alias of the clause it replaces. Carrying the clause id onto
//! the placeholder is what lets EXPLAIN keep the clause in the position it was written:
//! the constraint tree is the only record of written order, and a lifted clause that
//! left an anonymous `true` behind was indistinguishable from a clause that never
//! existed, so it sorted to the end of the plan.
unique_ptr<Expression> decide_rewrite::MakeTrueExpression(const string &source_alias) {
	auto placeholder = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
	CopyClauseProvenanceTags(source_alias, *placeholder);
	return placeholder;
}

ClauseScope decide_rewrite::EnterClauseScope(const Expression &node, ClauseScope scope) {
	if (node.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = node.Cast<BoundConjunctionExpression>();
		if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2) {
			scope.when = conj.children[1].get();
		}
	}
	return scope;
}

unique_ptr<Expression> decide_rewrite::ScopeHelperRow(unique_ptr<Expression> row, const ClauseScope &scope) {
	if (!scope.when) {
		return row;
	}
	auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	wrapper->children.push_back(std::move(row));
	wrapper->children.push_back(scope.when->Copy());
	wrapper->alias = WHEN_CONSTRAINT_TAG;
	return std::move(wrapper);
}

} // namespace duckdb
