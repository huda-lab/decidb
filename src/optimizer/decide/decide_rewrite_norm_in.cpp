//===----------------------------------------------------------------------===//
//                         DeciDB
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
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

void DecideOptimizer::RewriteNorm(LogicalDecide &decide) {
	idx_t l0_counter = 0;
	DecideCanonicalizer canonicalizer(optimizer.context, decide.decide_index, decide.bound.variable_scopes,
	                                  decide.bound.keyed_scopes, decide.bound.keys, decide.bound.is_boolean_var,
	                                  decide.bound.computed_values);

	// Whether the body in hand had a marker lowered.
	bool lowered = false;
	// `origin` is the clause the expression stands in, and `when` the rows that clause keeps.
	std::function<void(unique_ptr<Expression> &, const DecideRowOrigin &, const Expression *)> rewrite =
	    [&](unique_ptr<Expression> &expr, const DecideRowOrigin &origin, const Expression *when) {
		    if (!expr)
			    return;
		    auto *definition = expr->GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER
		                           ? &expr->Cast<BoundDecideReducerExpression>()
		                           : nullptr;
		    if (definition && definition->function && definition->norm) {
			    auto &norm = definition->norm;
			    // The canonical text has been rendered; from here the clause is formulated, and every layer after this
			    // one shows the definition the solver receives rather than the norm it came from. Canonicalization
			    // replaced the norm by that definition, so its EXPLAIN account (D27) is recorded here, while the
			    // definition still renders as the norm written.
			    using Kind = LogicalDecide::RewriteAccount::Kind;
			    // A norm of data reads no decision, so it stands on a bound, which the right-hand-side window rewrite
			    // made a column before this pass; an L0 norm is desugared only there.
			    D_ASSERT(!norm.IsCount());
			    AddRewriteAccount(
			        decide, origin.source_clause_id,
			        norm.order == DecideNorm::Order::L1
			            ? Kind::NORM_SUM_ABS
			            : (norm.order == DecideNorm::Order::L2 ? Kind::NORM_SUM_SQUARES : Kind::NORM_MAX_ABS),
			        RenderForPlan(*expr, decide));
			    norm = DecideNorm();
		    }
		    DecideNorm norm;
		    if (!IsDecideNormMarker(*expr, norm)) {
			    // A marker stands wherever a scalar may stand. Under a comparison, but just
			    // as much under `+`, `*` or a cast -- which is how a regularizer is normally
			    // written: `MINIMIZE SUM(cost*x) + 0.5 * norm(x - base, 1)`. Descend through
			    // every container so no spelling is left holding an unlowered marker, which
			    // would read downstream as the plain SUM the marker is built on and silently
			    // drop the norm. A marker beneath another aggregate cannot occur: the binder
			    // rejects `SUM(norm(...))` before this pass runs.
			    ExpressionIterator::EnumerateChildren(
			        *expr, [&](unique_ptr<Expression> &child) { rewrite(child, origin, when); });
			    return;
		    }
		    auto &aggregate = expr->Cast<BoundDecideReducerExpression>();
		    if (aggregate.children.size() != 1) {
			    throw InternalException("DECIDE NORM marker must contain one bound expression");
		    }
		    // The orders left are the L0 counts, with a written M or none.
		    bool auto_m = norm.bound == 0;
		    double m = auto_m ? 1.0 : norm.bound;
		    string z_name =
		        (auto_m ? L0_AUTO_INDICATOR_NAME_PREFIX : L0_INDICATOR_NAME_PREFIX) + to_string(l0_counter++) + "__";
		    // An indicator is a column defined by the counted expression: one per instance of the scopes it reads, its
		    // links one row per instance.
		    auto z_scope = ScopeOf(decide, canonicalizer, *aggregate.children[0]);
		    const idx_t z_idx = AddAuxiliaryDecision(decide, z_name, LogicalType::INTEGER, true, z_scope,
		                                             "(" + RenderForPlan(*aggregate.children[0], decide) + " <> 0)");
		    LogicalDecide::L0IndicatorFunction indicator;
		    indicator.value_var = z_idx;
		    indicator.auto_m = auto_m;
		    indicator.m = m;
		    indicator.label = RenderForPlan(aggregate, decide);
		    auto z = MakeDecisionRef(decide, z_idx);
		    AddRewriteAccount(decide, origin.source_clause_id, LogicalDecide::RewriteAccount::Kind::HELPER_PER_ROW,
		                      RenderForPlan(aggregate, decide));
		    auto make_mz = [&]() {
			    // An automatic bound is a placeholder stage 06 fills from the box.
			    return optimizer.BindScalarFunction("*", make_uniq<BoundConstantExpression>(Value::DOUBLE(m)),
			                                        z->Copy());
		    };
		    auto add_link = [&](DecideHelperRow role, unique_ptr<Expression> lhs, unique_ptr<Expression> rhs) {
			    auto comparison = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
			                                                           std::move(lhs), std::move(rhs));
			    auto link = MakeFormulationRow(std::move(comparison), origin, z_idx, role);
			    link.scope.when = when ? when->Copy() : nullptr;
			    ScopeRowPerInstance(decide, link, z_scope);
			    indicator.links.push_back(std::move(link));
		    };
		    add_link(DecideHelperRow::L0_BOUND, make_mz(), aggregate.children[0]->Copy());
		    add_link(DecideHelperRow::L0_BOUND, make_mz(),
		             optimizer.BindScalarFunction("-", make_uniq<BoundConstantExpression>(Value::DOUBLE(0.0)),
		                                          aggregate.children[0]->Copy()));
		    vector<unique_ptr<Expression>> absolute_children;
		    absolute_children.push_back(aggregate.children[0]->Copy());
		    auto tolerance = make_uniq<BoundConstantExpression>(Value::DOUBLE(GetDecideL0Tolerance(optimizer.context)));
		    add_link(DecideHelperRow::L0_TOLERANCE,
		             DecideRebindOperator(optimizer.context, "@", std::move(absolute_children), "abs"),
		             optimizer.BindScalarFunction("*", std::move(tolerance), z->Copy()));
		    auto count = BindSumOver(optimizer, std::move(z), aggregate.filter.get());
		    count->alias = aggregate.GetAlias();
		    count->Cast<BoundDecideReducerExpression>().CopyFacts(aggregate);
		    count->Cast<BoundDecideReducerExpression>().norm = DecideNorm();
		    expr = std::move(count);
		    lowered = true;
		    decide.l0_indicator_functions.push_back(std::move(indicator));
	    };

	// Lowering swaps a marker for a `SUM` of indicators under the factor the canonicalizer peeled, so each clause is
	// lowered where it sits, and one a marker was lowered in goes back through the canonicalizing boundary.
	vector<BoundDecideClause *> rewritten;
	decide.EnumerateBodies([&](DecideOwnedBody &body) {
		if (body.owner == DecideOwnedBody::Owner::CLAUSE || body.owner == DecideOwnedBody::Owner::OBJECTIVE) {
			lowered = false;
			rewrite(body.expr, body.Origin(), body.rows);
			if (lowered) {
				rewritten.push_back(body.clause.get());
			}
		}
	});
	for (auto clause : rewritten) {
		if (clause == &decide.bound.objective) {
			decide.SetObjective(optimizer.context, std::move(decide.bound.objective));
		} else {
			decide.CanonicalizeClause(optimizer.context, *clause);
		}
	}
}

