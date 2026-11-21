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
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"

namespace duckdb {

// The rewrite passes below live in decide_rewrite_*.cpp / decide_singleton_row_folding.cpp;
// this file keeps the dispatcher and the helpers they share.
using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

//! Defined below with the other NORM marker helpers; called at the end of OptimizeDecide.
static void VerifyNormMarkersLowered(LogicalDecide &decide);
static void VerifyExtremumBodiesLinear(ClientContext &context, LogicalDecide &decide);

DecideOptimizer::DecideOptimizer(Optimizer &optimizer) : optimizer(optimizer) {
}

unique_ptr<LogicalOperator> DecideOptimizer::Optimize(unique_ptr<LogicalOperator> plan) {
	OptimizeSubtree(plan, plan);
	return plan;
}

void DecideOptimizer::OptimizeSubtree(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &root) {
	for (auto &child : op->children) {
		OptimizeSubtree(child, root);
	}

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
	auto &decide = op->Cast<LogicalDecide>();

	// The objective's sums of group sums of squares, summed in the other order: their weights are reducers over data
	// the bound-side evaluator computes.
	RunPass("optimizer.objective_summation", [&] { SwapObjectiveSummation(decide); });
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
		RewriteForSolver(decide, *root);
	}
}

void DecideOptimizer::RewriteForSolver(LogicalDecide &decide, LogicalOperator &root) {
	RunPass("optimizer.tag_groups", [&] { TagAtomicRemovalGroups(decide); });
	// A range over decisions becomes a helper before any rewrite copies it.
	RunPass("optimizer.frame_ranges", [&] { RewriteFrameRanges(decide); });
	// Nested reducers remain in their written positions; the tree lowers them as G nodes beneath their readers.
	RunPass("optimizer.norm", [&] { RewriteNorm(decide); });
	RunPass("optimizer.in_domain", [&] { RewriteInDomain(decide); });
	// McCormick linearization for Boolean × anything bilinear products
	RunPass("optimizer.bilinear", [&] { RewriteBilinear(decide); });
	// A MIN/MAX in its cheap direction keeps its reducer, tagged, before any pass reads the clause's direction.
	RunPass("optimizer.minmax", [&] { RewriteMinMax(decide); });
	// Reserve the plan-time columns and F-node specs before later MIN/MAX rewrites and term splitting consume them.
	// The tree owns the definition functions and emits their E rows when its instances are known.
	DecideTreeConstruction::PrepareDefinitionFunctions(optimizer.context, decide);
	// The value rewrite: a VALUE-shape clause's reducers, and every other MIN/MAX, become helper decisions
	RunPass("optimizer.scoped_reducers", [&] { RewriteScopedReducers(decide); });
	// Reducers now nested in retained G bodies are decision variables; tag any products those substitutions exposed.
	RunPass("optimizer.bilinear_nested_values", [&] { RewriteBilinear(decide); });
	VerifyExtremumBodiesLinear(optimizer.context, decide);
	RunPass("optimizer.not_equal", [&] { RewriteNotEqual(decide); });
	RunPass("optimizer.avg", [&] { RewriteAvgToSum(decide); });
	// Last of the rewrites that join a helper's readers: each ABS takes its encoding from the push its body's owner
	// gives it.
	RunPass("optimizer.abs", [&] { RewriteAbs(decide); });
	// Products outside ABS need the helper it just allocated; the first bilinear pass has already handled products
	// inside ABS's operand and everywhere else.
	RunPass("optimizer.bilinear_abs_factors", [&] { RewriteBilinear(decide); });
	// Must stay last among the rewrites: RewriteInDomain emits a floor-lowering bound
	// that is itself a singleton-row bound, and every auxiliary variable must exist before the
	// box is sized.
	RunPass("optimizer.fold_singleton_rows", [&] { FoldSingletonRows(decide); });

	VerifyNormMarkersLowered(decide);
}

// ---------------------------------------------------------------------------
// Bound DECIDE syntax markers: NORM and IN
// ---------------------------------------------------------------------------

