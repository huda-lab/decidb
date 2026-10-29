#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/planner/decide/decide_cast_policy.hpp"

#include <cstdlib>
#include <map>
#include <set>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/decide_profile.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"
#include "duckdb/optimizer/decide/decide_answer_choice.hpp"
#include "duckdb/optimizer/decide/direct/direct_row_checks.hpp"
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
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
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
static void VerifyExtremumBodiesLinear(LogicalDecide &decide);

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
		OptimizeDecide(op, root);
	}
}

namespace {

//! One optimizer pass under a profile span of its own, so a span times the pass it names.
template <class PASS>
void RunPass(const char *name, PASS &&pass) {
	DecideProfileScope profile(name);
	pass();
}

} // namespace

void DecideOptimizer::OptimizeDecide(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &root) {
	DecideProfileScope profile("optimizer.decide");
	bool bench = std::getenv("DECIDB_BENCH") != nullptr;
	Profiler timer;
	if (bench) {
		timer.Start();
	}
	auto &decide = op->Cast<LogicalDecide>();

	// Pass 0, on every path: the bound-side evaluator, whose output both the direct-solve rules and the solver read.
	RunPass("optimizer.rhs_aggregates", [&] { RewriteRhsAggregates(decide); });

	// How the query is answered is settled here and nowhere else: a direct-solve rule replaces the node, else a solver
	// is chosen, and with it the formulation. Every pass below decides how to express a construct, and the right
	// answer depends on what the backend can take natively, so the backend is settled first, and settled only once.
	// From here it rides the plan (LogicalDecide::solver_backend_name and ::use_native_constructs → PhysicalDecide)
	// all the way to the solve and to any diagnostic re-solve, so nothing downstream ever decides a second time.
	bool replaced = false;
	RunPass("optimizer.choose_answer", [&] { replaced = ChooseDecideAnswer(op, root, optimizer); });
	if (!replaced) {
		RewriteForSolver(decide);
	}

	if (bench) {
		timer.End();
		fprintf(stderr, "DECIDB_BENCH: optimizer_ms=%.2f\n", timer.Elapsed() * 1000.0);
	}
}

void DecideOptimizer::InstallRowChecks(LogicalDecide &decide) {
	auto checks = DirectTakeRowChecks(optimizer.context, decide);
	decide.children[0]->ResolveOperatorTypes();
	decide.children[0] = DirectAddRowChecks(optimizer, checks, std::move(decide.children[0]));
}

