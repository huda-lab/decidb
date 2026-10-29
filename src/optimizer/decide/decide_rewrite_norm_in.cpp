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
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

void DecideOptimizer::RewriteNorm(LogicalDecide &decide) {
	vector<unique_ptr<Expression>> links;
	idx_t l0_counter = 0;

	std::function<void(unique_ptr<Expression> &, const string &, DecideEnclosingScope)> rewrite =
	    [&](unique_ptr<Expression> &expr, const string &source_alias, DecideEnclosingScope scope) {
		if (!expr)
			return;
		auto clause_alias = DescendSourceAlias(*expr, source_alias);
		scope = EnterDecideScope(*expr, scope);
		string payload;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE &&
		    ExtractDecideTagPayload(expr->GetAlias(), WRITTEN_NORM_TAG_PREFIX, payload)) {
			// The canonical text has been rendered; from here the clause is formulated, and every layer after this
			// one shows the definition the solver receives rather than the norm it came from. Canonicalization
			// replaced the norm by that definition, so its EXPLAIN account (D27) is recorded here, while the tag
			// still renders the clause as written.
			using Kind = LogicalDecide::RewriteAccount::Kind;
			// A norm of data reads no decision, so it stands on a bound, which the right-hand-side window rewrite
			// made a column before this pass; an L0 norm is desugared only there.
			D_ASSERT(payload == "1" || payload == "2" || payload == "inf");
			AddRewriteAccount(decide, clause_alias,
			                  payload == "1" ? Kind::NORM_SUM_ABS
			                                 : (payload == "2" ? Kind::NORM_SUM_SQUARES : Kind::NORM_MAX_ABS),
			                  RenderForPlan(*expr, decide));
			auto alias = expr->GetAlias();
			RemoveDecideTag(alias, string(WRITTEN_NORM_TAG_PREFIX) + payload + "__");
			expr->SetAlias(std::move(alias));
		}
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE ||
		    !TryParseNormMarker(expr->GetAlias(), payload)) {
			// A marker stands wherever a scalar may stand. Under a comparison, but just
			// as much under `+`, `*` or a cast -- which is how a regularizer is normally
			// written: `MINIMIZE SUM(cost*x) + 0.5 * norm(x - base, 1)`. Descend through
			// every container so no spelling is left holding an unlowered marker, which
			// would read downstream as the plain SUM the marker is built on and silently
			// drop the norm. A marker beneath another aggregate cannot occur: the binder
			// rejects `SUM(norm(...))` before this pass runs.
			ExpressionIterator::EnumerateChildren(
			    *expr, [&](unique_ptr<Expression> &child) { rewrite(child, clause_alias, scope); });
			return;
		}
		auto &aggregate = expr->Cast<BoundAggregateExpression>();
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
		if (payload == "1" || payload == "2" || payload == "inf") {
			throw InternalException("DECIDE norm of order %s reached the optimizer; canonicalization replaces it "
			                        "with its definition",
			                        payload);
		}
		bool auto_m = payload == "0_auto";
		if (!auto_m && payload.rfind("0_", 0) != 0) {
			throw InternalException("DECIDE NORM marker has invalid order payload '%s'", payload);
		}
		double m = 1.0;
		if (!auto_m) {
			try {
				m = std::stod(payload.substr(2));
			} catch (...) {
				throw InternalException("DECIDE NORM marker has invalid L0 bound '%s'", payload);
			}
		}
		string z_name = (auto_m ? "__l0auto_ind_" : "__l0_ind_") + to_string(l0_counter++) + "__";
		auto z = MakeDecisionRef(
		    decide, AddAuxiliaryDecision(decide, z_name, LogicalType::INTEGER, true, DecideVarScopeInfo::Row(),
		                                 "(" + RenderForPlan(*aggregate.children[0], decide) + " <> 0)"));
		AddRewriteAccount(decide, clause_alias, LogicalDecide::RewriteAccount::Kind::L0_INDICATORS,
		                  RenderForPlan(aggregate, decide));
		auto make_mz = [&]() {
			auto bound = make_uniq<BoundConstantExpression>(Value::DOUBLE(m));
			if (auto_m) {
				bound->alias = L0_AUTO_BOUND_TAG; // a placeholder stage 06 fills from the box
			}
			return optimizer.BindScalarFunction("*", std::move(bound), z->Copy());
		};
		auto add_link = [&](unique_ptr<Expression> lhs, unique_ptr<Expression> rhs) {
			auto link = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
			                                                  std::move(lhs), std::move(rhs));
			MarkFormulationConstraint(*link, clause_alias);
			links.push_back(ScopeHelperRow(std::move(link), scope.Condition()));
		};
		add_link(make_mz(), aggregate.children[0]->Copy());
		add_link(make_mz(), optimizer.BindScalarFunction("-", make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)),
		                                               aggregate.children[0]->Copy()));
		add_link(optimizer.BindScalarFunction("abs", aggregate.children[0]->Copy()),
		         optimizer.BindScalarFunction("*", make_uniq<BoundConstantExpression>(Value::DOUBLE(GetDecideL0Tolerance(optimizer.context))), z->Copy()));
		expr = make_aggregate("sum", std::move(z));
	};

	// Lowering swaps a marker for a `SUM` of indicators under the factor the canonicalizer peeled, so each clause is
	// detached, lowered, and reinstalled through the canonicalizing boundary rather than left as it was edited.
	auto objective = std::move(decide.decide_objective);
	rewrite(objective, OBJECTIVE_ROW_TAG, DecideEnclosingScope());
	decide.SetObjective(optimizer.context, std::move(objective));
	auto constraints = std::move(decide.decide_constraints);
	rewrite(constraints, string(), DecideEnclosingScope());
	if (constraints) AppendConstraint(decide, std::move(constraints));
	for (auto &link : links) AppendConstraint(decide, std::move(link));
}