unique_ptr<Expression> decide_rewrite::BindSumOver(Optimizer &optimizer, unique_ptr<Expression> body,
                                                   const Expression *filter) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(body));
	auto sum = optimizer.BindAggregateFunction("sum", std::move(children));
	if (filter) {
		sum->Cast<BoundAggregateExpression>().filter = filter->Copy();
	}
	return sum;
}

bool decide_rewrite::TryParseNormMarker(const string &alias, string &payload) {
	auto start = alias.find(NORM_MARKER_TAG_PREFIX);
	if (start == string::npos) {
		return false;
	}
	start += strlen(NORM_MARKER_TAG_PREFIX);
	auto end = alias.find("__", start);
	if (end == string::npos) {
		throw InternalException("DECIDE: NORM marker '%s' has no closing '__'; the canonicalizer promises to write it",
		                        alias);
	}
	payload = alias.substr(start, end - start);
	return true;
}

void decide_rewrite::AddValueCheck(LogicalDecide &decide, unique_ptr<Expression> check) {
	if (!check) {
		return;
	}
	// Expression::Equals ignores the place a check carries (its query location): two checks of one value stand once.
	for (auto &stood : decide.value_checks) {
		if (stood->Equals(*check)) {
			return;
		}
	}
	decide.value_checks.push_back(std::move(check));
}

void decide_rewrite::CopySourceClauseTag(const string &from_alias, Expression &to) {
	idx_t source_id;
	if (TryParseSourceClauseTag(from_alias, source_id)) {
		auto alias = to.GetAlias();
		AddDecideTag(alias, MakeSourceClauseTag(source_id));
		to.SetAlias(std::move(alias));
	}
}

void decide_rewrite::CopyClauseProvenanceTags(const string &from_alias, Expression &to) {
	CopySourceClauseTag(from_alias, to);
	auto alias = to.GetAlias();
	idx_t removal_id;
	if (TryParseRemovalGroupTag(from_alias, removal_id)) {
		AddDecideTag(alias, MakeRemovalGroupTag(removal_id));
	}
	if (HasDecideTag(from_alias, OBJECTIVE_ROW_TAG)) {
		AddDecideTag(alias, OBJECTIVE_ROW_TAG);
	}
	to.SetAlias(std::move(alias));
}

void decide_rewrite::MarkFormulationConstraint(Expression &expr, const string &source_alias, idx_t defined) {
	auto alias = expr.GetAlias();
	AddDecideTag(alias, STRUCTURAL_CONSTRAINT_TAG);
	if (defined != DConstants::INVALID_INDEX) {
		AddDecideTag(alias, MakeDefinesTag(defined));
	}
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
	if (decide.variable_scopes.size() != var_idx) {
		throw InternalException(
		    "DECIDE: %llu scopes for %llu decisions; the binder promises one scope per decision and "
		    "every rewrite adds a scope with its decision",
		    decide.variable_scopes.size(), var_idx);
	}
	decide.variable_scopes.push_back(scope);
	if (scope.IsKeyed()) {
		decide.keyed_scopes[scope.keyed_scope_idx].scoped_variable_indices.push_back(var_idx);
	}
	decide.aux_var_expressions.emplace_back(var_idx, label);
	return var_idx;
}