void DecideOptimizer::RewriteInDomain(LogicalDecide &decide) {
	vector<BoundDecideClause> generated;
	idx_t in_counter = 0;
	DecideCanonicalizer canonicalizer(optimizer.context, decide.decide_index, decide.bound.variable_scopes,
	                                  decide.bound.keyed_scopes, decide.bound.keys, decide.bound.is_boolean_var,
	                                  decide.bound.computed_values);
	auto rewrite = [&](BoundDecideClause &clause) {
		if (clause.body->GetExpressionClass() != ExpressionClass::BOUND_OPERATOR)
			return;
		// The clause as written, which the rows made of it copy from. A TRUE placeholder stands where it was, with its
		// origin: the clause list is the only record of written order, so EXPLAIN keeps the clause in the position
		// it was written. The clause's `when` goes with the clause it held: the placeholder stands in for both.
		const auto written = clause.Copy();
		clause.body = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
		clause.scope.when = nullptr;
		auto &in = written.body->Cast<BoundOperatorExpression>();
		if (in.children.size() < 2)
			throw InternalException("DECIDE IN marker has no domain values");
		auto *target = GetBareDecideColumnRef(*in.children[0], decide.decide_index);
		if (!target)
			throw InternalException("DECIDE IN marker target is not a decision variable");
		idx_t k = in.children.size() - 1;
		// Every row an IN generates keeps its clause's `when` and `per`, and is one row per instance of the
		// indicators' scope: an indicator is a column defined by the decision and its domain, so it is one per
		// instance of their join.
		auto scope_of_in = ScopeOf(decide, canonicalizer, *in.children[0]);
		for (idx_t i = 1; i < in.children.size(); i++)
			scope_of_in = JoinScopes(scope_of_in, ScopeOf(decide, canonicalizer, *in.children[i]));
		auto emit = [&](unique_ptr<Expression> constraint) {
			auto row = MakeFormulationRow(std::move(constraint), written.origin);
			ScopeRowPerClause(optimizer.context, decide, row, written);
			ScopeRowPerInstance(decide, row, scope_of_in);
			generated.push_back(std::move(row));
		};
		if (decide.bound.is_boolean_var[target->binding.column_index] && k == 2) {
			double a, b;
			if (TryEvaluateFoldableDouble(optimizer.context, *in.children[1], a, FOLD_CATCH_ALL) &&
			    TryEvaluateFoldableDouble(optimizer.context, *in.children[2], b, FOLD_CATCH_ALL) &&
			    ((a == 0.0 && b == 1.0) || (a == 1.0 && b == 0.0))) {
				emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
				                                          in.children[0]->Copy(),
				                                          make_uniq<BoundConstantExpression>(Value::INTEGER(0))));
				return;
			}
		}
		if (k == 1) {
			emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, in.children[0]->Copy(),
			                                          in.children[1]->Copy()));
			return;
		}
		vector<unique_ptr<Expression>> indicators;
		AddRewriteAccount(decide, clause.origin.source_clause_id, LogicalDecide::RewriteAccount::Kind::HELPER_PER_VALUE,
		                  RenderForPlan(in, decide));
		for (idx_t i = 0; i < k; i++) {
			string name = string(IN_INDICATOR_NAME_PREFIX) + target->GetName() + "_" + to_string(in_counter) + "_" +
			              to_string(i) + "__";
			// A plan names an indicator by the value it stands for.
			string label =
			    "(" + RenderForPlan(*in.children[0], decide) + " = " + RenderForPlan(*in.children[1 + i], decide) + ")";
			auto indicator = AddAuxiliaryDecision(decide, name, LogicalType::INTEGER, true, scope_of_in, label);
			indicators.push_back(MakeDecisionRef(decide, indicator));
		}
		in_counter++;
		auto cardinality = indicators[0]->Copy();
		for (idx_t i = 1; i < k; i++)
			cardinality = optimizer.BindScalarFunction("+", std::move(cardinality), indicators[i]->Copy());
		emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(cardinality),
		                                          make_uniq<BoundConstantExpression>(Value::INTEGER(1))));
		auto linking = in.children[0]->Copy();
		bool all_constant = true;
		double min_value = 0.0;
		for (idx_t i = 0; i < k; i++) {
			double value;
			if (!TryEvaluateFoldableDouble(optimizer.context, *in.children[i + 1], value, FOLD_CATCH_ALL))
				all_constant = false;
			else
				min_value = std::min(min_value, value);
			auto negative = optimizer.BindScalarFunction("-", make_uniq<BoundConstantExpression>(Value::INTEGER(0)),
			                                             in.children[i + 1]->Copy());
			linking = optimizer.BindScalarFunction(
			    "+", std::move(linking), optimizer.BindScalarFunction("*", std::move(negative), indicators[i]->Copy()));
		}
		emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(linking),
		                                          make_uniq<BoundConstantExpression>(Value::INTEGER(0))));
		if (all_constant && min_value < 0.0) {
			emit(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
			                                          in.children[0]->Copy(),
			                                          make_uniq<BoundConstantExpression>(Value::DOUBLE(min_value))));
		}
	};
	for (auto &clause : decide.bound.clauses)
		rewrite(clause);
	for (auto &row : generated)
		decide.AddClause(optimizer.context, std::move(row));
}

} // namespace duckdb