void DecideOptimizer::RewriteForSolver(LogicalDecide &decide) {
	RunPass("optimizer.install_bound_windows", [&] { InstallReducedBounds(decide); });
	RunPass("optimizer.install_row_checks", [&] { InstallRowChecks(decide); });
	RunPass("optimizer.tag_groups", [&] { TagAtomicRemovalGroups(decide); });
	RunPass("optimizer.norm", [&] { RewriteNorm(decide); });
	RunPass("optimizer.in_domain", [&] { RewriteInDomain(decide); });
	// Must run before RewriteAbs: marks ABS nodes that need Big-M
	RunPass("optimizer.tag_abs", [&] { TagAbsForBigM(optimizer.context, decide); });
	// Creates aux vars replacing ABS nodes
	RunPass("optimizer.abs", [&] { RewriteAbs(decide); });
	// McCormick linearization for Boolean × anything bilinear products
	RunPass("optimizer.bilinear", [&] { RewriteBilinear(decide); });
	// A non-linear reducer body a MIN/MAX reads becomes a helper per row
	RunPass("optimizer.define_bodies", [&] { DefineNonlinearReducerBodies(decide); });
	// The value rewrite: a VALUE-shape clause's reducers become helper decisions
	RunPass("optimizer.scoped_reducers", [&] { RewriteScopedReducers(decide); });
	// Detect composed MIN/MAX before single-term MIN/MAX rewrite
	RunPass("optimizer.composed_minmax", [&] { RewriteComposedMinMax(decide); });
	// Classify + rewrite min/max (creates indicators and SUM nodes)
	RunPass("optimizer.minmax", [&] {
		RewriteMinMax(decide);
		VerifyExtremumBodiesLinear(decide);
	});
	RunPass("optimizer.not_equal", [&] { RewriteNotEqual(decide); });
	RunPass("optimizer.avg", [&] { RewriteAvgToSum(decide); });
	// Must stay last among the rewrites: RewriteInDomain emits a floor-lowering bound
	// that is itself a singleton-row bound, and every auxiliary variable must exist before the
	// box is sized.
	RunPass("optimizer.fold_singleton_rows", [&] { FoldSingletonRows(decide); });

	VerifyNormMarkersLowered(decide);
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

DecideVarScopeInfo decide_rewrite::JoinScopes(const DecideVarScopeInfo &a, const DecideVarScopeInfo &b) {
	// The key lattice (JoinDecideKeys) over the scopes themselves: a keyed scope stands for its key.
	auto key_of = [](const DecideVarScopeInfo &scope) {
		return scope.IsGlobal()   ? DConstants::INVALID_INDEX
		       : scope.IsKeyed() ? scope.entity_scope_idx
		                          : DECIDE_ROW_KEY;
	};
	auto joined = JoinDecideKeys(key_of(a), key_of(b));
	return joined == key_of(a) ? a : joined == key_of(b) ? b : DecideVarScopeInfo::Row();
}

DecideVarScopeInfo decide_rewrite::ScopeOf(const LogicalDecide &decide, const DecideCanonicalizer &canonicalizer,
                                           const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		if (ref.binding.table_index == decide.decide_index) {
			auto var_idx = ref.binding.column_index;
			return var_idx < decide.variable_scopes.size() ? decide.variable_scopes[var_idx]
			                                               : DecideVarScopeInfo::Row();
		}
	}
	if (canonicalizer.IsQueryWideConstant(expr)) {
		return DecideVarScopeInfo::Global();
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return DecideVarScopeInfo::Row();
	}
	bool saw_child = false;
	auto scope = DecideVarScopeInfo::Global();
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		saw_child = true;
		scope = JoinScopes(scope, ScopeOf(decide, canonicalizer, child));
	});
	return saw_child ? scope : DecideVarScopeInfo::Row();
}

unique_ptr<Expression> decide_rewrite::MakeDecisionRef(const LogicalDecide &decide, idx_t var_idx) {
	auto &ref = decide.decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
	return make_uniq<BoundColumnRefExpression>(ref.GetName(), ref.return_type, ref.binding);
}

void decide_rewrite::ReplaceWithDecision(ClientContext &context, const LogicalDecide &decide, idx_t var_idx,
                                         unique_ptr<Expression> &replaced) {
	auto type = replaced->return_type;
	replaced = BoundCastExpression::AddCastToType(context, MakeDecisionRef(decide, var_idx), type);
}

string decide_rewrite::RenderForPlan(const Expression &expr, const LogicalDecide &decide) {
	// The labels of the auxiliaries earlier passes made, so a construct over one prints
	// it as what it stands for rather than by its internal name.
	return RenderDecideSource(expr, DecidePlanNames(decide.source_fragments, decide.entity_scopes, decide.keys,
	                                                decide.decide_index, decide.aux_var_expressions));
}

