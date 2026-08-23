#include "duckdb/optimizer/decide_optimizer.hpp"

#include "duckdb/decidb/decide_cast_policy.hpp"
#include "duckdb/decidb/ilp_solver.hpp"

#include <cstdlib>
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/decide_solver_gate.hpp"
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
#include "duckdb/planner/operator/logical_decide.hpp"
#include "duckdb/decidb/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"

namespace duckdb {

static ObjectiveAggregateType StrToAggType(const string &name) {
	if (name == "sum") return ObjectiveAggregateType::SUM;
	if (name == "min") return ObjectiveAggregateType::MIN_AGG;
	if (name == "max") return ObjectiveAggregateType::MAX_AGG;
	return ObjectiveAggregateType::NONE;
}

// Forward declaration: defined further down with the ABS soundness handling.
static void TagAbsConstraintsForBigM(ClientContext &context, LogicalDecide &decide);

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
	bool bench = std::getenv("DECIDB_BENCH") != nullptr;
	Profiler timer;
	if (bench) {
		timer.Start();
	}

	// Choose the solver, and with it the formulation, BEFORE any rewrite runs. Every
	// pass below decides how to express a construct, and the right answer depends on
	// what the backend can take natively — so both have to be settled first, and settled
	// only once. From here they ride the plan (LogicalDecide::solver_backend_name and
	// ::use_native_constructs → PhysicalDecide) all the way to the solve and to any
	// diagnostic re-solve, so nothing downstream ever decides a second time.
	ChooseDecideSolver(decide);

	RewriteNorm(decide);
	RewriteInDomain(decide);
	TagAbsConstraintsForBigM(optimizer.context, decide); // Must run before RewriteAbs: marks ABS nodes that need Big-M
	RewriteAbs(decide);          // Must run first: creates aux vars replacing ABS nodes
	RewriteBilinear(decide);     // McCormick linearization for Boolean × anything bilinear products
	RewriteComposedMinMax(decide); // Detect composed MIN/MAX before single-term MIN/MAX rewrite
	RewriteMinMax(decide);       // Classify + rewrite min/max (creates indicators and SUM nodes)
	RewriteNotEqual(decide);
	RewriteAvgToSum(decide);
	// Must stay last among the rewrites: RewriteInDomain emits a floor-lowering bound
	// that is itself absorbable, and every auxiliary variable must exist before the
	// box is sized.
	AbsorbVariableBounds(decide);

	if (bench) {
		timer.End();
		fprintf(stderr, "DECIDB_BENCH: optimizer_ms=%.2f\n", timer.Elapsed() * 1000.0);
	}
}

// ---------------------------------------------------------------------------
// Bound DECIDE syntax markers: NORM and IN
// ---------------------------------------------------------------------------

static bool TryParseNormMarker(const string &alias, string &payload) {
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

static void CopySourceClauseTag(const string &from_alias, Expression &to) {
	idx_t source_id;
	if (!TryParseSourceClauseTag(from_alias, source_id)) {
		return;
	}
	auto alias = to.GetAlias();
	AddDecideTag(alias, MakeSourceClauseTag(source_id));
	to.SetAlias(std::move(alias));
}

static void MarkFormulationConstraint(Expression &expr, const string &source_alias) {
	auto alias = expr.GetAlias();
	AddDecideTag(alias, STRUCTURAL_CONSTRAINT_TAG);
	expr.SetAlias(std::move(alias));
	CopySourceClauseTag(source_alias, expr);
}

static const BoundColumnRefExpression *FindDecideColumn(const Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &col = expr.Cast<BoundColumnRefExpression>();
		return col.binding.table_index == decide_index ? &col : nullptr;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		return FindDecideColumn(*expr.Cast<BoundCastExpression>().child, decide_index);
	}
	return nullptr;
}

static bool TryGetFoldableDouble(const Expression &expr, double &value) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		try {
			value = expr.Cast<BoundConstantExpression>().value.GetValue<double>();
			return true;
		} catch (...) {
			return false;
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		return TryGetFoldableDouble(*expr.Cast<BoundCastExpression>().child, value);
	}
	return false;
}

static unique_ptr<Expression> WrapWithWhen(unique_ptr<Expression> constraint, const Expression *when) {
	if (!when) {
		return constraint;
	}
	auto wrapper = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	wrapper->children.push_back(std::move(constraint));
	wrapper->children.push_back(when->Copy());
	wrapper->alias = WHEN_CONSTRAINT_TAG;
	return std::move(wrapper);
}

static bool IsWhenWrapper(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	       HasDecideTag(expr.GetAlias(), WHEN_CONSTRAINT_TAG) &&
	       expr.Cast<BoundConjunctionExpression>().children.size() == 2;
}

static unique_ptr<Expression> MakeTrueExpression() {
	return make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
}

void DecideOptimizer::RewriteNorm(LogicalDecide &decide) {
	vector<unique_ptr<Expression>> links;
	idx_t l0_counter = 0;

	std::function<void(unique_ptr<Expression> &, const string &)> rewrite =
	    [&](unique_ptr<Expression> &expr, const string &source_alias) {
		if (!expr) return;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
			auto &comparison = expr->Cast<BoundComparisonExpression>();
			rewrite(comparison.left, comparison.GetAlias());
			rewrite(comparison.right, comparison.GetAlias());
			return;
		}
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			for (auto &child : expr->Cast<BoundConjunctionExpression>().children) rewrite(child, source_alias);
			return;
		}
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) return;
		auto &aggregate = expr->Cast<BoundAggregateExpression>();
		string payload;
		if (!TryParseNormMarker(aggregate.GetAlias(), payload)) return;
		if (aggregate.children.size() != 1) {
			throw InternalException("DECIDE NORM marker must contain one bound expression");
		}
		auto old_alias = aggregate.GetAlias();
		auto make_aggregate = [&](const string &name, unique_ptr<Expression> child) {
			vector<unique_ptr<Expression>> children;
			children.push_back(std::move(child));
			auto result = optimizer.BindAggregateFunction(name, std::move(children));
			result->alias = old_alias;
			RemoveDecideTag(result->alias, string(NORM_MARKER_TAG_PREFIX) + payload + "__");
			if (aggregate.filter) result->Cast<BoundAggregateExpression>().filter = aggregate.filter->Copy();
			return result;
		};
		if (payload == "1") {
			expr = make_aggregate("sum", optimizer.BindScalarFunction("abs", aggregate.children[0]->Copy()));
			return;
		}
		if (payload == "2") {
			expr = make_aggregate("sum", optimizer.BindScalarFunction(
			    "power", aggregate.children[0]->Copy(), make_uniq<BoundConstantExpression>(Value::INTEGER(2))));
			return;
		}
		if (payload == "inf") {
			expr = make_aggregate("max", optimizer.BindScalarFunction("abs", aggregate.children[0]->Copy()));
			return;
		}
		bool auto_m = payload == "0_auto";
		if (!auto_m && payload.rfind("0_", 0) != 0) {
			throw InternalException("DECIDE NORM marker has invalid order payload '%s'", payload);
		}
		double m = 1.0;
		if (!auto_m) {
			try { m = std::stod(payload.substr(2)); } catch (...) {
				throw InternalException("DECIDE NORM marker has invalid L0 bound '%s'", payload);
			}
		}
		idx_t z_idx = decide.decide_variables.size();
		string z_name = (auto_m ? "__l0auto_ind_" : "__l0_ind_") + to_string(l0_counter++) + "__";
		auto z = make_uniq<BoundColumnRefExpression>(z_name, LogicalType::INTEGER,
		                                             ColumnBinding(decide.decide_index, z_idx));
		decide.decide_variables.push_back(z->Copy());
		decide.num_auxiliary_vars++;
		decide.is_boolean_var.push_back(true);
		if (!decide.variable_scopes.empty()) decide.variable_scopes.push_back(DecideVarScopeInfo::Row());
		auto make_mz = [&]() { return optimizer.BindScalarFunction("*",
		    make_uniq<BoundConstantExpression>(Value::DOUBLE(m)), z->Copy()); };
		auto add_link = [&](unique_ptr<Expression> lhs, unique_ptr<Expression> rhs) {
			auto link = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
			                                                  std::move(lhs), std::move(rhs));
			MarkFormulationConstraint(*link, source_alias);
			links.push_back(std::move(link));
		};
		add_link(make_mz(), aggregate.children[0]->Copy());
		add_link(make_mz(), optimizer.BindScalarFunction("-", make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)),
		                                               aggregate.children[0]->Copy()));
		add_link(optimizer.BindScalarFunction("abs", aggregate.children[0]->Copy()),
		         optimizer.BindScalarFunction("*", make_uniq<BoundConstantExpression>(Value::DOUBLE(GetDecideL0Tolerance(optimizer.context))), z->Copy()));
		expr = make_aggregate("sum", std::move(z));
	};

	rewrite(decide.decide_objective, string());
	rewrite(decide.decide_constraints, string());
	for (auto &link : links) AppendConstraint(decide, std::move(link));
}

