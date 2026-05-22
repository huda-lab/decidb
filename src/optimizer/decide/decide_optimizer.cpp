#include "duckdb/optimizer/decide_optimizer.hpp"

#include <cstdlib>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"
#include "duckdb/common/exception/binder_exception.hpp"

namespace duckdb {

static ObjectiveAggregateType StrToAggType(const string &name) {
	if (name == "sum") return ObjectiveAggregateType::SUM;
	if (name == "min") return ObjectiveAggregateType::MIN_AGG;
	if (name == "max") return ObjectiveAggregateType::MAX_AGG;
	return ObjectiveAggregateType::NONE;
}

// Forward declaration: defined further down with the ABS soundness handling.
static void TagAbsConstraintsForBigM(LogicalDecide &decide);

DecideOptimizer::DecideOptimizer(Optimizer &optimizer) : optimizer(optimizer) {
}

unique_ptr<LogicalOperator> DecideOptimizer::Optimize(unique_ptr<LogicalOperator> op) {
	// Recurse into children first (bottom-up)
	for (auto &child : op->children) {
		child = Optimize(std::move(child));
	}

	// If this is a LogicalDecide node, apply DECIDE-specific optimizations
	if (op->type == LogicalOperatorType::LOGICAL_DECIDE) {
		auto &decide = op->Cast<LogicalDecide>();
		OptimizeDecide(decide);
	}

	return op;
}

void DecideOptimizer::OptimizeDecide(LogicalDecide &decide) {
	bool bench = std::getenv("PACKDB_BENCH") != nullptr;
	Profiler timer;
	if (bench) {
		timer.Start();
	}

	TagAbsConstraintsForBigM(decide); // Must run before RewriteAbs: marks ABS nodes that need Big-M
	RewriteAbs(decide);          // Must run first: creates aux vars replacing ABS nodes
	RewriteBilinear(decide);     // McCormick linearization for Boolean × anything bilinear products
	RewriteComposedMinMax(decide); // Detect composed MIN/MAX before single-term MIN/MAX rewrite
	RewriteMinMax(decide);       // Classify + rewrite min/max (creates indicators and SUM nodes)
	RewriteNotEqual(decide);
	RewriteAvgToSum(decide);

	if (bench) {
		timer.End();
		fprintf(stderr, "PACKDB_BENCH: optimizer_ms=%.2f\n", timer.Elapsed() * 1000.0);
	}
}

void DecideOptimizer::RewriteNotEqual(LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	// Walk the bound constraint tree and find all COMPARE_NOTEQUAL expressions.
	// For each one, create an auxiliary BOOLEAN indicator variable.
	// The constraint expression itself is NOT modified — the physical operator
	// matches COMPARE_NOTEQUAL constraints with ne_indicator_indices at execution time.
	FindNotEqualConstraints(*decide.decide_constraints, decide);
}

void DecideOptimizer::FindNotEqualConstraints(Expression &expr, LogicalDecide &decide) {
	// Handle WHEN/PER wrappers: BoundConjunctionExpression with alias tag
	// Recurse into child[0] (the actual constraint)
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (conj.alias == WHEN_CONSTRAINT_TAG || IsPerConstraintTag(conj.alias)) {
			// child[0] is the wrapped constraint
			if (!conj.children.empty()) {
				FindNotEqualConstraints(*conj.children[0], decide);
			}
			return;
		}
		// Regular AND conjunction — recurse into all children
		for (auto &child : conj.children) {
			FindNotEqualConstraints(*child, decide);
		}
		return;
	}

	// Found a not-equal comparison — create an indicator variable
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		if (comp.type == ExpressionType::COMPARE_NOTEQUAL) {
			// Create auxiliary BOOLEAN indicator variable
			idx_t ind_idx = decide.decide_variables.size();
			string ind_name = "__ne_ind_" + to_string(decide.ne_indicator_indices.size()) + "__";
			auto ind_var = make_uniq<BoundColumnRefExpression>(
			    ind_name, LogicalType::BOOLEAN, ColumnBinding(decide.decide_index, ind_idx));
			decide.decide_variables.push_back(std::move(ind_var));
			decide.ne_indicator_indices.push_back(ind_idx);
			decide.num_auxiliary_vars++;
			decide.is_boolean_var.push_back(true);
			if (!decide.variable_entity_scope.empty()) {
				decide.variable_entity_scope.push_back(DConstants::INVALID_INDEX);
			}
			// Tag the comparison with the indicator index for direct matching
			comp.alias = string(NE_INDICATOR_TAG_PREFIX) + to_string(ind_idx) + "__";
		}
	}
}

// ---------------------------------------------------------------------------
// AVG → SUM rewrite
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteAvgToSum(LogicalDecide &decide) {
	if (decide.decide_constraints) {
		RewriteAvgInExpression(decide.decide_constraints);
	}
	if (decide.decide_objective) {
		RewriteAvgInExpression(decide.decide_objective);
	}
}

void DecideOptimizer::RewriteAvgInExpression(unique_ptr<Expression> &expr) {
	if (!expr) {
		return;
	}

	// Check if this node is an AVG aggregate — may be wrapped in a BOUND_CAST
	Expression *inner = expr.get();
	bool has_cast = false;
	if (inner->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		has_cast = true;
		inner = inner->Cast<BoundCastExpression>().child.get();
	}

	if (inner->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = inner->Cast<BoundAggregateExpression>();
		if (StringUtil::CIEquals(agg.function.name, "avg") && agg.children.size() == 1) {
			// Replace AVG(expr) with SUM(expr)
			vector<unique_ptr<Expression>> sum_children;
			sum_children.push_back(agg.children[0]->Copy());
			auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
			if (agg.filter) {
				new_sum->Cast<BoundAggregateExpression>().filter = agg.filter->Copy();
			}

			// Tag so execution layer knows to apply AVG's row-count denominator.
			// For a single objective AVG this is optimization-equivalent to SUM,
			// but additive objective expressions can mix AVG with SUM or filtered
			// aggregates, so preserving the scale is required for correct semantics.
			new_sum->alias = AVG_REWRITE_TAG;

			if (has_cast) {
				// Preserve the cast wrapper — update its child
				auto &cast_expr = expr->Cast<BoundCastExpression>();
				cast_expr.child = std::move(new_sum);
			} else {
				expr = std::move(new_sum);
			}
			return;
		}
	}

	// Recurse into children
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		RewriteAvgInExpression(child);
	});
}

// ---------------------------------------------------------------------------
// Shared helper
// ---------------------------------------------------------------------------

//! Check if a bound expression references any decide variable (by table_index match)
static bool BoundExprReferencesDecideVar(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		return colref.binding.table_index == decide_index;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = BoundExprReferencesDecideVar(child, decide_index);
		}
	});
	return found;
}