idx_t decide_rewrite::SourceClauseOf(const Expression &comparison, const LogicalDecide &decide) {
	idx_t source_id;
	if (!TryParseSourceClauseTag(comparison.GetAlias(), source_id) || source_id >= decide.constraint_sources.size()) {
		throw InternalException("DECIDE: a comparison carries no source clause");
	}
	return source_id;
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

//! The MIN/MAX rewrites' exit contract: every member an extremum reads is linear.
//!
//! The hard rows, the composed terms, the value links and the MIN/MAX objectives all read a
//! member as a linear form; a square there is dropped without a word and the extremum is of
//! something else. DefineNonlinearReducerBodies names every such body before they run, so
//! a non-linear member here is a body that pass missed. The cheap half of a MIN/MAX
//! (MINMAX_EASY_REWRITE_TAG) bounds each member in its own quadratic row and is exempt.
static void VerifyExtremumBodiesLinear(LogicalDecide &decide) {
	const idx_t decide_index = decide.decide_index;
	auto check = [&](const Expression &body) {
		if (!DecideExpressionDegree(body, decide_index).IsLinear()) {
			throw InternalException("DECIDE MIN/MAX member '%s' is not linear", body.ToString());
		}
	};
	auto is_extremum = [](const string &name) {
		return name == "min" || name == "max";
	};
	for (auto &constraint : decide.composed_minmax_constraints) {
		for (auto &term : constraint.terms) {
			check(*term.inner_expr);
		}
	}
	for (auto &term : decide.composed_minmax_objective_terms) {
		if (is_extremum(term.agg_name)) {
			check(*term.inner_expr);
		}
	}
	for (auto &link : decide.reducer_value_links) {
		if (is_extremum(link.agg_name)) {
			check(*link.inner_expr);
		}
	}
	std::function<void(const Expression &)> hard_rows = [&](const Expression &expr) {
		if (HasDecideTag(expr.GetAlias(), MINMAX_EASY_REWRITE_TAG)) {
			return;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE &&
		    expr.GetAlias().find(MINMAX_CLAUSE_TAG_PREFIX) != string::npos) {
			for (auto &child : expr.Cast<BoundAggregateExpression>().children) {
				check(*child);
			}
			return;
		}
		ExpressionIterator::EnumerateChildren(expr, hard_rows);
	};
	if (decide.decide_constraints) {
		hard_rows(*decide.decide_constraints);
	}
	auto is_extremum_agg = [](ObjectiveAggregateType agg) {
		return agg == ObjectiveAggregateType::MIN_AGG || agg == ObjectiveAggregateType::MAX_AGG;
	};
	if (decide.decide_objective && (is_extremum_agg(decide.flat_objective_agg) ||
	                                is_extremum_agg(decide.per_inner_agg) || is_extremum_agg(decide.per_outer_agg))) {
		std::function<void(const Expression &)> bodies = [&](const Expression &expr) {
			if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
				for (auto &child : expr.Cast<BoundAggregateExpression>().children) {
					check(*child);
				}
				return;
			}
			ExpressionIterator::EnumerateChildren(expr, bodies);
		};
		bodies(*decide.decide_objective);
	}
}