void DecideOptimizer::RewriteInDomain(LogicalDecide &decide) {
	vector<unique_ptr<Expression>> generated;
	idx_t in_counter = 0;
	auto emit = [&](unique_ptr<Expression> constraint, const Expression *when, const string &source_alias) {
		MarkFormulationConstraint(*constraint, source_alias);
		generated.push_back(WrapWithWhen(std::move(constraint), when));
	};
	std::function<void(unique_ptr<Expression> &, const Expression *, const string &)> rewrite =
	    [&](unique_ptr<Expression> &expr, const Expression *when, const string &source_alias) {
		if (!expr) return;
		if (IsWhenWrapper(*expr)) {
			auto &wrapper = expr->Cast<BoundConjunctionExpression>();
			rewrite(wrapper.children[0], wrapper.children[1].get(), source_alias);
			if (wrapper.children[0]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
			    wrapper.children[0]->Cast<BoundConstantExpression>().value.GetValue<bool>()) expr = MakeTrueExpression();
			return;
		}
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			for (auto &child : expr->Cast<BoundConjunctionExpression>().children) rewrite(child, when, source_alias);
			return;
		}
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_OPERATOR || expr->type != ExpressionType::COMPARE_IN) return;
		auto &in = expr->Cast<BoundOperatorExpression>();
		if (in.children.size() < 2) throw InternalException("DECIDE IN marker has no domain values");
		auto *target = FindDecideColumn(*in.children[0], decide.decide_index);
		if (!target) throw InternalException("DECIDE IN marker target is not a decision variable");
		string local_source = source_alias.empty() ? in.GetAlias() : source_alias;
		idx_t k = in.children.size() - 1;
		if (target->binding.column_index < decide.is_boolean_var.size() && decide.is_boolean_var[target->binding.column_index] && k == 2) {
			double a, b;
			if (TryGetFoldableDouble(*in.children[1], a) && TryGetFoldableDouble(*in.children[2], b) &&
			    ((a == 0.0 && b == 1.0) || (a == 1.0 && b == 0.0))) {
				emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
				                                   in.children[0]->Copy(), make_uniq<BoundConstantExpression>(Value::INTEGER(0))), when, local_source);
				expr = MakeTrueExpression();
				return;
			}
		}
		if (k == 1) {
			emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, in.children[0]->Copy(), in.children[1]->Copy()), when, local_source);
			expr = MakeTrueExpression();
			return;
		}
		vector<unique_ptr<Expression>> indicators;
		for (idx_t i = 0; i < k; i++) {
			idx_t ind_idx = decide.decide_variables.size();
			string name = "__in_ind_" + target->GetName() + "_" + to_string(in_counter) + "_" + to_string(i) + "__";
			auto indicator = make_uniq<BoundColumnRefExpression>(name, LogicalType::INTEGER,
			                                                    ColumnBinding(decide.decide_index, ind_idx));
			decide.decide_variables.push_back(indicator->Copy());
			decide.num_auxiliary_vars++;
			decide.is_boolean_var.push_back(true);
			if (!decide.variable_scopes.empty()) decide.variable_scopes.push_back(DecideVarScopeInfo::Row());
			indicators.push_back(std::move(indicator));
		}
		in_counter++;
		auto cardinality = indicators[0]->Copy();
		for (idx_t i = 1; i < k; i++) cardinality = optimizer.BindScalarFunction("+", std::move(cardinality), indicators[i]->Copy());
		emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(cardinality),
		                                         make_uniq<BoundConstantExpression>(Value::INTEGER(1))), when, local_source);
		auto linking = in.children[0]->Copy();
		bool all_constant = true;
		double min_value = 0.0;
		for (idx_t i = 0; i < k; i++) {
			double value;
			if (!TryGetFoldableDouble(*in.children[i + 1], value)) all_constant = false;
			else min_value = std::min(min_value, value);
			auto negative = optimizer.BindScalarFunction("-", make_uniq<BoundConstantExpression>(Value::INTEGER(0)), in.children[i + 1]->Copy());
			linking = optimizer.BindScalarFunction("+", std::move(linking), optimizer.BindScalarFunction("*", std::move(negative), indicators[i]->Copy()));
		}
		emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(linking),
		                                         make_uniq<BoundConstantExpression>(Value::INTEGER(0))), when, local_source);
		if (all_constant && min_value < 0.0) {
			emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO, in.children[0]->Copy(),
			                                         make_uniq<BoundConstantExpression>(Value::DOUBLE(min_value))), when, local_source);
		}
		expr = MakeTrueExpression();
	};
	rewrite(decide.decide_constraints, nullptr, string());
	for (auto &constraint : generated) AppendConstraint(decide, std::move(constraint));
}

// ---------------------------------------------------------------------------
// A factor sitting on a reducer
// ---------------------------------------------------------------------------
//
// The canonicalizer peels a factor OUTWARD off a reducer (`2 * SUM(x*p)`) and
// converges every spelling onto one. It stays outside from there: nothing pushes
// it back into the reducer's body.
//
// WHY IT STAYS OUTSIDE. `MIN`/`MAX` are order statistics, not linear functionals.
// They commute with a POSITIVE factor only -- `MAX(-2x)` is `-2*MIN(x)`, not
// `-2*MAX(x)` -- so pushing a factor in requires knowing its sign, and a scalar
// subquery's sign is not known until the query runs. Leaving the factor outside
// makes the sign irrelevant to CORRECTNESS: it only selects which linearization is
// cheaper. An unknown sign then costs performance instead of failing.
//
// (A previous version of this pass did fold, swapping MIN/MAX for a negative
// factor. It was exact, but it made the sign load-bearing for correctness and so
// had nothing to fall back on when the sign was unknown.)
//
// Where the factor is finally applied depends on the term:
//   SUM/AVG   -- multiplied into the per-row coefficients
//               (`PhysicalDecide::ApplyScaleToExtracted`)
//   MIN/MAX   -- multiplied into the auxiliary's contribution
//               (`ComposedMinMaxTerm::scale`), or distributed over the per-row
//               form when the constraint linearizes the easy way.
static bool TryEvaluateFoldableDoubleNoThrow(ClientContext &context, const Expression &expr, double &out) {
	try {
		return TryEvaluateFoldableDouble(context, expr, out);
	} catch (...) {
		return false;
	}
}