// ---------------------------------------------------------------------------
// ABS-in-constraint soundness handling (must run BEFORE RewriteAbs)
// ---------------------------------------------------------------------------
//
// PackDB's ABS linearization replaces `ABS(e)` with an auxiliary `aux` and
// adds the lower-envelope constraints `aux >= e` and `aux >= -e`. That alone
// only forces `aux >= |e|`. Soundness then requires that aux be pinned to
// exactly |e|, not free above it. There are three pinning mechanisms:
//
//   1. Solver pressure under MINIMIZE objective: aux contributes positively
//      to the objective, the solver pushes aux down to |e|. Sound — no
//      Big-M needed.
//   2. Constraint context that upper-bounds aux: ABS on the LHS of `<=`/`<`,
//      or on the RHS of `>=`/`>`. The constraint itself caps aux from
//      above, the lower envelope caps it from below at |e|. Sound — no
//      Big-M needed.
//   3. Big-M upper envelope with a sign-indicator binary y: emit
//      `aux <= e + 2M(1-y)` and `aux <= -e + 2M*y`. Combined with the
//      lower envelope this forces aux = |e| exactly, regardless of solver
//      pressure or constraint shape. Used for MAXIMIZE objective ABS, and
//      now for any constraint shape outside category 2.
//
// This pass walks the constraint tree, classifies each ABS-bearing
// comparison, and tags ABS function expressions in non-category-2 positions
// with ABS_NEEDS_BIGM_TAG. RewriteAbs reads the tag and propagates to
// AbsPairInfo::needs_bigm; Phase 2 then allocates the y indicator and emits
// the upper-envelope at execution time (the existing AbsMaximizeLink path).
//
// Aggregates over ABS (`SUM`, `AVG`, `MIN`, `MAX`) compose under the same
// rule. When the aggregate constraint upper-bounds the aggregate value
// (e.g. `SUM(ABS) <= K`, `MAX(ABS) <= K`), individual auxes are pinned
// transitively and Big-M is not needed. Otherwise we Big-M each aux and
// the aggregate then operates on pinned auxes.
// ---------------------------------------------------------------------------

static bool ContainsAbsOverDecideVar(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (StringUtil::CIEquals(func.function.name, "abs") && func.children.size() == 1) {
			if (BoundExprReferencesDecideVar(*func.children[0], decide_index)) {
				return true;
			}
		}
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = ContainsAbsOverDecideVar(child, decide_index);
		}
	});
	return found;
}

// Walk an expression tree and tag every BoundFunctionExpression for ABS over
// a decide var with ABS_NEEDS_BIGM_TAG. Used on the side of a comparison that
// does not upper-bound aux (or on entire BETWEEN/IN/equality/<> subtrees).
static void TagAbsForBigM(Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (StringUtil::CIEquals(func.function.name, "abs") && func.children.size() == 1) {
			if (BoundExprReferencesDecideVar(*func.children[0], decide_index)) {
				func.alias = ABS_NEEDS_BIGM_TAG;
				return;
			}
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		TagAbsForBigM(child, decide_index);
	});
}

static void ClassifyAbsConstraints(Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		// WHEN/PER wrappers: child[0] is the inner constraint; recurse into it.
		// The WHEN/PER filter only affects which rows participate in the
		// aggregate; aux pinning is unconditional per row, so the wrapper
		// doesn't change classification.
		if (conj.alias == WHEN_CONSTRAINT_TAG || IsPerConstraintTag(conj.alias)) {
			if (!conj.children.empty()) {
				ClassifyAbsConstraints(*conj.children[0], decide_index);
			}
			return;
		}
		for (auto &child : conj.children) {
			ClassifyAbsConstraints(*child, decide_index);
		}
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		bool lhs_has_abs = ContainsAbsOverDecideVar(*comp.left, decide_index);
		bool rhs_has_abs = ContainsAbsOverDecideVar(*comp.right, decide_index);
		if (!lhs_has_abs && !rhs_has_abs) {
			return;
		}
		auto t = comp.type;
		bool lhs_upper_bounded = (t == ExpressionType::COMPARE_LESSTHAN ||
		                          t == ExpressionType::COMPARE_LESSTHANOREQUALTO);
		bool rhs_upper_bounded = (t == ExpressionType::COMPARE_GREATERTHAN ||
		                          t == ExpressionType::COMPARE_GREATERTHANOREQUALTO);
		// Sound (no Big-M needed): exactly one side has ABS and the comparison
		// upper-bounds that side. ABS on both sides falls through to Big-M
		// because we'd have to upper-bound both auxes anyway.
		bool sound = (lhs_has_abs && lhs_upper_bounded && !rhs_has_abs) ||
		             (rhs_has_abs && rhs_upper_bounded && !lhs_has_abs);
		if (sound) {
			return;
		}
		// Hard-direction or symmetric: tag every ABS in both sides for Big-M.
		// Tagging both sides covers the "ABS on both sides" case correctly.
		if (lhs_has_abs) {
			TagAbsForBigM(*comp.left, decide_index);
		}
		if (rhs_has_abs) {
			TagAbsForBigM(*comp.right, decide_index);
		}
		return;
	}
	// Other top-level shapes (BETWEEN, IN, equality, <>, conjunctions handled
	// above): no per-side direction analysis available — tag every ABS in the
	// subtree for Big-M to be safe.
	if (ContainsAbsOverDecideVar(expr, decide_index)) {
		TagAbsForBigM(expr, decide_index);
	}
}

static void TagAbsConstraintsForBigM(LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	ClassifyAbsConstraints(*decide.decide_constraints, decide.decide_index);
}

// ---------------------------------------------------------------------------
// Composed MIN/MAX constraints (additive LHS mixing SUM/AVG/MIN/MAX terms)
// ---------------------------------------------------------------------------
//
// Single-term `MIN/MAX(expr) CMP K` is handled by RewriteMinMax below. When a
// MIN/MAX appears *inside* an additive LHS (e.g. `SUM(a*x) + MAX(b*x) <= K`),
// we extract the full constraint shape into decide.composed_minmax_constraints
// and replace the comparison with a TRUE placeholder. The physical layer
// allocates global auxiliaries (z_k per MIN/MAX term) and emits the pinning
// constraints at sink-finalize time.

void DecideOptimizer::RewriteComposedMinMax(LogicalDecide &decide) {
	if (decide.decide_constraints) {
		RewriteComposedMinMaxInConstraint(decide.decide_constraints, decide);
	}
	RewriteComposedMinMaxObjectiveTop(decide);
}

// Unwrap CAST wrappers to get at the payload expression.
static const Expression &UnwrapCast(const Expression &e) {
	const Expression *cur = &e;
	while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		cur = cur->Cast<BoundCastExpression>().child.get();
	}
	return *cur;
}

// True if the expression is a BOUND_FUNCTION for `+` (after unwrapping cast).
static bool IsAddNode(const Expression &e) {
	auto &u = UnwrapCast(e);
	if (u.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) return false;
	return StringUtil::Lower(u.Cast<BoundFunctionExpression>().function.name) == "+";
}

// True if the expression is a `-` function (both binary and unary).
static bool IsSubNode(const Expression &e) {
	auto &u = UnwrapCast(e);
	if (u.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) return false;
	return StringUtil::Lower(u.Cast<BoundFunctionExpression>().function.name) == "-";
}

// True if any MIN/MAX aggregate over a decide var appears at or below the node.
// Recurses through any function node's children (not just +/-), so shapes like
// `2 * MIN(...)` are detected and can be rejected with a clean binder error.
static bool AdditiveContainsMinMax(const Expression &e, idx_t decide_index) {
	auto &u = UnwrapCast(e);
	if (u.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = u.Cast<BoundAggregateExpression>();
		auto name = StringUtil::Lower(agg.function.name);
		if ((name == "min" || name == "max") && agg.children.size() == 1 &&
		    BoundExprReferencesDecideVar(*agg.children[0], decide_index)) {
			return true;
		}
	}
	if (u.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &fn = u.Cast<BoundFunctionExpression>();
		for (auto &child : fn.children) {
			if (AdditiveContainsMinMax(*child, decide_index)) {
				return true;
			}
		}
	}
	return false;
}

