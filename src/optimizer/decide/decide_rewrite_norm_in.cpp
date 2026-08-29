//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_rewrite_norm_in.cpp
//
// DECIDE norm / IN-domain rewrites. See decide_optimizer.cpp.
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
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

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
			    wrapper.children[0]->Cast<BoundConstantExpression>().value.GetValue<bool>())
				expr = MakeTrueExpression(wrapper.children[0]->GetAlias());
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
				expr = MakeTrueExpression(local_source);
				return;
			}
		}
		if (k == 1) {
			emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, in.children[0]->Copy(), in.children[1]->Copy()), when, local_source);
			expr = MakeTrueExpression(local_source);
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
		expr = MakeTrueExpression(local_source);
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
bool decide_rewrite::TryEvaluateFoldableDoubleNoThrow(ClientContext &context, const Expression &expr, double &out) {
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
int decide_rewrite::ScaleSignAtPlanTime(ClientContext &context, const Expression *scale, bool divides) {
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

} // namespace duckdb