//! The sign a factor contributes: +1, -1, or 0 when it is not known until the query
//! runs. Only a plain literal is decidable here; a scalar subquery is not.
//!
//! Nothing depends on this for correctness -- it selects between an exact cheap
//! linearization and an exact expensive one. 0 must therefore be treated as "assume
//! the expensive one", never as an error.
static int ScaleSignAtPlanTime(ClientContext &context, const Expression *scale, bool divides) {
	if (!scale) {
		return 1;
	}
	double d;
	if (!TryEvaluateFoldableDoubleNoThrow(context, *scale, d)) {
		return 0;
	}
	if (d == 0.0) {
		// A zero factor annihilates the term. Dividing by it is an error the
		// evaluator will raise; multiplying by it makes the sign irrelevant, and
		// "positive" keeps the cheap path (every coefficient is zero either way).
		return divides ? 0 : 1;
	}
	return d < 0.0 ? -1 : 1;
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

//! User-facing rendering of a `<>` comparand for diagnosis labels: unwrap the implicit
//! CAST the binder inserts around a literal so `x <> 1` reads `x <> 1`, not
//! `x <> CAST(1 AS INTEGER)`. Falls through to the raw ToString for anything else.
static string DiagnosisComparand(const Expression &expr) {
	const Expression *cur = StripCastsForIdentity(expr);
	return cur->ToString();
}

void DecideOptimizer::FindNotEqualConstraints(Expression &expr, LogicalDecide &decide) {
	// Handle WHEN/PER wrappers: BoundConjunctionExpression with alias tag
	// Recurse into child[0] (the actual constraint)
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) || IsPerConstraintTag(conj.alias)) {
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
			if (!decide.variable_scopes.empty()) {
				decide.variable_scopes.push_back(DecideVarScopeInfo::Row());
			}
			// F6: record the user's original <> comparison for diagnosis naming
			decide.aux_var_expressions.emplace_back(
			    ind_idx, DiagnosisComparand(*comp.left) + " <> " + DiagnosisComparand(*comp.right));
			// Tag the comparison with the indicator index for direct matching
			AddDecideTag(comp.alias, string(NE_INDICATOR_TAG_PREFIX) + to_string(ind_idx) + "__");
		}
	}
}

// ---------------------------------------------------------------------------
// AVG → SUM rewrite
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteAvgToSum(LogicalDecide &decide) {
	if (decide.decide_constraints) {
		RewriteAvgInExpression(decide.decide_constraints, decide.decide_index);
	}
	if (decide.decide_objective) {
		// Rewrites mutate in place, so the objective is detached, rewritten, and
		// reinstalled through the boundary rather than edited where it sits.
		auto objective = std::move(decide.decide_objective);
		RewriteAvgInExpression(objective, decide.decide_index);
		decide.SetObjective(optimizer.context, std::move(objective));
	}
}

void DecideOptimizer::RewriteAvgInExpression(unique_ptr<Expression> &expr, idx_t decide_index) {
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
		// A decision-free AVG is left alone. The rewrite exists only to linearize a
		// decision-bearing AVG into `1/N * x_i` coefficients; a data-only AVG has
		// nothing to linearize, it is just a number the right-hand side evaluates.
		// Rewriting it is actively harmful there: BindAggregateFunction("sum", ...)
		// redeclares the node with SUM's type (HUGEINT where AVG is DOUBLE), and the
		// right-hand side must hand its value back to the surrounding expression,
		// which was bound against that declared type -- so the fractional part is
		// cast away. Leaving it as a real AVG keeps the round trip DOUBLE->DOUBLE.
		if (StringUtil::CIEquals(agg.function.name, "avg") && agg.children.size() == 1 &&
		    BoundExpressionReferencesDecide(*inner, decide_index)) {
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
			// Carry the AVG's own tags across — a relation qualifier was stamped by
			// the binder and still names the relation the SUM de-duplicates by.
			new_sum->alias = agg.alias;
			AddDecideTag(new_sum->alias, AVG_REWRITE_TAG);

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
		RewriteAvgInExpression(child, decide_index);
	});
}

// ---------------------------------------------------------------------------
// ABS-in-constraint soundness handling (must run BEFORE RewriteAbs)
// ---------------------------------------------------------------------------
//
// DecidB's ABS linearization replaces `ABS(e)` with an auxiliary `aux` and
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
			if (BoundExpressionReferencesDecide(*func.children[0], decide_index)) {
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
			if (BoundExpressionReferencesDecide(*func.children[0], decide_index)) {
				AddDecideTag(func.alias, ABS_NEEDS_BIGM_TAG);
				return;
			}
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		TagAbsForBigM(child, decide_index);
	});
}

// Sign of a complete foldable expression. Data casts are evaluated, never peeled.
// Returns false when the expression is not a statically-known number.
static bool TryGetConstantSign(ClientContext &context, const Expression &expr, int &out_sign) {
	double value;
	if (!TryEvaluateFoldableDoubleNoThrow(context, expr, value)) {
		return false;
	}
	out_sign = value < 0 ? -1 : 1;
	return true;
}

// Collect every ABS-over-decide-var node together with the sign it carries once
// the constraint is expanded additively. `sign` is the sign of the path taken to
// reach the current node.
//
// Only structure whose sign is known at plan time is traversed with a flip: `+`,
// binary and unary `-`, casts, aggregate bodies, and numeric literal factors. A
// factor whose sign is not known until execution -- a data column, as in
// `SUM(w * ABS(x - t))` -- yields sign 0, "unknown", which never matches the
// pinning direction and so forces Big-M. That is conservative: for `w >= 0` the
// aux would have been pinned without it. Deciding per row at execution time
// would recover those rows, but the indicator is allocated at plan time, so it
// cannot be chosen lazily as the code stands.
static void CollectAbsWithSign(ClientContext &context, Expression &expr, int sign, idx_t decide_index,
                               vector<std::pair<Expression *, int>> &out) {
	if (!ContainsAbsOverDecideVar(expr, decide_index)) {
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		CollectAbsWithSign(context, *expr.Cast<BoundCastExpression>().child, sign, decide_index, out);
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		auto name = StringUtil::Lower(func.function.name);
		if (StringUtil::CIEquals(name, "abs") && func.children.size() == 1 &&
		    BoundExpressionReferencesDecide(*func.children[0], decide_index)) {
			// The ABS node itself is the unit that gets tagged; a nested ABS inside
			// it is subsumed, exactly as TagAbsForBigM stops here.
			out.emplace_back(&expr, sign);
			return;
		}
		if (name == "+") {
			for (auto &child : func.children) {
				CollectAbsWithSign(context, *child, sign, decide_index, out);
			}
			return;
		}
		if (name == "-" && func.children.size() == 2) {
			CollectAbsWithSign(context, *func.children[0], sign, decide_index, out);
			CollectAbsWithSign(context, *func.children[1], -sign, decide_index, out);
			return;
		}
		if (name == "-" && func.children.size() == 1) {
			CollectAbsWithSign(context, *func.children[0], -sign, decide_index, out);
			return;
		}
		if (name == "*" || name == "/") {
			// Fold the sign of every constant factor. For division only the numerator
			// can carry an ABS, but the divisor's sign still applies.
			//
			// A factor that is not a statically-known number and does not itself carry
			// the ABS is a data-dependent multiplier -- `w` in `SUM(w * ABS(x - t))`.
			// Its sign is unknown until execution, so the whole path sign becomes 0.
			// An ABS-bearing factor is the term being collected rather than a
			// multiplier, and is non-negative by construction, so it never flips.
			int factor_sign = 1;
			for (auto &child : func.children) {
				int child_sign;
				if (TryGetConstantSign(context, *child, child_sign)) {
					factor_sign *= child_sign;
				} else if (!ContainsAbsOverDecideVar(*child, decide_index)) {
					factor_sign = 0;
				}
			}
			for (auto &child : func.children) {
				int ignored;
				if (!TryGetConstantSign(context, *child, ignored)) {
					CollectAbsWithSign(context, *child, sign * factor_sign, decide_index, out);
				}
			}
			return;
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		// Aggregates compose: when the constraint bounds the aggregate's value in
		// the pinning direction, the auxes underneath are pinned transitively.
		for (auto &child : expr.Cast<BoundAggregateExpression>().children) {
			CollectAbsWithSign(context, *child, sign, decide_index, out);
		}
		return;
	}
	// Anything else (subqueries, CASE, operators with no sign discipline): the
	// sign of the path is unknown, so the safe answer is "not pinned". Hand the
	// contained ABS nodes back with sign 0, which never classifies as pinned.
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
		CollectAbsWithSign(context, child, 0, decide_index, out);
	});
}