// Walk the additive LHS, emitting a ComposedMinMaxTerm for each leaf aggregate.
// Throws BinderException on v1-unsupported shapes (non-aggregate leaves,
// nested subtraction/scaling, non-SUM/AVG/MIN/MAX aggregates).
static void WalkComposedLhs(const Expression &e, int sign, idx_t decide_index, bool outer_push_down,
                             vector<LogicalDecide::ComposedMinMaxTerm> &out_terms) {
	auto &u = UnwrapCast(e);
	if (IsAddNode(u)) {
		auto &fn = u.Cast<BoundFunctionExpression>();
		for (auto &child : fn.children) {
			WalkComposedLhs(*child, sign, decide_index, outer_push_down, out_terms);
		}
		return;
	}
	if (IsSubNode(u)) {
		// v1 rejects subtraction in composed MIN/MAX LHS — it flips the direction
		// of each term, doubling the easy/hard classification surface.
		throw BinderException(
		    "Composed MIN/MAX in DECIDE v1 does not support subtraction in the LHS. "
		    "Rewrite the constraint as an additive sum (e.g., move terms to the RHS).");
	}
	// Scalar * aggregate (e.g. `2 * MIN(...)`) is not supported in v1.
	if (u.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &fn = u.Cast<BoundFunctionExpression>();
		auto fname = StringUtil::Lower(fn.function.name);
		if (fname == "*" || fname == "/") {
			throw BinderException(
			    "Composed MIN/MAX in DECIDE v1 does not support scalar multiplication "
			    "or division of aggregate terms (e.g. `2 * MIN(...)`). Each term must "
			    "be a bare SUM/AVG/MIN/MAX aggregate.");
		}
	}
	if (u.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		throw BinderException(
		    "Composed MIN/MAX in DECIDE v1 supports only additive sums of SUM/AVG/MIN/MAX aggregates. "
		    "Got non-aggregate term: %s",
		    e.ToString());
	}
	auto &agg = u.Cast<BoundAggregateExpression>();
	auto name = StringUtil::Lower(agg.function.name);
	if (name != "sum" && name != "avg" && name != "min" && name != "max") {
		throw BinderException("Composed MIN/MAX in DECIDE v1 does not support aggregate '%s'; "
		                      "only SUM/AVG/MIN/MAX are supported.", name);
	}
	if (agg.children.size() != 1) {
		throw BinderException("Composed MIN/MAX: aggregate '%s' must have a single inner expression.", name);
	}

	LogicalDecide::ComposedMinMaxTerm term;
	term.kind = (name == "min" || name == "max")
	                ? LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND
	                : LogicalDecide::ComposedMinMaxTerm::SUM_KIND;
	term.agg_name = name;
	term.sign = sign;
	term.inner_expr = agg.children[0]->Copy();
	if (agg.filter) {
		term.filter = agg.filter->Copy();
	}
	if (term.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) {
		bool is_max = (name == "max");
		// z_k pushed down if the outer wants LHS small and this term's sign is +,
		// or outer wants LHS large and this term's sign is -.
		bool z_pushed_down = (sign > 0) ? outer_push_down : !outer_push_down;
		// Easy: MAX pushed down, or MIN pushed up.
		term.is_easy = (is_max && z_pushed_down) || (!is_max && !z_pushed_down);
	}
	out_terms.push_back(std::move(term));
}

void DecideOptimizer::RewriteComposedMinMaxInConstraint(unique_ptr<Expression> &expr, LogicalDecide &decide) {
	if (!expr) {
		return;
	}

	// Walk through AND conjunctions and WHEN/PER wrappers (no composed MIN/MAX inside WHEN/PER in v1).
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr->Cast<BoundConjunctionExpression>();
		if (conj.alias == WHEN_CONSTRAINT_TAG || IsPerConstraintTag(conj.alias)) {
			// If the wrapped constraint is composed MIN/MAX, reject in v1.
			if (!conj.children.empty()) {
				auto &inner = *conj.children[0];
				if (inner.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
					auto &cmp = inner.Cast<BoundComparisonExpression>();
					if (cmp.left && AdditiveContainsMinMax(*cmp.left, decide.decide_index) &&
					    IsAddNode(*cmp.left)) {
						throw BinderException(
						    "Composed MIN/MAX in DECIDE v1 does not support outer WHEN/PER wrappers. "
						    "Remove the WHEN/PER or restructure the constraint.");
					}
				}
				RewriteComposedMinMaxInConstraint(conj.children[0], decide);
			}
			return;
		}
		// Regular AND conjunction — recurse into all children
		for (auto &child : conj.children) {
			RewriteComposedMinMaxInConstraint(child, decide);
		}
		return;
	}

	if (expr->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
		return;
	}
	auto &comp = expr->Cast<BoundComparisonExpression>();
	if (!comp.left) {
		return;
	}

	// Must be additive (+/-) AND contain a MIN/MAX leaf; otherwise leave to the
	// single-term rewrite. The walker rejects subtraction with a clear binder error.
	if (!IsAddNode(*comp.left) && !IsSubNode(*comp.left)) {
		return;
	}
	if (!AdditiveContainsMinMax(*comp.left, decide.decide_index)) {
		return;
	}

	auto cmp_type = comp.type;
	if (cmp_type != ExpressionType::COMPARE_LESSTHAN &&
	    cmp_type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
	    cmp_type != ExpressionType::COMPARE_GREATERTHAN &&
	    cmp_type != ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
		throw BinderException(
		    "Composed MIN/MAX in DECIDE v1 supports only <, <=, >, >= comparisons. "
		    "Equality, IN, and BETWEEN are not supported.");
	}

	bool outer_push_down = (cmp_type == ExpressionType::COMPARE_LESSTHAN ||
	                         cmp_type == ExpressionType::COMPARE_LESSTHANOREQUALTO);

	LogicalDecide::ComposedMinMaxConstraint spec;
	spec.outer_cmp = cmp_type;
	spec.rhs_expr = comp.right->Copy();

	WalkComposedLhs(*comp.left, /*sign=*/1, decide.decide_index, outer_push_down, spec.terms);

	decide.composed_minmax_constraints.push_back(std::move(spec));

	// Replace the comparison with a TRUE placeholder so the normal constraint path is a no-op.
	expr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
}

void DecideOptimizer::RewriteComposedMinMaxObjectiveTop(LogicalDecide &decide) {
	if (!decide.decide_objective) {
		return;
	}
	auto &obj = *decide.decide_objective;

	// Reject composed MIN/MAX in objectives with outer PER or WHEN (v1 scope).
	if (obj.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = obj.Cast<BoundConjunctionExpression>();
		if (IsPerConstraintTag(conj.alias) || conj.alias == WHEN_CONSTRAINT_TAG) {
			// If the wrapped objective is composed, reject.
			if (!conj.children.empty()) {
				auto &inner = *conj.children[0];
				if (AdditiveContainsMinMax(inner, decide.decide_index) &&
				    (IsAddNode(inner) || IsSubNode(inner))) {
					throw BinderException(
					    "Composed MIN/MAX in DECIDE v1 does not support outer WHEN/PER "
					    "wrappers on the objective. Restructure the objective.");
				}
			}
			return;
		}
	}

	// LHS must be additive (+/-) AND contain a MIN/MAX leaf; otherwise leave to
	// the flat-single-aggregate rewrite.
	if (!IsAddNode(obj) && !IsSubNode(obj)) {
		return;
	}
	if (!AdditiveContainsMinMax(obj, decide.decide_index)) {
		return;
	}

	// Direction: MAXIMIZE pushes each term UP; MINIMIZE pushes each term DOWN.
	bool outer_push_down = (decide.decide_sense == DecideSense::MINIMIZE);

	vector<LogicalDecide::ComposedMinMaxTerm> terms;
	WalkComposedLhs(obj, /*sign=*/1, decide.decide_index, outer_push_down, terms);

	decide.composed_minmax_objective_terms = std::move(terms);

	// Replace the objective with a zero placeholder. The physical layer fills in
	// objective coefficients from the spec.
	decide.decide_objective = make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0));
}