void DecideOptimizer::RewriteInDomain(LogicalDecide &decide) {
	vector<unique_ptr<Expression>> generated;
	idx_t in_counter = 0;
	DecideCanonicalizer canonicalizer(optimizer.context, decide.decide_index, decide.variable_scopes,
	                                  decide.entity_scopes, decide.is_boolean_var);
	// Every row an IN generates keeps its clause's `when`, and is one row per instance of the indicators' scope: an
	// indicator is a column defined by the decision and its domain, so it is one per instance of their join.
	DecideVarScopeInfo scope_of_in;
	auto emit = [&](unique_ptr<Expression> constraint, const Expression *when, const string &source_alias) {
		MarkFormulationConstraint(*constraint, source_alias);
		generated.push_back(ScopeRowPerInstance(decide, ScopeHelperRow(std::move(constraint), when), scope_of_in));
	};
	std::function<void(unique_ptr<Expression> &, DecideEnclosingScope, const string &)> rewrite =
	    [&](unique_ptr<Expression> &expr, DecideEnclosingScope scope, const string &source_alias) {
		if (!expr) return;
		auto clause_alias = DescendSourceAlias(*expr, source_alias);
		scope = EnterDecideScope(*expr, scope);
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			for (idx_t i = 0; i < conj.children.size(); i++) {
				if (IsConstraintChild(conj, i)) rewrite(conj.children[i], scope, clause_alias);
			}
			// A `when c:` goes with the clause it held: the placeholder stands in for both.
			if (IsWhenConstraintWrapper(conj) && conj.children.size() == 2 &&
			    conj.children[0]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
			    conj.children[0]->Cast<BoundConstantExpression>().value.GetValue<bool>())
				expr = MakeTrueExpression(conj.children[0]->GetAlias());
			return;
		}
		auto when = scope.Condition();
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_OPERATOR || expr->type != ExpressionType::COMPARE_IN) return;
		auto &in = expr->Cast<BoundOperatorExpression>();
		if (in.children.size() < 2) throw InternalException("DECIDE IN marker has no domain values");
		auto *target = GetBareDecideColumnRef(*in.children[0], decide.decide_index);
		if (!target) throw InternalException("DECIDE IN marker target is not a decision variable");
		idx_t k = in.children.size() - 1;
		scope_of_in = ScopeOf(decide, canonicalizer, *in.children[0]);
		for (idx_t i = 1; i < in.children.size(); i++)
			scope_of_in = JoinScopes(scope_of_in, ScopeOf(decide, canonicalizer, *in.children[i]));
		if (target->binding.column_index < decide.is_boolean_var.size() && decide.is_boolean_var[target->binding.column_index] && k == 2) {
			double a, b;
			if (TryEvaluateFoldableDouble(optimizer.context, *in.children[1], a, FOLD_CATCH_ALL) &&
			    TryEvaluateFoldableDouble(optimizer.context, *in.children[2], b, FOLD_CATCH_ALL) &&
			    ((a == 0.0 && b == 1.0) || (a == 1.0 && b == 0.0))) {
				emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
				                                   in.children[0]->Copy(), make_uniq<BoundConstantExpression>(Value::INTEGER(0))), when, clause_alias);
				expr = MakeTrueExpression(clause_alias);
				return;
			}
		}
		if (k == 1) {
			emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, in.children[0]->Copy(), in.children[1]->Copy()), when, clause_alias);
			expr = MakeTrueExpression(clause_alias);
			return;
		}
		vector<unique_ptr<Expression>> indicators;
		AddRewriteAccount(decide, clause_alias, LogicalDecide::RewriteAccount::Kind::IN_INDICATORS,
		                  RenderForPlan(in, decide));
		for (idx_t i = 0; i < k; i++) {
			string name = "__in_ind_" + target->GetName() + "_" + to_string(in_counter) + "_" + to_string(i) + "__";
			// A plan names an indicator by the value it stands for.
			string label =
			    "(" + RenderForPlan(*in.children[0], decide) + " = " + RenderForPlan(*in.children[1 + i], decide) + ")";
			auto indicator = AddAuxiliaryDecision(decide, name, LogicalType::INTEGER, true, scope_of_in, label);
			if (scope_of_in.IsKeyed())
				decide.entity_scopes[scope_of_in.entity_scope_idx].scoped_variable_indices.push_back(indicator);
			indicators.push_back(MakeDecisionRef(decide, indicator));
		}
		in_counter++;
		auto cardinality = indicators[0]->Copy();
		for (idx_t i = 1; i < k; i++) cardinality = optimizer.BindScalarFunction("+", std::move(cardinality), indicators[i]->Copy());
		emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(cardinality),
		                                         make_uniq<BoundConstantExpression>(Value::INTEGER(1))), when, clause_alias);
		auto linking = in.children[0]->Copy();
		bool all_constant = true;
		double min_value = 0.0;
		for (idx_t i = 0; i < k; i++) {
			double value;
			if (!TryEvaluateFoldableDouble(optimizer.context, *in.children[i + 1], value, FOLD_CATCH_ALL))
				all_constant = false;
			else min_value = std::min(min_value, value);
			auto negative = optimizer.BindScalarFunction("-", make_uniq<BoundConstantExpression>(Value::INTEGER(0)), in.children[i + 1]->Copy());
			linking = optimizer.BindScalarFunction("+", std::move(linking), optimizer.BindScalarFunction("*", std::move(negative), indicators[i]->Copy()));
		}
		emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(linking),
		                                         make_uniq<BoundConstantExpression>(Value::INTEGER(0))), when, clause_alias);
		if (all_constant && min_value < 0.0) {
			emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO, in.children[0]->Copy(),
			                                         make_uniq<BoundConstantExpression>(Value::DOUBLE(min_value))), when, clause_alias);
		}
		expr = MakeTrueExpression(clause_alias);
	};
	rewrite(decide.decide_constraints, DecideEnclosingScope(), string());
	for (auto &constraint : generated) AppendConstraint(decide, std::move(constraint));
}

} // namespace duckdb