static void ClassifyAbsConstraints(ClientContext &context, Expression &expr, idx_t decide_index) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		// WHEN/PER wrappers: child[0] is the inner constraint; recurse into it.
		// The WHEN/PER filter only affects which rows participate in the
		// aggregate; aux pinning is unconditional per row, so the wrapper
		// doesn't change classification.
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) || IsPerConstraintTag(conj.alias)) {
			if (!conj.children.empty()) {
				ClassifyAbsConstraints(context, *conj.children[0], decide_index);
			}
			return;
		}
		for (auto &child : conj.children) {
			ClassifyAbsConstraints(context, *child, decide_index);
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
		// Read the constraint as `E <op> 0` with `E = LHS - RHS`: every ABS is then
		// a single signed term of one expression, and "does this comparison push the
		// aux down?" is a question about that term's sign rather than about which
		// side of the relation it happens to be written on. That distinction is
		// invisible before canonicalization -- which never moved a term across the
		// relation until it ran ahead of this pass -- and load-bearing after it.
		int pinning_sign;
		if (t == ExpressionType::COMPARE_LESSTHAN || t == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
			pinning_sign = 1; // E bounded above: a positive term is pushed down
		} else if (t == ExpressionType::COMPARE_GREATERTHAN ||
		           t == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
			pinning_sign = -1; // E bounded below: a negative term is pushed down
		} else {
			pinning_sign = 0; // equality, <>: pin nothing, exactly as before
		}

		vector<std::pair<Expression *, int>> signed_abs;
		CollectAbsWithSign(context, *comp.left, 1, decide_index, signed_abs);
		CollectAbsWithSign(context, *comp.right, -1, decide_index, signed_abs);
		for (auto &entry : signed_abs) {
			if (pinning_sign == 0 || entry.second != pinning_sign) {
				TagAbsForBigM(*entry.first, decide_index);
			}
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

static void TagAbsConstraintsForBigM(ClientContext &context, LogicalDecide &decide) {
	if (!decide.decide_constraints) {
		return;
	}
	ClassifyAbsConstraints(context, *decide.decide_constraints, decide.decide_index);
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

// True if the expression is a BOUND_FUNCTION for `+` (after unwrapping cast).
static bool IsAddNode(const Expression &e, idx_t decide_index) {
	auto &u = (*UnwrapDecideCasts(e, decide_index));
	if (u.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) return false;
	return StringUtil::Lower(u.Cast<BoundFunctionExpression>().function.name) == "+";
}

// True if the expression is a `-` function (both binary and unary).
static bool IsSubNode(const Expression &e, idx_t decide_index) {
	auto &u = (*UnwrapDecideCasts(e, decide_index));
	if (u.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) return false;
	return StringUtil::Lower(u.Cast<BoundFunctionExpression>().function.name) == "-";
}

// True if any MIN/MAX aggregate over a decide var appears at or below the node.
// Recurses through any function node's children (not just +/-), so shapes like
// `2 * MIN(...)` are detected and can be rejected with a clean binder error.
static bool AdditiveContainsMinMax(const Expression &e, idx_t decide_index) {
	auto &u = (*UnwrapDecideCasts(e, decide_index));
	if (u.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &agg = u.Cast<BoundAggregateExpression>();
		auto name = StringUtil::Lower(agg.function.name);
		if ((name == "min" || name == "max") && agg.children.size() == 1 &&
		    BoundExpressionReferencesDecide(*agg.children[0], decide_index)) {
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
static void WalkComposedLhs(ClientContext &context, const Expression &e, int sign, idx_t decide_index,
                             bool outer_push_down,
                             vector<LogicalDecide::ComposedMinMaxTerm> &out_terms) {
	auto &u = (*UnwrapDecideCasts(e, decide_index));
	if (IsAddNode(u, decide_index)) {
		auto &fn = u.Cast<BoundFunctionExpression>();
		for (auto &child : fn.children) {
			WalkComposedLhs(context, *child, sign, decide_index, outer_push_down, out_terms);
		}
		return;
	}
	if (IsSubNode(u, decide_index)) {
		// Subtraction flips the direction each term is pushed. `sign` already
		// carries that through to the easy/hard classification below, and the
		// physical layer is sign-generic, so descending is all that is required.
		// This has to work: canonicalization moves decision terms onto the LHS,
		// and a term that crosses the relation arrives negated.
		auto &fn = u.Cast<BoundFunctionExpression>();
		if (fn.children.size() == 2) {
			WalkComposedLhs(context, *fn.children[0], sign, decide_index, outer_push_down, out_terms);
			WalkComposedLhs(context, *fn.children[1], -sign, decide_index, outer_push_down, out_terms);
			return;
		}
		if (fn.children.size() == 1) {
			WalkComposedLhs(context, *fn.children[0], -sign, decide_index, outer_push_down, out_terms);
			return;
		}
	}
	// A zero constant contributes nothing. It reaches here as the head of the
	// canonicalizer's `0 - term` idiom for a leading negative term, so rejecting
	// it would reject shapes purely on how the negation was spelled.
	if (u.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		auto &val = u.Cast<BoundConstantExpression>().value;
		if (!val.IsNull() && val.type().IsNumeric() && val.GetValue<double>() == 0.0) {
			return;
		}
	}
	// A factor on this term (`2 * MIN(...)`) rides alongside it rather than being
	// pushed into the aggregate. Its sign joins the easy/hard classification below.
	ScaledAggregateMatch scale_match;
	const BoundAggregateExpression *scaled_agg =
	    TryMatchScaledAggregate(u, decide_index, scale_match) ? scale_match.aggregate : nullptr;
	auto scale = scale_match.scale;
	bool scale_divides = scale_match.divides;

	if (!scaled_agg && u.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		throw BinderException(
		    "Composed MIN/MAX in DECIDE v1 supports only additive sums of SUM/AVG/MIN/MAX aggregates. "
		    "Got non-aggregate term: %s",
		    e.ToString());
	}
	auto &agg = scaled_agg ? *scaled_agg : u.Cast<BoundAggregateExpression>();
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
	if (scale) {
		term.scale = scale->Copy();
		term.scale_divides = scale_divides;
	}
	// Carry the relation qualifier (`SUM(D: ...)`) off the tag the binder stamped on the
	// aggregate. Without this the composed path reduces over join-result rows and a
	// qualified reducer silently reverts to row semantics.
	TryParseQualifiedReducerTag(agg.alias, term.qualifier_scope_idx);
	if (term.kind == LogicalDecide::ComposedMinMaxTerm::MINMAX_KIND) {
		bool is_max = (name == "max");
		// A negative FACTOR flips which way z is pushed exactly as a subtraction does,
		// so the two combine into one effective sign. `-2 * MAX(e)` under `<=` pushes
		// MAX up, the hard direction, just as `- MAX(e)` would.
		int scale_sign = ScaleSignAtPlanTime(context, scale, scale_divides);
		if (scale_sign == 0) {
			// The factor's sign is not known until the query runs, so neither is the
			// cheap direction. The indicator layer pins z to the true MIN/MAX in BOTH
			// directions, which is correct for either sign -- pay for it rather than
			// guess. This is the case that used to be rejected outright.
			term.is_easy = false;
		} else {
			int effective_sign = sign * scale_sign;
			// z_k pushed down if the outer wants LHS small and this term's effective
			// sign is +, or outer wants LHS large and it is -.
			bool z_pushed_down = (effective_sign > 0) ? outer_push_down : !outer_push_down;
			// Easy: MAX pushed down, or MIN pushed up.
			term.is_easy = (is_max && z_pushed_down) || (!is_max && !z_pushed_down);
		}
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
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) || IsPerConstraintTag(conj.alias)) {
			// If the wrapped constraint is composed MIN/MAX, reject in v1.
			if (!conj.children.empty()) {
				auto &inner = *conj.children[0];
				if (inner.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
					auto &cmp = inner.Cast<BoundComparisonExpression>();
					if (cmp.left && AdditiveContainsMinMax(*cmp.left, decide.decide_index) &&
					    IsAddNode(*cmp.left, decide.decide_index)) {
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

	// Additive (+/-) with a MIN/MAX leaf, OR a lone MIN/MAX whose factor has a sign
	// this stage cannot know.
	//
	// The second case matters because the single-term rewrite has only two shapes: an
	// "easy" per-row fan-out (`∀i e_i <= K`) and a "hard" indicator form (`∃i e_i >=
	// K`). Those encode OPPOSITE quantifiers, so choosing between them requires the
	// factor's sign — with the sign unknown there is no safe default, and guessing
	// "hard" silently drops the ∀ half. The composed path emits the envelope pin AND
	// the indicator layer, which together pin the auxiliary to the true MIN/MAX in both
	// directions, so multiplying it by a factor of either sign stays exact.
	ScaledAggregateMatch lone_match;
	const BoundAggregateExpression *lone_scaled =
	    TryMatchScaledAggregate(*comp.left, decide.decide_index, lone_match) ? lone_match.aggregate : nullptr;
	auto lone_scale = lone_match.scale;
	bool lone_divides = lone_match.divides;
	bool unknown_sign_lone_minmax =
	    lone_scaled && lone_scale && ScaleSignAtPlanTime(optimizer.context, lone_scale, lone_divides) == 0 &&
	    (StringUtil::Lower(lone_scaled->function.name) == "min" ||
	     StringUtil::Lower(lone_scaled->function.name) == "max") &&
	    lone_scaled->children.size() == 1 &&
	    BoundExpressionReferencesDecide(*lone_scaled->children[0], decide.decide_index) &&
	    !BoundExpressionReferencesDecide(*lone_scale, decide.decide_index);

	if (!unknown_sign_lone_minmax) {
		if (!IsAddNode(*comp.left, decide.decide_index) && !IsSubNode(*comp.left, decide.decide_index)) {
			return;
		}
		if (!AdditiveContainsMinMax(*comp.left, decide.decide_index)) {
			return;
		}
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
	TryParseSourceClauseTag(comp.GetAlias(), spec.source_clause_id);

	WalkComposedLhs(optimizer.context, *comp.left, /*sign=*/1, decide.decide_index, outer_push_down,
	                spec.terms);

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
		if (IsPerConstraintTag(conj.alias) || HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG)) {
			// If the wrapped objective is composed, reject.
			if (!conj.children.empty()) {
				auto &inner = *conj.children[0];
				if (AdditiveContainsMinMax(inner, decide.decide_index) &&
				    (IsAddNode(inner, decide.decide_index) || IsSubNode(inner, decide.decide_index))) {
					throw BinderException(
					    "Composed MIN/MAX in DECIDE v1 does not support outer WHEN/PER "
					    "wrappers on the objective. Restructure the objective.");
				}
			}
			return;
		}
	}

	// Additive (+/-) with a MIN/MAX leaf, OR a lone MIN/MAX carrying a factor.
	//
	// The second case is here rather than on the flat path because the flat path
	// replaces the whole objective with its auxiliary at coefficient 1.0 -- it has
	// nowhere to put a factor. The composed path already carries one per term, and a
	// one-term composition is a perfectly good degenerate case. An UNSCALED lone
	// MIN/MAX still goes to the flat path, which keeps its cheaper encoding.
	ScaledAggregateMatch lone_match;
	const BoundAggregateExpression *lone_scaled =
	    TryMatchScaledAggregate(obj, decide.decide_index, lone_match) ? lone_match.aggregate : nullptr;
	auto lone_scale = lone_match.scale;
	bool is_scaled_lone_minmax =
	    lone_scaled && lone_scale &&
	    (StringUtil::Lower(lone_scaled->function.name) == "min" ||
	     StringUtil::Lower(lone_scaled->function.name) == "max") &&
	    lone_scaled->children.size() == 1 &&
	    BoundExpressionReferencesDecide(*lone_scaled->children[0], decide.decide_index) &&
	    !BoundExpressionReferencesDecide(*lone_scale, decide.decide_index);

	if (!is_scaled_lone_minmax) {
		if (!IsAddNode(obj, decide.decide_index) && !IsSubNode(obj, decide.decide_index)) {
			return;
		}
		if (!AdditiveContainsMinMax(obj, decide.decide_index)) {
			return;
		}
	}

	// Direction: MAXIMIZE pushes each term UP; MINIMIZE pushes each term DOWN.
	bool outer_push_down = (decide.decide_sense == DecideSense::MINIMIZE);

	vector<LogicalDecide::ComposedMinMaxTerm> terms;
	WalkComposedLhs(optimizer.context, obj, /*sign=*/1, decide.decide_index, outer_push_down, terms);

	decide.composed_minmax_objective_terms = std::move(terms);

	// Replace the objective with a zero placeholder. The physical layer fills in
	// objective coefficients from the spec.
	decide.SetObjective(optimizer.context, make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)));
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
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG)) {
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

	// Unwrap any BoundCastExpression on the LHS, and any factor peeled onto it. The
	// factor STAYS OUTSIDE the aggregate; all it does here is contribute its sign to
	// the easy/hard classification, and ride along on whichever form is emitted.
	Expression *lhs = UnwrapDecideCasts(*comp.left, decide.decide_index);
	ScaledAggregateMatch scale_match;
	const BoundAggregateExpression *scaled_agg =
	    TryMatchScaledAggregate(*lhs, decide.decide_index, scale_match) ? scale_match.aggregate : nullptr;
	auto scale = scale_match.scale;
	bool scale_divides = scale_match.divides;

	if (!scaled_agg && lhs->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	auto &agg = scaled_agg ? *scaled_agg : lhs->Cast<BoundAggregateExpression>();
	auto fname = StringUtil::Lower(agg.function.name);
	if (fname != "min" && fname != "max") {
		return;
	}
	if (agg.children.size() != 1) {
		return;
	}
	// Guard: only rewrite MIN/MAX over decide variables
	if (!BoundExpressionReferencesDecide(*agg.children[0], decide.decide_index)) {
		return;
	}
	// A factor that is itself decision-bearing is bilinear, not a scale; the
	// canonicalizer rejects those, so reaching one here would be a bug rather than a
	// user error. Leave the shape alone rather than mis-linearize it.
	if (scale && BoundExpressionReferencesDecide(*scale, decide.decide_index)) {
		return;
	}

	bool is_max = (fname == "max");
	auto cmp_type = comp.type;

	// A negative factor reverses the relation the aggregate faces: `-2 * MAX(e) >= -8`
	// is `MAX(e) <= 4`, the cheap direction, not the expensive one. Classify against
	// the flipped relation rather than the written one.
	//
	// An unknown sign (a scalar subquery factor) makes the cheap direction
	// undecidable, so take the expensive one -- it pins the auxiliary to the true
	// MIN/MAX in both directions and is therefore right for either sign.
	int scale_sign = ScaleSignAtPlanTime(optimizer.context, scale, scale_divides);

	// Neither of this path's two encodings is safe without the sign. "Easy" fans the
	// bound out over EVERY row; "hard" asserts SOME row attains it. Those are opposite
	// quantifiers, and a negative factor swaps which one the constraint means, so a
	// factor whose value is not known until the query runs cannot pick between them.
	// RewriteComposedMinMax claims that shape first (it runs earlier and emits both
	// halves); reaching here means it declined, so decline too rather than guess.
	if (scale_sign == 0) {
		return;
	}
	auto effective_cmp = (scale_sign < 0) ? FlipComparisonExpression(cmp_type) : cmp_type;

	// Classify: easy vs hard
	bool is_easy = false;
	if (is_max && (effective_cmp == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	               effective_cmp == ExpressionType::COMPARE_LESSTHAN)) {
		is_easy = true; // MAX(expr) <= K → every row: expr <= K
	}
	if (!is_max && (effective_cmp == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	                effective_cmp == ExpressionType::COMPARE_GREATERTHAN)) {
		is_easy = true; // MIN(expr) >= K → every row: expr >= K
	}

	bool is_hard = false;
	if (is_max && (effective_cmp == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	               effective_cmp == ExpressionType::COMPARE_GREATERTHAN)) {
		is_hard = true; // MAX(expr) >= K → need indicator
	}
	if (!is_max && (effective_cmp == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	                effective_cmp == ExpressionType::COMPARE_LESSTHAN)) {
		is_hard = true; // MIN(expr) <= K → need indicator
	}

	if (cmp_type == ExpressionType::COMPARE_NOTEQUAL) {
		throw BinderException("DECIDE does not support <> comparison with MIN/MAX aggregates.");
	}

	// Re-attach the peeled factor to whatever form is emitted below. `scale * e` keeps
	// the factor on the left, matching the one spelling canonicalization produces.
	auto apply_scale = [&](unique_ptr<Expression> inner) -> unique_ptr<Expression> {
		if (!scale) {
			return inner;
		}
		return scale_divides ? optimizer.BindScalarFunction("/", std::move(inner), scale->Copy())
		                     : optimizer.BindScalarFunction("*", scale->Copy(), std::move(inner));
	};

	if (cmp_type == ExpressionType::COMPARE_EQUAL) {
		// Equality: split into easy + hard parts
		// MAX(expr) = K → (expr <= K) AND (MAX(expr) >= K)
		// MIN(expr) = K → (expr >= K) AND (MIN(expr) <= K)

		// Easy part: per-row bound. A negative factor reverses it, for the same reason
		// it reverses the classification above.
		auto easy_cmp_type = is_max ? ExpressionType::COMPARE_LESSTHANOREQUALTO
		                            : ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		if (scale_sign < 0) {
			easy_cmp_type = FlipComparisonExpression(easy_cmp_type);
		}
		unique_ptr<Expression> easy = make_uniq<BoundComparisonExpression>(
		    easy_cmp_type,
		    apply_scale(agg.children[0]->Copy()), comp.right->Copy());
		easy->alias = comp.alias;
		AddDecideTag(easy->alias, MINMAX_EASY_REWRITE_TAG);
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
		if (scale_sign < 0) {
			hard_cmp_type = FlipComparisonExpression(hard_cmp_type);
		}
		idx_t ind_idx;
		auto hard_lhs = EmitHardMinMaxClause(decide, fname, *agg.children[0], agg.filter.get(), ind_idx);
		comp.left = apply_scale(std::move(hard_lhs));
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
		// The factor distributes over the per-row form and the relation keeps the
		// direction the user wrote: `s * MAX(e) <op> K` is `s*e_i <op> K` for every
		// row, for EITHER sign of s. (A negative s flips the relation twice -- once
		// getting from the aggregate to `MAX(e) <op'> K/s`, once dividing by s -- so
		// the two cancel. The classification above is what consumed the sign.)
		comp.left = apply_scale(agg.children[0]->Copy());
		// Tag the comparison so physical_decide.cpp can enforce empty-WHEN
		// rejection on constraints the user wrote as MIN/MAX, even after the
		// optimizer strips the aggregate.
		AddDecideTag(comp.alias, MINMAX_EASY_REWRITE_TAG);
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
		// Hard case: allocate indicator + tagged SUM via shared helper. The factor
		// rides on the outside; the physical extractor multiplies it into the row's
		// coefficients, which is exact whatever its sign because the indicator layer
		// has already pinned the auxiliary to the true MIN/MAX.
		idx_t ind_idx;
		auto hard_lhs = EmitHardMinMaxClause(decide, fname, *agg.children[0], agg.filter.get(), ind_idx);
		comp.left = apply_scale(std::move(hard_lhs));
		return;
	}
}

unique_ptr<Expression> DecideOptimizer::EmitHardMinMaxClause(LogicalDecide &decide,
                                                            const string &agg_name,
                                                            const Expression &inner,
                                                            const Expression *filter,
                                                            idx_t &out_clause_idx) {
	// No indicator variable. `MAX(e) >= K` now becomes an extremum COLUMN and the user's
	// own bound as a single row over it, whichever way that column is then pinned, and the
	// binaries a Big-M pinning needs are global-block columns stage 06 allocates for the
	// rows it actually emits. A row-scoped binary per data row, created here before anyone
	// knows whether it will be read, is exactly what that removed.
	//
	// What stage 05 still owns is the marking: which clause this is, what it reduces with,
	// and the text to call it in a diagnosis. The clause index rides the tag.
	idx_t clause_idx = decide.minmax_clause_labels.size();
	decide.minmax_clause_labels.push_back(StringUtil::Upper(agg_name) + "(" + inner.ToString() + ")");

	// Build a SUM(inner) aggregate tagged as a hard MIN/MAX. The aggregate name is the
	// marking; the clause index rides along so stage 06 can name what it emits.
	vector<unique_ptr<Expression>> sum_children;
	sum_children.push_back(inner.Copy());
	auto new_sum = optimizer.BindAggregateFunction("sum", std::move(sum_children));
	if (filter) {
		new_sum->Cast<BoundAggregateExpression>().filter = filter->Copy();
	}
	new_sum->alias =
	    string(MINMAX_INDICATOR_TAG_PREFIX) + to_string(clause_idx) + "_" + agg_name + "__";
	out_clause_idx = clause_idx;
	return new_sum;
}

void DecideOptimizer::RewriteMinMaxObjective(LogicalDecide &decide) {
	if (!decide.decide_objective) {
		return;
	}
	// Detach, rewrite, reinstall through the boundary. The rewrite itself is
	// unchanged; it just operates on a local tree now, so whatever it produces is
	// re-canonicalized exactly like an optimizer-generated constraint is.
	auto objective = std::move(decide.decide_objective);
	RewriteMinMaxObjectiveTree(decide, objective);
	decide.SetObjective(optimizer.context, std::move(objective));
}

void DecideOptimizer::RewriteMinMaxObjectiveTree(LogicalDecide &decide, unique_ptr<Expression> &objective) {
	// Navigate through PER and WHEN wrappers to find the actual aggregate
	unique_ptr<Expression> *obj_owner = &objective;
	Expression *obj_expr = objective.get();
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
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) && !conj.children.empty()) {
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

	// A factor peeled onto the objective's reducer (`2 * MAX(x*v)`, `MAX(x*v) * 2`).
	// Objectives are not canonicalized, so both spellings arrive as written.
	ScaledAggregateMatch scale_match;
	const BoundAggregateExpression *scaled_obj_agg =
	    TryMatchScaledAggregate(*obj_expr, decide.decide_index, scale_match) ? scale_match.aggregate : nullptr;
	auto obj_scale = scale_match.scale;

	// Now inspect the actual aggregate
	if (!scaled_obj_agg && obj_expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return;
	}
	auto &outer_agg = scaled_obj_agg ? const_cast<BoundAggregateExpression &>(*scaled_obj_agg)
	                                : obj_expr->Cast<BoundAggregateExpression>();
	auto outer_name = StringUtil::Lower(outer_agg.function.name);
	// A decision-bearing factor is bilinear, not a scale; the canonicalizer rejects
	// those, so leave the shape alone rather than mis-linearize it.
	if (obj_scale && BoundExpressionReferencesDecide(*obj_scale, decide.decide_index)) {
		return;
	}

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
			    BoundExpressionReferencesDecide(*inner_agg.children[0], decide.decide_index)) {
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
	    BoundExpressionReferencesDecide(*outer_agg.children[0], decide.decide_index)) {
		throw BinderException(
		    "MINIMIZE/MAXIMIZE %s(...) PER is ambiguous. "
		    "With PER, use a nested aggregate to specify how per-group values are combined: "
		    "e.g., SUM(%s(...)) PER col or MAX(%s(...)) PER col.",
		    StringUtil::Upper(outer_name), StringUtil::Upper(outer_name),
		    StringUtil::Upper(outer_name));
	}

	// Flat non-PER MIN/MAX objective.
	//
	// A FACTOR on it never reaches here: this path replaces the whole objective with
	// its auxiliary at coefficient 1.0, so there is nowhere to put one.
	// RewriteComposedMinMaxObjectiveTop claims the scaled case first (it runs earlier
	// and carries a per-term scale), leaving this path the unscaled shape it was
	// written for and its cheaper encoding.
	if (obj_scale) {
		return;
	}
	if (!has_per && (outer_name == "min" || outer_name == "max") &&
	    outer_agg.children.size() == 1 &&
	    BoundExpressionReferencesDecide(*outer_agg.children[0], decide.decide_index)) {
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
		auto objective = std::move(decide.decide_objective);
		FindAndReplaceAbs(objective, decide, abs_pairs, /*in_objective=*/true);
		decide.SetObjective(optimizer.context, std::move(objective));
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
			// The sign indicator exists to switch the Big-M upper envelope, and the
			// native arm emits no Big-M: it states `aux = |t|` outright. So allocate y
			// only on the arm that has a use for it. Left in on the native arm it was one
			// free binary PER DATA ROW -- row-scoped, so it grew with the relation --
			// referenced by no row and no general constraint. Solvers presolve such a
			// column away, so it never changed an answer; it was still built, stored and
			// marshalled across the solver API for every row of the input.
			//
			// This is readable here only because stage 05 now OWNS the formulation
			// decision: the arm is known at the moment the variable would be allocated.
			idx_t y_idx = DConstants::INVALID_INDEX;
			if (!decide.use_native_constructs.abs) {
				y_idx = decide.decide_variables.size();
				string y_name = "__abs_y_" + to_string(pi) + "__";
				auto y_var = make_uniq<BoundColumnRefExpression>(
				    y_name, LogicalType::BOOLEAN,
				    ColumnBinding(decide.decide_index, y_idx));
				decide.decide_variables.push_back(std::move(y_var));
				decide.num_auxiliary_vars++;
				decide.is_boolean_var.push_back(true);
				if (!decide.variable_scopes.empty()) {
					decide.variable_scopes.push_back(DecideVarScopeInfo::Row());
				}
			}

			// Tag C1 and C2 so the linearizer can find them at finalization. Keyed by the
			// AUXILIARY, which both arms have, rather than by y, which only the lowering
			// arm allocates.
			c1->alias = string(ABS_UB_POS_TAG_PREFIX) + to_string(pair.aux_idx) + "__";
			c2->alias = string(ABS_UB_NEG_TAG_PREFIX) + to_string(pair.aux_idx) + "__";

			decide.abs_maximize_links.push_back({pair.aux_idx, y_idx});
		} else {
			c1->alias = STRUCTURAL_CONSTRAINT_TAG;
			c2->alias = STRUCTURAL_CONSTRAINT_TAG;
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
			if (BoundExpressionReferencesDecide(*func.children[0], decide.decide_index)) {
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
				if (!decide.variable_scopes.empty()) {
					decide.variable_scopes.push_back(DecideVarScopeInfo::Row());
				}
				// F6: record the user's original ABS(inner) for diagnosis naming
				decide.aux_var_expressions.emplace_back(
				    aux_idx, "ABS(" + func.children[0]->ToString() + ")");

				// Read the Big-M marker set by TagAbsConstraintsForBigM. Tag is
				// set on the BoundFunctionExpression alias before the rewrite.
				// Constraint context owns the tag; objective ABS sets needs_bigm
				// independently below in Phase 2 based on sense.
				bool needs_bigm = HasDecideTag(func.alias, ABS_NEEDS_BIGM_TAG);

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
		auto objective = std::move(decide.decide_objective);
		FindAndReplaceBilinear(objective, decide, links);
		decide.SetObjective(optimizer.context, std::move(objective));
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
				if (!decide.variable_scopes.empty()) {
					decide.variable_scopes.push_back(DecideVarScopeInfo::Row()); // row-scoped
				}
				// F6: record the user's original product (b * x) for diagnosis naming
				decide.aux_var_expressions.emplace_back(
				    aux_idx, "(" + func.children[0]->ToString() + " * " + func.children[1]->ToString() + ")");

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
					c1->alias = STRUCTURAL_CONSTRAINT_TAG;
					AppendConstraint(decide, std::move(c1));

					// w <= b2
					auto c2 = make_uniq<BoundComparisonExpression>(
					    ExpressionType::COMPARE_LESSTHANOREQUALTO,
					    make_uniq<BoundColumnRefExpression>(w_ref.alias, w_ref.return_type, w_ref.binding),
					    make_uniq<BoundColumnRefExpression>(b2_ref.alias, b2_ref.return_type, b2_ref.binding));
					c2->alias = STRUCTURAL_CONSTRAINT_TAG;
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
					c3->alias = STRUCTURAL_CONSTRAINT_TAG;
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

					// The McCormick linking constraints (including the upper corner
					// w <= x - L*(1-b), which for L>=0 is the plain structural w <= x)
					// are emitted at execution time in physical_decide.cpp, where the
					// resolved bounds L and U of the other variable are known. Emitting
					// the structural w <= x here would be unconditional and therefore
					// wrong for a negative-domain x (it forces x >= 0 when b=0). We only
					// record the link; execution adds all four corners.
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

// ---------------------------------------------------------------------------
// Bound absorption: a bound, not a row
// ---------------------------------------------------------------------------
//
// `x <= 10` is one fact about a column, so it belongs in that column's box rather
// than in `num_rows` identical model rows. Choosing between the two is a
// formulation decision, which is why it lives here and not in physical execution.
//
// The pass reads a comparison, a decision variable and a foldable literal. It never
// touches data, so it needs types, not rows.

//! One decision variable, resolved, plus the type facts absorption needs about it.
//! The comparison and BETWEEN arms ask exactly the same questions, so they ask once.
struct AbsorptionTarget {
	LogicalDecide *decide = nullptr;
	idx_t var_idx = DConstants::INVALID_INDEX;
	bool is_integer = false;
	bool is_boolean = false;

	//! A bound that merely restates a BOOLEAN's intrinsic [0,1] box is not a loosenable
	//! parameter: the domain is applied to the solver column directly and never
	//! synthesized as a constraint, so such a bound only exists because the user wrote
	//! it redundantly. A genuine pin (`x <= 0`, `x >= 1`, `x = 1`) does tighten the box
	//! and is recorded — erasing those made the elastic model diverge from the query.
	bool ShouldRecord(char sense, double k) const {
		if (!is_boolean) {
			return true;
		}
		if (sense == '<' && k >= 1.0) {
			return false;
		}
		if (sense == '>' && k <= 0.0) {
			return false;
		}
		return true;
	}

	//! Tighten one side of the box and record the bound, in the one order that is always
	//! correct: tighten unconditionally (a BOOLEAN restatement is a harmless no-op against
	//! the intrinsic box), record only when ShouldRecord agrees.
	void Absorb(char sense, double k, bool strict, double typed_k, idx_t source_clause_id) const {
		auto &lower = decide->absorbed_lower_bounds[var_idx];
		auto &upper = decide->absorbed_upper_bounds[var_idx];
		if (sense != '>') {
			upper = std::min(upper, k); // '<' and '='
		}
		if (sense != '<') {
			lower = std::max(lower, k); // '>' and '='
		}
		if (ShouldRecord(sense, k)) {
			decide->user_absorbed_bounds.push_back({var_idx, sense, k, strict, typed_k, source_clause_id});
		}
	}
};

//! Resolve `expr` to a decision variable. Fails for anything that is not a bare
//! decision reference under casts — a multi-variable LHS (`x - 3*z_1 - 5*z_2 = 0`,
//! the IN linking row) is a genuine relation, not a bound.
static bool TryMatchAbsorptionTarget(const Expression &expr, LogicalDecide &decide, AbsorptionTarget &out) {
	auto *unwrapped = UnwrapDecideCasts(const_cast<Expression &>(expr), decide.decide_index);
	if (unwrapped->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &colref = unwrapped->Cast<BoundColumnRefExpression>();
	for (idx_t i = 0; i < decide.decide_variables.size(); i++) {
		auto &decide_var = decide.decide_variables[i]->Cast<BoundColumnRefExpression>();
		if (colref.binding != decide_var.binding) {
			continue;
		}
		auto type_id = decide.decide_variables[i]->return_type.id();
		out.decide = &decide;
		out.var_idx = i;
		out.is_integer = (type_id == LogicalTypeId::INTEGER || type_id == LogicalTypeId::BIGINT);
		out.is_boolean = i < decide.is_boolean_var.size() && decide.is_boolean_var[i];
		return true;
	}
	return false;
}

void DecideOptimizer::AbsorbBoundsInExpression(Expression &expr, LogicalDecide &decide) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		// PER: only the constraint (child 0) carries bounds; the grouping columns do not.
		if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
			AbsorbBoundsInExpression(*conj.children[0], decide);
			break;
		}
		// WHEN: conditional per-row constraints must NOT contribute to a global bound.
		// `x <= 0 WHEN c` does not mean `x <= 0` everywhere.
		if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) && conj.children.size() == 2) {
			break;
		}
		for (auto &child : conj.children) {
			AbsorbBoundsInExpression(*child, decide);
		}
		break;
	}

	case ExpressionClass::BOUND_COMPARISON: {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		idx_t source_clause_id = DConstants::INVALID_INDEX;
		TryParseSourceClauseTag(comp.GetAlias(), source_clause_id);

		AbsorptionTarget target;
		if (!TryMatchAbsorptionTarget(*comp.left, decide, target)) {
			break;
		}

		// A non-finite bound is left for the constraint path, which already reads it the
		// way the solver does: `x <= +inf` is no bound, `x >= +inf` has no solution, and
		// NaN is an error naming the arithmetic. Absorbing it instead would write it into
		// the column box, where min/max against the ±1e30 sentinels keep an upper bound
		// but not a lower one — the same infinity silently vanishing in one direction and
		// reaching the model validator as an internal error in the other.
		double k;
		if (!IsCastWrappedConstant(*comp.right) ||
		    !TryEvaluateFoldableDouble(optimizer.context, *comp.right, k) || !std::isfinite(k)) {
			break;
		}

		// `=` intersects both sides rather than assigning, so `x = 5 AND x = 10` inverts
		// the box and the conflict is caught instead of resolving to whichever was
		// written last. A strict `<` / `>` normalizes into the bound for an integer
		// (`x < 10` is `x <= 9`), carrying the user's literal as `typed_k` so the
		// diagnosis re-quotes `< 10`. A REAL has no such normalization, so it declines
		// and the constraint path rejects it with a message naming the clause.
		bool absorbed = true;
		switch (comp.type) {
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			target.Absorb('<', k, false, 0.0, source_clause_id);
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			target.Absorb('>', k, false, 0.0, source_clause_id);
			break;
		case ExpressionType::COMPARE_EQUAL:
			target.Absorb('=', k, false, 0.0, source_clause_id);
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			absorbed = target.is_integer;
			if (absorbed) {
				target.Absorb('<', k - 1.0, true, k, source_clause_id);
			}
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			absorbed = target.is_integer;
			if (absorbed) {
				target.Absorb('>', k + 1.0, true, k, source_clause_id);
			}
			break;
		default:
			absorbed = false;
			break;
		}

		if (absorbed) {
			// The comparison stays in the tree so EXPLAIN keeps rendering what the user
			// wrote; the tag is what stops it also becoming a model row.
			AddDecideTag(comp.alias, ABSORBED_BOUND_TAG);
		}
		break;
	}

	case ExpressionClass::BOUND_BETWEEN: {
		auto &between = expr.Cast<BoundBetweenExpression>();
		AbsorptionTarget target;
		if (!TryMatchAbsorptionTarget(*between.input, decide, target)) {
			break;
		}

		auto ExtractBound = [&](const Expression &e) -> double {
			double value;
			return IsCastWrappedConstant(e) && TryEvaluateFoldableDouble(optimizer.context, e, value)
			           ? value
			           : std::numeric_limits<double>::quiet_NaN();
		};

		// Each finite side is recorded as its own spec so the infeasible diagnosis
		// loosens BETWEEN uniformly with the other simple bounds. A strict side is
		// integer-normalized like the comparison arm, carrying the user's typed literal
		// so the diagnosis re-quotes `> a` rather than the normalized `>= a+1`.
		double lo = ExtractBound(*between.lower);
		if (!std::isnan(lo)) {
			bool strict = !between.lower_inclusive && target.is_integer;
			target.Absorb('>', strict ? lo + 1.0 : lo, strict, lo, DConstants::INVALID_INDEX);
		}
		double hi = ExtractBound(*between.upper);
		if (!std::isnan(hi)) {
			bool strict = !between.upper_inclusive && target.is_integer;
			target.Absorb('<', strict ? hi - 1.0 : hi, strict, hi, DConstants::INVALID_INDEX);
		}
		break;
	}

	case ExpressionClass::BOUND_CONSTANT:
		// Type declarations and rewrite placeholders (`TRUE`) carry no bound.
		break;

	default:
		break;
	}
}

void DecideOptimizer::AbsorbVariableBounds(LogicalDecide &decide) {
	// Sized here rather than at construction so every auxiliary variable created by the
	// preceding passes is already counted.
	idx_t num_decide_vars = decide.decide_variables.size();
	decide.absorbed_lower_bounds.assign(num_decide_vars, LogicalDecide::ABSORBED_LOWER_UNSET);
	decide.absorbed_upper_bounds.assign(num_decide_vars, 1e30);

	// Seed a BOOLEAN's intrinsic ceiling here rather than leaving every variable at
	// 1e30 and repairing it at model-build time. The box travels to stage 06 as
	// `SolverInput::upper_bounds`, and every Big-M derivation reads it through
	// `DecideRowTermRange`, which treats `>= 1e20` as unbounded: a declared BOOL with no
	// written upper bound therefore looked unbounded to all of them and collapsed to the
	// fallback constant. `SUM(x) <> 2` over four BOOLs took M = 1000000 where the same
	// query spelled `x(INT) ... x <= 1` took 7.
	//
	// The ceiling is a property of the declared type, known here, and it is rigid: the
	// elastic engine resets BOOLEAN columns only within [0,1], so no diagnosis opens it.
	// That also lets the `<>` range collapse see a BOOL's upper side, which it could not
	// before. Absorption only ever narrows (`std::min`), and a user restatement like
	// `x <= 1` is already treated as a harmless no-op against the intrinsic box, so
	// seeding it changes nothing about how a written bound is recorded.
	for (idx_t i = 0; i < num_decide_vars; i++) {
		if (i < decide.is_boolean_var.size() && decide.is_boolean_var[i]) {
			decide.absorbed_upper_bounds[i] = 1.0;
		}
	}

	if (decide.decide_constraints) {
		AbsorbBoundsInExpression(*decide.decide_constraints, decide);
	}
}

void DecideOptimizer::AppendConstraint(LogicalDecide &decide, unique_ptr<Expression> constraint) {
	// The constraint pool belongs to the operator, and so does the invariant that
	// everything in it is canonical. Rewrites keep emitting whatever shape is
	// natural for them (`aux >= inner`, `aux >= 0 - inner`, ...); LogicalDecide
	// puts it into canonical form on the way in.
	decide.AddConstraint(optimizer.context, std::move(constraint));
}

} // namespace duckdb