// ---------------------------------------------------------------------------
// MIN/MAX linearization
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteMinMax(LogicalDecide &decide) {
	RewriteMinMaxConstraints(decide);
	RewriteMinMaxObjective(decide);
}

void DecideOptimizer::RewriteMinMaxConstraints(LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	vector<unique_ptr<Expression>> new_constraints;
	bool was_easy = false;
	RewriteMinMaxInConstraint(decide.decide_constraints, decide, new_constraints, was_easy);

	// Append generated constraints (from equality splitting) to the constraint tree
	for (auto &nc : new_constraints) {
		AppendConstraint(decide, std::move(nc));
	}
}

void DecideOptimizer::RewriteMinMaxInConstraint(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                                vector<unique_ptr<Expression>> &new_constraints,
                                                bool &out_was_easy) {
	if (!expr) {
		return;
	}

	// Handle WHEN/PER wrappers
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr->Cast<BoundConjunctionExpression>();
		if (conj.alias == WHEN_CONSTRAINT_TAG) {
			// Recurse into the wrapped constraint (child[0])
			if (!conj.children.empty()) {
				RewriteMinMaxInConstraint(conj.children[0], decide, new_constraints, out_was_easy);
			}
			return;
		}
		if (IsPerConstraintTag(conj.alias)) {
			// Recurse into the wrapped constraint (child[0])
			if (!conj.children.empty()) {
				RewriteMinMaxInConstraint(conj.children[0], decide, new_constraints, out_was_easy);
				// Easy MIN/MAX (e.g., MAX(e) <= C, MIN(e) >= C) are vacuously true over
				// empty sets. Strip PER — the per-row form skips WHEN-excluded rows.
				if (out_was_easy) {
					expr = std::move(conj.children[0]);
				}
			}
			return;
		}
		// Regular AND conjunction — recurse into all children
		for (auto &child : conj.children) {
			bool child_easy = false;
			RewriteMinMaxInConstraint(child, decide, new_constraints, child_easy);
		}
		return;
	}

	// Check for comparison with MIN/MAX on LHS
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
		return;
	}
	auto &comp = expr->Cast<BoundComparisonExpression>();

	// Unwrap any BoundCastExpression on the LHS
	Expression *lhs = comp.left.get();
	while (lhs->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		lhs = lhs->Cast<BoundCastExpression>().child.get();
	}

	if (lhs->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	auto &agg = lhs->Cast<BoundAggregateExpression>();
	auto fname = StringUtil::Lower(agg.function.name);
	if (fname != "min" && fname != "max") {
		return;
	}
	if (agg.children.size() != 1) {
		return;
	}
	// Guard: only rewrite MIN/MAX over decide variables
	if (!BoundExprReferencesDecideVar(*agg.children[0], decide.decide_index)) {
		return;
	}

	bool is_max = (fname == "max");
	auto cmp_type = comp.type;

	// Classify: easy vs hard
	bool is_easy = false;
	if (is_max && (cmp_type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	               cmp_type == ExpressionType::COMPARE_LESSTHAN)) {
		is_easy = true; // MAX(expr) <= K → every row: expr <= K
	}
	if (!is_max && (cmp_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	                cmp_type == ExpressionType::COMPARE_GREATERTHAN)) {
		is_easy = true; // MIN(expr) >= K → every row: expr >= K
	}

	bool is_hard = false;
	if (is_max && (cmp_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	               cmp_type == ExpressionType::COMPARE_GREATERTHAN)) {
		is_hard = true; // MAX(expr) >= K → need indicator
	}
	if (!is_max && (cmp_type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	                cmp_type == ExpressionType::COMPARE_LESSTHAN)) {
		is_hard = true; // MIN(expr) <= K → need indicator
	}

	if (cmp_type == ExpressionType::COMPARE_NOTEQUAL) {
		throw BinderException("DECIDE does not support <> comparison with MIN/MAX aggregates.");
	}

	if (cmp_type == ExpressionType::COMPARE_EQUAL) {
		// Equality: split into easy + hard parts
		// MAX(expr) = K → (expr <= K) AND (MAX(expr) >= K)
		// MIN(expr) = K → (expr >= K) AND (MIN(expr) <= K)

		// Easy part: per-row bound
		auto easy_cmp_type = is_max ? ExpressionType::COMPARE_LESSTHANOREQUALTO
		                            : ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		unique_ptr<Expression> easy = make_uniq<BoundComparisonExpression>(
		    easy_cmp_type,
		    agg.children[0]->Copy(), comp.right->Copy());
		easy->alias = MINMAX_EASY_REWRITE_TAG;
		// Preserve aggregate-local WHEN filter as a per-row WHEN wrapper
		if (agg.filter) {
			auto when_wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			when_wrapper->children.push_back(std::move(easy));
			when_wrapper->children.push_back(agg.filter->Copy());
			when_wrapper->alias = WHEN_CONSTRAINT_TAG;
			easy = std::move(when_wrapper);
		}
		new_constraints.push_back(std::move(easy));

		// Hard part: allocate indicator + tagged SUM
		auto hard_cmp_type = is_max ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
		                            : ExpressionType::COMPARE_LESSTHANOREQUALTO;
		idx_t ind_idx;
		comp.left = EmitHardMinMaxIndicator(decide, fname, *agg.children[0], agg.filter.get(), ind_idx);
		comp.type = hard_cmp_type;
		return;
	}

	if (is_easy) {
		// Easy case: strip the aggregate, make it per-row
		// MAX(expr) <= K → expr <= K
		// MIN(expr) >= K → expr >= K
		// Save filter before destroying the aggregate (comp.left assignment invalidates agg reference)
		unique_ptr<Expression> saved_filter;
		if (agg.filter) {
			saved_filter = agg.filter->Copy();
		}
		comp.left = agg.children[0]->Copy();
		// Tag the comparison so physical_decide.cpp can enforce empty-WHEN
		// rejection on constraints the user wrote as MIN/MAX, even after the
		// optimizer strips the aggregate.
		comp.alias = MINMAX_EASY_REWRITE_TAG;
		// Preserve aggregate-local WHEN filter as a per-row WHEN wrapper
		if (saved_filter) {
			auto when_wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			when_wrapper->children.push_back(std::move(expr));
			when_wrapper->children.push_back(std::move(saved_filter));
			when_wrapper->alias = WHEN_CONSTRAINT_TAG;
			expr = std::move(when_wrapper);
		}
		out_was_easy = true;
		return;
	}

	if (is_hard) {
		// Hard case: allocate indicator + tagged SUM via shared helper
		idx_t ind_idx;
		comp.left = EmitHardMinMaxIndicator(decide, fname, *agg.children[0], agg.filter.get(), ind_idx);
		return;
	}
}