void DecideOptimizer::TagAtomicRemovalGroups(LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	auto is_removable = [](const Expression &node) {
		if (node.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			return node.type == ExpressionType::COMPARE_NOTEQUAL || ContainsNormMarker(node, true);
		}
		return node.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR && node.type == ExpressionType::COMPARE_IN;
	};
	// The clause a leaf belongs to, named by its first source id: a BETWEEN is one clause of two leaves, which
	// diagnosis removes together or not at all. INVALID_INDEX for a leaf with no source clause.
	auto clause_of = [&](const Expression &node) {
		idx_t source_id;
		if (!TryParseSourceClauseTag(node.GetAlias(), source_id) || source_id >= decide.constraint_sources.size()) {
			return DConstants::INVALID_INDEX;
		}
		auto &source = decide.constraint_sources[source_id];
		return source.between_upper ? source.between_partner : source_id;
	};
	std::set<idx_t> removable_clauses;
	ForEachConstraintLeaf(*decide.decide_constraints, [&](Expression &node) {
		if (is_removable(node) && clause_of(node) != DConstants::INVALID_INDEX) {
			removable_clauses.insert(clause_of(node));
		}
	});
	idx_t next_group = 0;
	std::map<idx_t, idx_t> group_of_clause;
	ForEachConstraintLeaf(*decide.decide_constraints, [&](Expression &node) {
		auto clause = clause_of(node);
		bool removable = clause == DConstants::INVALID_INDEX ? is_removable(node) : removable_clauses.count(clause) > 0;
		if (!removable) {
			return;
		}
		idx_t group = next_group;
		if (clause != DConstants::INVALID_INDEX) {
			group = group_of_clause.emplace(clause, next_group).first->second;
		}
		next_group = std::max(next_group, group + 1);
		auto alias = node.GetAlias();
		AddDecideTag(alias, MakeRemovalGroupTag(group));
		node.SetAlias(std::move(alias));
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

unique_ptr<Expression> decide_rewrite::ScopeHelperRow(unique_ptr<Expression> row, const Expression *when) {
	if (!when) {
		return row;
	}
	auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	wrapper->children.push_back(std::move(row));
	wrapper->children.push_back(when->Copy());
	wrapper->alias = WHEN_CONSTRAINT_TAG;
	return std::move(wrapper);
}

unique_ptr<Expression> decide_rewrite::ScopeRowPerInstance(const LogicalDecide &decide, unique_ptr<Expression> row,
                                                           const DecideVarScopeInfo &scope) {
	if (!scope.IsGlobal() && !scope.IsKeyed()) {
		return row;
	}
	auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	wrapper->children.push_back(std::move(row));
	wrapper->alias = PER_CLASS_TAG;
	if (scope.IsKeyed()) {
		AddDecideTag(wrapper->alias, MakePerKeyTag(decide.entity_scopes[scope.entity_scope_idx].key));
	}
	return std::move(wrapper);
}

void decide_rewrite::WalkPolarity(ClientContext &context, Expression &expr, int sign, const PolarityHolds &holds,
                                  const PolarityVisit &visit) {
	if (!holds(expr) || visit(expr, sign)) {
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		WalkPolarity(context, *expr.Cast<BoundCastExpression>().child, sign, holds, visit);
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		auto name = StringUtil::Lower(func.function.name);
		if (name == "+") {
			for (auto &child : func.children) {
				WalkPolarity(context, *child, sign, holds, visit);
			}
			return;
		}
		if (name == "-" && func.children.size() == 2) {
			WalkPolarity(context, *func.children[0], sign, holds, visit);
			WalkPolarity(context, *func.children[1], -sign, holds, visit);
			return;
		}
		if (name == "-" && func.children.size() == 1) {
			WalkPolarity(context, *func.children[0], -sign, holds, visit);
			return;
		}
		if (name == "*" || name == "/") {
			// Fold the sign of every factor beside the held one, read before the query runs
			// (PlanTimeSignOf, which sees through products: `0 * w` is zero whatever the data
			// column `w` holds). For division only the numerator can hold the term, but the
			// divisor's sign still applies. A held factor is the term being walked rather
			// than a multiplier, and never flips.
			//
			// A factor whose sign is not known until execution -- `w` in `SUM(w * ABS(x - t))`
			// -- makes the whole path sign 0. A factor that is exactly zero makes the term
			// vanish: nothing pushes it anywhere, so it is not walked.
			int factor_sign = 1;
			for (idx_t i = 0; i < func.children.size(); i++) {
				auto &child = *func.children[i];
				if (holds(child)) {
					continue;
				}
				auto child_sign = PlanTimeSignOf(context, child);
				const bool is_divisor = name == "/" && i == 1;
				if (child_sign == PlanTimeSign::ZERO && !is_divisor) {
					return;
				}
				if (child_sign == PlanTimeSign::UNKNOWN || child_sign == PlanTimeSign::ZERO) {
					factor_sign = 0;
				} else if (child_sign == PlanTimeSign::NEGATIVE) {
					factor_sign = -factor_sign;
				}
			}
			for (auto &child : func.children) {
				WalkPolarity(context, *child, sign * factor_sign, holds, visit);
			}
			return;
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		// SUM, AVG, MIN and MAX never decrease when a member grows, so a member carries the
		// reducer's sign.
		for (auto &child : expr.Cast<BoundAggregateExpression>().children) {
			WalkPolarity(context, *child, sign, holds, visit);
		}
		return;
	}
	// Anything else (subqueries, CASE, operators with no sign discipline): the sign of the
	// path is unknown.
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](Expression &child) { WalkPolarity(context, child, 0, holds, visit); });
}

} // namespace duckdb