DecideVarScopeInfo decide_rewrite::JoinScopes(const DecideVarScopeInfo &a, const DecideVarScopeInfo &b) {
	// The key lattice (JoinDecideKeys) over the scopes themselves: a keyed scope stands for its key.
	auto key_of = [](const DecideVarScopeInfo &scope) {
		return scope.IsGlobal() ? DConstants::INVALID_INDEX : scope.IsKeyed() ? scope.keyed_scope_idx : DECIDE_ROW_KEY;
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
			// A decision brings its declared scope. A reducer value helper's is a scope of its own, one class per
			// group of its path key or per window of its frame (AddValueHelper), so what is computed from its value
			// has an instance per group, not per row: a row's instance would make each row a variant of a scope
			// reading it (`sum(per t.g: abs(avg(...)))`).
			return var_idx < decide.variable_scopes.size() ? decide.variable_scopes[var_idx]
			                                               : DecideVarScopeInfo::Row();
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		idx_t frame;
		if (TryGetDecideFrame(expr, frame)) {
			// Point frames are anchored at a row. Their value is one per anchor, even when the underlying order
			// or timeline key is not a declared DECIDE scope.
			return DecideVarScopeInfo::Row();
		}
		// An ungrouped reducer has one query-wide value. A reducer with a `by` or relation qualifier has multiple
		// values whose groups are not a declared DECIDE scope, so its ABS helper must retain row identity.
		idx_t qualifier;
		if (TryParseQualifiedReducerTag(expr.GetAlias(), qualifier) ||
		    DecideReducerGroupKey(expr.Cast<BoundAggregateExpression>()) != DConstants::INVALID_INDEX) {
			return DecideVarScopeInfo::Row();
		}
		return DecideVarScopeInfo::Global();
	}
	if (canonicalizer.IsQueryWideConstant(expr)) {
		return DecideVarScopeInfo::Global();
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return DecideVarScopeInfo::Row();
	}
	auto scope = DecideVarScopeInfo::Global();
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { scope = JoinScopes(scope, ScopeOf(decide, canonicalizer, child)); });
	return scope;
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
	return RenderDecideSource(expr, DecidePlanNames(decide.source_text, decide.keyed_scopes, decide.keys,
	                                                decide.decide_index, decide.aux_var_expressions, decide.frames));
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
	return TryParseSourceClauseTag(expr.GetAlias(), provenance_id) || HasDecideTag(expr.GetAlias(), OBJECTIVE_ROW_TAG)
	           ? expr.GetAlias()
	           : inherited;
}

//! Does `expr` hold a NORM marker anywhere beneath it?
//!
//! Only the L0 counts over decisions are still markers here (`0_auto`, `0_<double>`): canonicalization replaced every
//! other norm by its definition (DecideCanonicalizer::DesugarNorms).
static bool ContainsNormMarker(const Expression &expr) {
	string payload;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE && TryParseNormMarker(expr.GetAlias(), payload)) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found && ContainsNormMarker(child)) {
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
		if (ContainsNormMarker(**expr)) {
			throw InternalException("DECIDE NORM marker survived optimization in '%s'", (*expr)->ToString());
		}
	});
}

//! The MIN/MAX rewrites' exit contract: every member an extremum helper reads is linear.
//!
//! The value links read a member as a linear form; a square there is dropped without a word and the extremum is of
//! something else. Definition F-node preparation registers every such body before they run, so a non-linear member
//! here is a body the tree preparation missed. The cheap half of a MIN/MAX (MINMAX_EASY_REWRITE_TAG) bounds each
//! member in its own quadratic row and is exempt.
static void VerifyExtremumBodiesLinear(ClientContext &context, LogicalDecide &decide) {
	DecideTermSplitter splitter(context, decide);
	for (auto &link : decide.reducer_value_links) {
		if (IsExtremumName(link.agg_name) && !splitter.IsLinear(*link.inner_expr)) {
			throw InternalException("DECIDE MIN/MAX member '%s' is not linear", link.inner_expr->ToString());
		}
	}
}