unique_ptr<Expression> DecideOptimizer::EmitHardMinMaxIndicator(LogicalDecide &decide,
                                                                 const string &agg_name,
                                                                 const Expression &inner,
                                                                 const Expression *filter,
                                                                 idx_t &out_ind_idx) {
	// Allocate Boolean indicator decide variable
	idx_t ind_idx = decide.decide_variables.size();
	string ind_name = "__minmax_ind_" + to_string(decide.minmax_indicator_links.size()) + "__";
	auto ind_var = make_uniq<BoundColumnRefExpression>(
	    ind_name, LogicalType::BOOLEAN, ColumnBinding(decide.decide_index, ind_idx));
	decide.decide_variables.push_back(std::move(ind_var));
	decide.num_auxiliary_vars++;
	decide.is_boolean_var.push_back(true);
	if (!decide.variable_entity_scope.empty()) {
		decide.variable_entity_scope.push_back(DConstants::INVALID_INDEX);
	}
	decide.minmax_indicator_links.emplace_back(agg_name, ind_idx);

	// Build a SUM(inner) aggregate tagged with the indicator index
	vector<unique_ptr<Expression>> sum_children;
	sum_children.push_back(inner.Copy());
	auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
	if (filter) {
		new_sum->Cast<BoundAggregateExpression>().filter = filter->Copy();
	}
	new_sum->alias = string(MINMAX_INDICATOR_TAG_PREFIX) + to_string(ind_idx) + "_" + agg_name + "__";
	out_ind_idx = ind_idx;
	return new_sum;
}

void DecideOptimizer::RewriteMinMaxObjective(LogicalDecide &decide) {
	if (!decide.decide_objective) {
		return;
	}

	// Navigate through PER and WHEN wrappers to find the actual aggregate
	unique_ptr<Expression> *obj_owner = &decide.decide_objective;
	Expression *obj_expr = decide.decide_objective.get();
	bool has_per = false;

	// Unwrap PER wrapper (outermost layer)
	if (obj_expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = obj_expr->Cast<BoundConjunctionExpression>();
		if (IsPerConstraintTag(conj.alias) && !conj.children.empty()) {
			has_per = true;
			obj_owner = &conj.children[0];
			obj_expr = conj.children[0].get();
		}
	}

	// Unwrap WHEN wrapper (inside PER, if present)
	if (obj_expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = obj_expr->Cast<BoundConjunctionExpression>();
		if (conj.alias == WHEN_CONSTRAINT_TAG && !conj.children.empty()) {
			obj_owner = &conj.children[0];
			obj_expr = conj.children[0].get();
		}
	}

	// Unwrap any BoundCastExpression
	if (obj_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = obj_expr->Cast<BoundCastExpression>();
		obj_owner = &cast.child;
		obj_expr = cast.child.get();
	}

	// Now inspect the actual aggregate
	if (obj_expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	auto &outer_agg = obj_expr->Cast<BoundAggregateExpression>();
	auto outer_name = StringUtil::Lower(outer_agg.function.name);

	// Check for nested aggregate: OUTER(INNER(expr)) where INNER is also SUM/MIN/MAX/AVG
	if (has_per && (outer_name == "sum" || outer_name == "min" || outer_name == "max" || outer_name == "avg") &&
	    outer_agg.children.size() == 1) {
		// Unwrap cast on inner child if present
		Expression *inner_expr = outer_agg.children[0].get();
		if (inner_expr->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			inner_expr = inner_expr->Cast<BoundCastExpression>().child.get();
		}
		if (inner_expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			auto &inner_agg = inner_expr->Cast<BoundAggregateExpression>();
			auto inner_name = StringUtil::Lower(inner_agg.function.name);

			if ((inner_name == "sum" || inner_name == "min" || inner_name == "max" || inner_name == "avg") &&
			    inner_agg.children.size() == 1 &&
			    BoundExprReferencesDecideVar(*inner_agg.children[0], decide.decide_index)) {
				// Found nested pattern: set metadata
				// Map outer AVG → SUM (dividing by constant G doesn't change optimal)
				decide.per_outer_agg = (outer_name == "avg") ? ObjectiveAggregateType::SUM
				                                             : StrToAggType(outer_name);
				// Map inner AVG → SUM with flag for coefficient scaling
				if (inner_name == "avg") {
					decide.per_inner_agg = ObjectiveAggregateType::SUM;
					decide.per_inner_was_avg = true;
				} else {
					decide.per_inner_agg = StrToAggType(inner_name);
				}

				// Pre-compute easy/hard classification for inner and outer levels
				if (inner_name == "min" || inner_name == "max") {
					bool inner_is_min = (inner_name == "min");
					decide.per_inner_is_easy = (inner_is_min && decide.decide_sense == DecideSense::MAXIMIZE) ||
					                           (!inner_is_min && decide.decide_sense == DecideSense::MINIMIZE);
				}
				if (outer_name == "min" || outer_name == "max") {
					bool outer_is_min = (outer_name == "min");
					decide.per_outer_is_easy = (outer_is_min && decide.decide_sense == DecideSense::MAXIMIZE) ||
					                           (!outer_is_min && decide.decide_sense == DecideSense::MINIMIZE);
				}

				// Rewrite inner MIN/MAX/AVG → SUM for normalization
				if (inner_name == "min" || inner_name == "max" || inner_name == "avg") {
					vector<unique_ptr<Expression>> sum_children;
					sum_children.push_back(inner_agg.children[0]->Copy());
					auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
					if (inner_agg.filter) {
						new_sum->Cast<BoundAggregateExpression>().filter = inner_agg.filter->Copy();
					}
					// Replace inner aggregate within the outer
					outer_agg.children[0] = std::move(new_sum);
				}
				// Strip outer wrapper: replace OUTER(INNER(expr)) with INNER(expr)
				*obj_owner = std::move(outer_agg.children[0]);
				return;
			}
		}
	}

	// Flat MIN/MAX + PER → error (ambiguous without outer aggregate)
	if (has_per && (outer_name == "min" || outer_name == "max") &&
	    outer_agg.children.size() == 1 &&
	    BoundExprReferencesDecideVar(*outer_agg.children[0], decide.decide_index)) {
		throw BinderException(
		    "MINIMIZE/MAXIMIZE %s(...) PER is ambiguous. "
		    "With PER, use a nested aggregate to specify how per-group values are combined: "
		    "e.g., SUM(%s(...)) PER col or MAX(%s(...)) PER col.",
		    StringUtil::Upper(outer_name), StringUtil::Upper(outer_name),
		    StringUtil::Upper(outer_name));
	}

	// Flat non-PER MIN/MAX objective
	if (!has_per && (outer_name == "min" || outer_name == "max") &&
	    outer_agg.children.size() == 1 &&
	    BoundExprReferencesDecideVar(*outer_agg.children[0], decide.decide_index)) {
		decide.flat_objective_agg = StrToAggType(outer_name);
		bool is_min = (outer_name == "min");
		decide.flat_objective_is_easy = (is_min && decide.decide_sense == DecideSense::MAXIMIZE) ||
		                                (!is_min && decide.decide_sense == DecideSense::MINIMIZE);
		// Replace MIN/MAX with SUM
		vector<unique_ptr<Expression>> sum_children;
		sum_children.push_back(outer_agg.children[0]->Copy());
		auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
		if (outer_agg.filter) {
			new_sum->Cast<BoundAggregateExpression>().filter = outer_agg.filter->Copy();
		}
		*obj_owner = std::move(new_sum);
	}
}

// ---------------------------------------------------------------------------
// ABS linearization (self-contained: detect, replace, and generate constraints)
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteAbs(LogicalDecide &decide) {
	// Phase 1: Find ABS(expr) nodes over decide vars, replace with auxiliary variables.
	// Each entry records the aux index, a copy of the inner expression, and whether
	// the ABS node originated in the objective (vs. a constraint).
	vector<AbsPairInfo> abs_pairs;
	if (decide.decide_constraints) {
		FindAndReplaceAbs(decide.decide_constraints, decide, abs_pairs, /*in_objective=*/false);
	}
	if (decide.decide_objective) {
		FindAndReplaceAbs(decide.decide_objective, decide, abs_pairs, /*in_objective=*/true);
	}

	// Phase 2: Generate linearization constraints for each auxiliary variable.
	// Always emit the lower-bound envelope:
	//   aux >= inner  and  aux >= -inner
	// (forces aux >= |inner|). Then, for auxes that are NOT pinned to |inner|
	// by natural solver pressure or constraint shape, allocate a binary sign
	// indicator y and tag the lower-envelope constraints so physical_decide.cpp
	// can emit the upper envelope at execution time:
	//   aux <= inner  + 2M*(1-y)   (pins aux to inner  when y=1)
	//   aux <= -inner + 2M*y       (pins aux to -inner when y=0)
	//
	// Big-M is required when:
	//   - ABS appears in the objective with sense==MAXIMIZE (solver pushes aux up).
	//   - ABS appears in a constraint shape that does not upper-bound aux (the
	//     hard direction: ABS(...) >= K, ABS = K, ABS <> K, BETWEEN, etc.).
	//     These auxes are flagged via pair.needs_bigm by TagAbsConstraintsForBigM.
	//
	// MINIMIZE objective and sound constraint shapes (e.g. ABS <= K) skip the
	// upper envelope — solver pressure / direct upper-bounding handles pinning.
	for (idx_t pi = 0; pi < abs_pairs.size(); pi++) {
		auto &pair = abs_pairs[pi];
		auto &aux_var = decide.decide_variables[pair.aux_idx];
		auto &aux_ref = aux_var->Cast<BoundColumnRefExpression>();

		// Constraint 1 (C1): aux >= inner_expr
		auto aux_ref1 = make_uniq<BoundColumnRefExpression>(
		    aux_ref.alias, aux_ref.return_type, aux_ref.binding);
		auto c1 = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		    std::move(aux_ref1), pair.inner_expr->Copy());

		// Constraint 2 (C2): aux >= -inner_expr  (computed as 0 - inner_expr)
		auto aux_ref2 = make_uniq<BoundColumnRefExpression>(
		    aux_ref.alias, aux_ref.return_type, aux_ref.binding);
		auto neg_expr = optimizer.BindScalarFunction(
		    "-",
		    make_uniq<BoundConstantExpression>(Value::INTEGER(0)),
		    pair.inner_expr->Copy());
		auto c2 = make_uniq<BoundComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		    std::move(aux_ref2), std::move(neg_expr));

		bool needs_bigm = pair.needs_bigm ||
		                  (pair.in_objective && decide.decide_sense == DecideSense::MAXIMIZE);
		if (needs_bigm) {
			// Allocate a boolean sign indicator variable y.
			idx_t y_idx = decide.decide_variables.size();
			string y_name = "__abs_y_" + to_string(pi) + "__";
			auto y_var = make_uniq<BoundColumnRefExpression>(
			    y_name, LogicalType::BOOLEAN,
			    ColumnBinding(decide.decide_index, y_idx));
			decide.decide_variables.push_back(std::move(y_var));
			decide.num_auxiliary_vars++;
			decide.is_boolean_var.push_back(true);
			if (!decide.variable_entity_scope.empty()) {
				decide.variable_entity_scope.push_back(DConstants::INVALID_INDEX);
			}

			// Tag C1 and C2 so physical_decide.cpp can find them by y_idx at finalization.
			c1->alias = string(ABS_UB_POS_TAG_PREFIX) + to_string(y_idx) + "__";
			c2->alias = string(ABS_UB_NEG_TAG_PREFIX) + to_string(y_idx) + "__";

			decide.abs_maximize_links.push_back({pair.aux_idx, y_idx});
		}

		AppendConstraint(decide, std::move(c1));
		AppendConstraint(decide, std::move(c2));
	}
}

void DecideOptimizer::FindAndReplaceAbs(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                        vector<AbsPairInfo> &abs_pairs, bool in_objective) {
	if (!expr) {
		return;
	}

	if (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr->Cast<BoundFunctionExpression>();
		if (StringUtil::CIEquals(func.function.name, "abs") && func.children.size() == 1) {
			if (BoundExprReferencesDecideVar(*func.children[0], decide.decide_index)) {
				// Declare the auxiliary as INTEGER when the inner expression is
				// integer-typed — |k| preserves integer-valuedness, so downstream
				// strict-inequality rewrites (`SUM(|...|) < K → <= K-1`) stay sound.
				// Without this, all ABS auxes are DOUBLE and the LHS-integer check
				// in ilp_model_builder would reject valid integer-valued strict cases.
				auto &inner_type = func.children[0]->return_type;
				bool inner_is_integer = inner_type.IsIntegral() ||
				                        inner_type.id() == LogicalTypeId::BOOLEAN;
				LogicalType aux_type =
				    inner_is_integer ? LogicalType::INTEGER : LogicalType::DOUBLE;

				// Create auxiliary variable
				idx_t aux_idx = decide.decide_variables.size();
				string aux_name = "__abs_aux_" + to_string(abs_pairs.size()) + "__";
				auto aux_var = make_uniq<BoundColumnRefExpression>(
				    aux_name, aux_type,
				    ColumnBinding(decide.decide_index, aux_idx));
				decide.decide_variables.push_back(std::move(aux_var));
				decide.num_auxiliary_vars++;
				decide.is_boolean_var.push_back(false);
				if (!decide.variable_entity_scope.empty()) {
					decide.variable_entity_scope.push_back(DConstants::INVALID_INDEX);
				}

				// Read the Big-M marker set by TagAbsConstraintsForBigM. Tag is
				// set on the BoundFunctionExpression alias before the rewrite.
				// Constraint context owns the tag; objective ABS sets needs_bigm
				// independently below in Phase 2 based on sense.
				bool needs_bigm = (func.alias == ABS_NEEDS_BIGM_TAG);

				// Stash the bound inner expression for constraint generation
				abs_pairs.push_back({aux_idx, func.children[0]->Copy(), in_objective, needs_bigm});

				// Replace ABS(inner) with aux var reference
				expr = make_uniq<BoundColumnRefExpression>(
				    aux_name, aux_type,
				    ColumnBinding(decide.decide_index, aux_idx));
				return;
			}
		}
	}

	// Recurse into children
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		FindAndReplaceAbs(child, decide, abs_pairs, in_objective);
	});
}

// ---------------------------------------------------------------------------
// Bilinear McCormick linearization (Boolean × anything)
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteBilinear(LogicalDecide &decide) {
	vector<LogicalDecide::BilinearLink> links;
	if (decide.decide_objective) {
		FindAndReplaceBilinear(decide.decide_objective, decide, links);
	}
	if (decide.decide_constraints) {
		FindAndReplaceBilinear(decide.decide_constraints, decide, links);
	}
	decide.bilinear_links = std::move(links);
}