void DecideOptimizer::TagAtomicRemovalGroups(LogicalDecide &decide) {
	auto is_removable = [](const Expression &node) {
		if (node.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			return node.type == ExpressionType::COMPARE_NOTEQUAL || ContainsNormMarker(node);
		}
		return true; // a COMPARE_IN
	};
	// The clause a leaf belongs to, named by its first source id: a BETWEEN is one clause of two leaves, which
	// diagnosis removes together or not at all.
	auto clause_of = [&](const Expression &node) {
		auto source_id = SourceClauseOf(node, decide);
		auto &source = decide.constraint_sources[source_id];
		return source.between_upper ? source.between_partner : source_id;
	};
	std::set<idx_t> removable_clauses;
	ForEachConstraintLeaf(*decide.decide_constraints, [&](Expression &node) {
		if (is_removable(node)) {
			removable_clauses.insert(clause_of(node));
		}
	});
	idx_t next_group = 0;
	std::map<idx_t, idx_t> group_of_clause;
	ForEachConstraintLeaf(*decide.decide_constraints, [&](Expression &node) {
		auto clause = clause_of(node);
		if (removable_clauses.count(clause) == 0) {
			return;
		}
		idx_t group = group_of_clause.emplace(clause, next_group).first->second;
		next_group = std::max(next_group, group + 1);
		auto alias = node.GetAlias();
		AddDecideTag(alias, MakeRemovalGroupTag(group));
		node.SetAlias(std::move(alias));
	});
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
	wrapper->alias = WHEN_FILTER_TAG;
	return std::move(wrapper);
}

unique_ptr<Expression> decide_rewrite::ScopeRowPerClause(ClientContext &context, LogicalDecide &decide,
                                                         unique_ptr<Expression> row, const DecideEnclosingScope &scope,
                                                         const string &source_alias) {
	if (!scope.per_wrapper) {
		return ScopeHelperRow(std::move(row), scope.Condition());
	}
	DecideCanonicalizer canonicalizer(context, decide.decide_index, decide.variable_scopes, decide.keyed_scopes,
	                                  decide.keys, decide.is_boolean_var);
	canonicalizer.frames = &decide.frames;
	vector<DecideSignatureField> fields;
	canonicalizer.CollectSignatureFields(*row, scope.PerKey(), fields);
	row = ScopeHelperRow(std::move(row), scope.Condition());
	// The wrapper holds its clause only, its key is in the tag (DecideConstraintsBinder).
	auto wrapper = make_uniq<BoundConjunctionExpression>(scope.per_wrapper->type);
	auto alias = scope.per_wrapper->GetAlias();
	idx_t written;
	string text = "per " + DecideKeyText(decide.keys, scope.PerKey());
	if (TryParseSignatureTag(alias, written)) {
		text = decide.signatures[written].text;
		RemoveDecideTag(alias, MakeSignatureTag(written));
	}
	if (!fields.empty()) {
		DecideSignature signature;
		signature.rows = scope.Condition() ? scope.Condition()->Copy() : nullptr;
		signature.key = scope.PerKey();
		signature.fields = std::move(fields);
		signature.text = std::move(text);
		TryParseSourceClauseTag(source_alias, signature.source_clause_id);
		decide.signatures.push_back(std::move(signature));
		AddDecideTag(alias, MakeSignatureTag(decide.signatures.size() - 1));
	}
	wrapper->alias = std::move(alias);
	wrapper->children.push_back(std::move(row));
	return std::move(wrapper);
}

unique_ptr<Expression> decide_rewrite::ScopeRowPerInstance(const LogicalDecide &decide, unique_ptr<Expression> row,
                                                           const DecideVarScopeInfo &scope) {
	if (!scope.IsGlobal() && !scope.IsKeyed()) {
		return row;
	}
	// A range frame's helper has one instance per window of its frame, which no key's classes name
	// (RewriteFrameRanges): a row of it stays one per row, each reading its window's instance, and the rows of one
	// window say the same.
	if (scope.IsKeyed()) {
		auto link = decide.keyed_scopes[scope.keyed_scope_idx].value_link;
		if (link != DConstants::INVALID_INDEX && decide.reducer_value_links[link].frame != DConstants::INVALID_INDEX) {
			return row;
		}
	}
	auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	wrapper->children.push_back(std::move(row));
	wrapper->alias = PER_CLASS_TAG;
	if (scope.IsKeyed()) {
		AddDecideTag(wrapper->alias, MakePerKeyTag(decide.keyed_scopes[scope.keyed_scope_idx].key));
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
		if (name == "-") {
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
				const bool is_divisor = name == "/";
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