//! Identify whether a bound expression is a single DECIDE variable reference
//! and return its index. Unwraps CAST nodes (DuckDB inserts implicit casts
//! when operand types differ, e.g. INTEGER * DOUBLE).
//! Returns INVALID_INDEX if not a single variable.
static idx_t GetSingleDecideVarIdx(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (colref.binding.table_index == decide_index) {
			return colref.binding.column_index;
		}
	}
	// Unwrap CAST nodes
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		return GetSingleDecideVarIdx(*cast.child, decide_index);
	}
	return DConstants::INVALID_INDEX;
}

//! Recursively find all DECIDE variable indices referenced in an expression
static void CollectDecideVarIndices(const Expression &expr, idx_t decide_index, vector<idx_t> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (colref.binding.table_index == decide_index) {
			out.push_back(colref.binding.column_index);
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		CollectDecideVarIndices(child, decide_index, out);
	});
}

bool DecideOptimizer::ExtractMultiplicativeCoefficient(const Expression &expr, idx_t decide_index,
                                                        idx_t var_idx, unique_ptr<Expression> &coef_out) {
	coef_out = nullptr;
	// Bare variable reference (possibly CAST-wrapped — GetSingleDecideVarIdx unwraps casts).
	idx_t found = GetSingleDecideVarIdx(expr, decide_index);
	if (found == var_idx) {
		return true;
	}
	// Unwrap CAST nodes — the binder inserts implicit casts around mixed-type
	// multiplications (e.g. `cost * b` becomes `CAST(CAST(cost) * CAST(b))`).
	// Walk through the cast to reach the underlying multiplication.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		auto &cast = expr.Cast<BoundCastExpression>();
		return ExtractMultiplicativeCoefficient(*cast.child, decide_index, var_idx, coef_out);
	}
	// Multiplication chain: walk down the side that contains the variable, multiply
	// coefficients harvested from the other side at each level.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (StringUtil::Lower(func.function.name) == "*" && func.children.size() == 2) {
			vector<idx_t> left_vars, right_vars;
			CollectDecideVarIndices(*func.children[0], decide_index, left_vars);
			CollectDecideVarIndices(*func.children[1], decide_index, right_vars);
			int var_side = -1;
			if (left_vars.size() == 1 && left_vars[0] == var_idx && right_vars.empty()) {
				var_side = 0;
			} else if (right_vars.size() == 1 && right_vars[0] == var_idx && left_vars.empty()) {
				var_side = 1;
			} else {
				return false;
			}
			unique_ptr<Expression> sub_coef;
			if (!ExtractMultiplicativeCoefficient(*func.children[var_side], decide_index, var_idx, sub_coef)) {
				return false;
			}
			auto outer_coef = func.children[1 - var_side]->Copy();
			if (sub_coef) {
				coef_out = optimizer.BindScalarFunction("*", std::move(outer_coef), std::move(sub_coef));
			} else {
				coef_out = std::move(outer_coef);
			}
			return true;
		}
	}
	return false;
}

void DecideOptimizer::FindAndReplaceBilinear(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                              vector<LogicalDecide::BilinearLink> &links) {
	if (!expr) return;

	if (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr->Cast<BoundFunctionExpression>();
		string fname = StringUtil::Lower(func.function.name);

		if (fname == "*" && func.children.size() == 2) {
			// Check if this is a bilinear product of two different decide variable expressions
			vector<idx_t> left_vars, right_vars;
			CollectDecideVarIndices(*func.children[0], decide.decide_index, left_vars);
			CollectDecideVarIndices(*func.children[1], decide.decide_index, right_vars);

			if (!left_vars.empty() && !right_vars.empty()) {
				// Both sides contain decide variables — this is bilinear (or identical QP)
				// Skip the identical-expression case (QP, not bilinear)
				if (func.children[0]->ToString() == func.children[1]->ToString()) {
					return; // Handled by existing QP pipeline
				}

				// Determine which variables are involved and their types.
				// For McCormick, we need exactly one Boolean factor.
				// First try GetSingleDecideVarIdx (bare var or CAST-wrapped),
				// then fall back to CollectDecideVarIndices for complex expressions
				// like (data_col * bool_var) where the decide var is buried in a multiply.
				idx_t left_single = GetSingleDecideVarIdx(*func.children[0], decide.decide_index);
				idx_t right_single = GetSingleDecideVarIdx(*func.children[1], decide.decide_index);

				// Fallback: if one side is a complex expression with exactly one decide var,
				// use that var's index. This handles cases like (profit * b) * x.
				if (left_single == DConstants::INVALID_INDEX && left_vars.size() == 1) {
					left_single = left_vars[0];
				}
				if (right_single == DConstants::INVALID_INDEX && right_vars.size() == 1) {
					right_single = right_vars[0];
				}

				bool left_is_bool = false, right_is_bool = false;
				if (left_single != DConstants::INVALID_INDEX && left_single < decide.is_boolean_var.size()) {
					left_is_bool = decide.is_boolean_var[left_single];
				}
				if (right_single != DConstants::INVALID_INDEX && right_single < decide.is_boolean_var.size()) {
					right_is_bool = decide.is_boolean_var[right_single];
				}

				// Only linearize if at least one side is a single Boolean variable
				if (!left_is_bool && !right_is_bool) {
					// Non-Boolean × Non-Boolean: leave for Q matrix (Phase 2)
					// Still recurse into children for nested bilinear
					ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
						FindAndReplaceBilinear(child, decide, links);
					});
					return;
				}

				// Decide which side is the Boolean (b) and which is the other (x)
				idx_t bool_var_idx, other_var_idx;
				Expression *bool_expr, *other_expr;
				if (left_is_bool) {
					bool_var_idx = left_single;
					other_var_idx = right_single; // may be INVALID_INDEX if right is complex
					bool_expr = func.children[0].get();
					other_expr = func.children[1].get();
				} else {
					bool_var_idx = right_single;
					other_var_idx = left_single; // may be INVALID_INDEX if left is complex
					bool_expr = func.children[1].get();
					other_expr = func.children[0].get();
				}

				// For Bool×Bool: special AND-linearization (simpler, no Big-M)
				bool both_bool = left_is_bool && right_is_bool;

				// Resolve the non-bool factor's variable index up-front so the aux type
				// decision below can consult it. This mirrors the fallback resolution
				// done in the non-both-bool branch (lines below), hoisted earlier.
				idx_t resolved_other_idx = other_var_idx;
				if (!both_bool && resolved_other_idx == DConstants::INVALID_INDEX) {
					vector<idx_t> other_vars_resolve;
					CollectDecideVarIndices(*other_expr, decide.decide_index, other_vars_resolve);
					if (other_vars_resolve.size() == 1) {
						resolved_other_idx = other_vars_resolve[0];
					}
				}

				// Create auxiliary variable
				idx_t aux_idx = decide.decide_variables.size();
				string aux_name = "__bilinear_aux_" + to_string(aux_idx) + "__";
				// Bool×Bool auxiliary is semantically boolean but uses INTEGER type to match
				// how user BOOLEAN variables are represented (INTEGER with 0/1 bounds).
				// Using BOOLEAN would cause type-mismatch errors when binding arithmetic.
				//
				// Bool×Integer: the product b * y with b ∈ {0,1} and y ∈ ℤ always takes
				// integer values, so declare the aux as INTEGER rather than DOUBLE. This
				// preserves integer-valuedness of the LHS through McCormick linearization,
				// which matters for the strict-inequality rewrite (`< K → <= K-1`) in
				// ilp_model_builder.cpp.
				bool other_is_integer_typed = false;
				if (!both_bool && resolved_other_idx != DConstants::INVALID_INDEX &&
				    resolved_other_idx < decide.decide_variables.size()) {
					auto &rt = decide.decide_variables[resolved_other_idx]->return_type;
					other_is_integer_typed = !(rt == LogicalType::DOUBLE || rt == LogicalType::FLOAT);
				}
				LogicalType aux_type = (both_bool || other_is_integer_typed)
				                           ? LogicalType::INTEGER
				                           : LogicalType::DOUBLE;
				auto aux_var = make_uniq<BoundColumnRefExpression>(
				    aux_name, aux_type, ColumnBinding(decide.decide_index, aux_idx));
				decide.decide_variables.push_back(std::move(aux_var));
				decide.num_auxiliary_vars++;
				decide.is_boolean_var.push_back(both_bool);
				if (!decide.variable_entity_scope.empty()) {
					decide.variable_entity_scope.push_back(DConstants::INVALID_INDEX); // row-scoped
				}

				if (both_bool) {
					// AND-linearization: w <= b1, w <= b2, w >= b1 + b2 - 1
					auto &b1_ref = decide.decide_variables[bool_var_idx]->Cast<BoundColumnRefExpression>();
					auto &b2_ref = decide.decide_variables[other_var_idx]->Cast<BoundColumnRefExpression>();
					auto &w_ref = decide.decide_variables[aux_idx]->Cast<BoundColumnRefExpression>();

					// w <= b1
					auto c1 = make_uniq<BoundComparisonExpression>(
					    ExpressionType::COMPARE_LESSTHANOREQUALTO,
					    make_uniq<BoundColumnRefExpression>(w_ref.alias, w_ref.return_type, w_ref.binding),
					    make_uniq<BoundColumnRefExpression>(b1_ref.alias, b1_ref.return_type, b1_ref.binding));
					AppendConstraint(decide, std::move(c1));

					// w <= b2
					auto c2 = make_uniq<BoundComparisonExpression>(
					    ExpressionType::COMPARE_LESSTHANOREQUALTO,
					    make_uniq<BoundColumnRefExpression>(w_ref.alias, w_ref.return_type, w_ref.binding),
					    make_uniq<BoundColumnRefExpression>(b2_ref.alias, b2_ref.return_type, b2_ref.binding));
					AppendConstraint(decide, std::move(c2));

					// w >= b1 + b2 - 1  (i.e., b1 + b2 - w <= 1)
					auto b1_plus_b2 = optimizer.BindScalarFunction(
					    "+",
					    make_uniq<BoundColumnRefExpression>(b1_ref.alias, b1_ref.return_type, b1_ref.binding),
					    make_uniq<BoundColumnRefExpression>(b2_ref.alias, b2_ref.return_type, b2_ref.binding));
					auto b1_plus_b2_minus_w = optimizer.BindScalarFunction(
					    "-",
					    std::move(b1_plus_b2),
					    make_uniq<BoundColumnRefExpression>(w_ref.alias, w_ref.return_type, w_ref.binding));
					auto c3 = make_uniq<BoundComparisonExpression>(
					    ExpressionType::COMPARE_LESSTHANOREQUALTO,
					    std::move(b1_plus_b2_minus_w),
					    make_uniq<BoundConstantExpression>(Value::INTEGER(1)));
					AppendConstraint(decide, std::move(c3));
				} else {
					// Bool × Non-Bool McCormick: w <= x (structural, at plan time)
					// w <= U*b and w >= x - U*(1-b) generated at execution time via BilinearLink

					// Resolve other_var_idx if other_expr is a complex single-variable expression.
					if (other_var_idx == DConstants::INVALID_INDEX) {
						vector<idx_t> other_vars;
						CollectDecideVarIndices(*other_expr, decide.decide_index, other_vars);
						if (other_vars.size() == 1) {
							other_var_idx = other_vars[0];
						} else {
							throw BinderException(
							    "Bilinear product of a Boolean variable with a multi-variable expression "
							    "is not yet supported. Use a simple variable reference (e.g., b * x, not b * (x + y)).");
						}
					}

					auto &w_ref = decide.decide_variables[aux_idx]->Cast<BoundColumnRefExpression>();
					auto &other_var_ref = decide.decide_variables[other_var_idx]->Cast<BoundColumnRefExpression>();

					// w <= x  (structural constraint, no Big-M needed). Use the bare other-variable
					// reference even when the source had a coefficient: the McCormick relation
					// w = b*x must be tight, and any coefficient gets folded into the replacement
					// expression below.
					auto c_struct = make_uniq<BoundComparisonExpression>(
					    ExpressionType::COMPARE_LESSTHANOREQUALTO,
					    make_uniq<BoundColumnRefExpression>(w_ref.alias, w_ref.return_type, w_ref.binding),
					    make_uniq<BoundColumnRefExpression>(other_var_ref.alias, other_var_ref.return_type, other_var_ref.binding));
					AppendConstraint(decide, std::move(c_struct));

					LogicalDecide::BilinearLink link;
					link.aux_idx = aux_idx;
					link.bool_var_idx = bool_var_idx;
					link.other_var_idx = other_var_idx;
					links.push_back(link);
				}

				// Replace the bilinear product with the auxiliary variable reference,
				// folding in any data coefficients that were attached to either factor
				// (e.g., `cost * b * x` parses as `(cost*b)*x` — without folding, `cost`
				// would be silently dropped).
				unique_ptr<Expression> bool_coef, other_coef;
				ExtractMultiplicativeCoefficient(*bool_expr, decide.decide_index, bool_var_idx, bool_coef);
				ExtractMultiplicativeCoefficient(*other_expr, decide.decide_index, other_var_idx, other_coef);

				auto &w_ref = decide.decide_variables[aux_idx]->Cast<BoundColumnRefExpression>();
				auto w_replacement = make_uniq<BoundColumnRefExpression>(
				    w_ref.alias, w_ref.return_type, w_ref.binding);

				unique_ptr<Expression> combined_coef;
				if (bool_coef && other_coef) {
					combined_coef = optimizer.BindScalarFunction("*", std::move(bool_coef), std::move(other_coef));
				} else if (bool_coef) {
					combined_coef = std::move(bool_coef);
				} else if (other_coef) {
					combined_coef = std::move(other_coef);
				}

				if (combined_coef) {
					expr = optimizer.BindScalarFunction("*", std::move(combined_coef), std::move(w_replacement));
				} else {
					expr = std::move(w_replacement);
				}
				return;
			}
		}
	}

	// Recurse into children
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		FindAndReplaceBilinear(child, decide, links);
	});
}

void DecideOptimizer::AppendConstraint(LogicalDecide &decide, unique_ptr<Expression> constraint) {
	if (decide.decide_constraints) {
		auto conj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		conj->children.push_back(std::move(decide.decide_constraints));
		conj->children.push_back(std::move(constraint));
		decide.decide_constraints = std::move(conj);
	} else {
		decide.decide_constraints = std::move(constraint);
	}
}

} // namespace duckdb
