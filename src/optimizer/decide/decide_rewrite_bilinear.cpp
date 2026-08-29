//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/optimizer/decide/decide_rewrite_bilinear.cpp
//
// DECIDE bilinear McCormick linearization. See decide_optimizer.cpp.
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

// ---------------------------------------------------------------------------
// Bilinear McCormick linearization (Boolean × anything)
// ---------------------------------------------------------------------------

void DecideOptimizer::RewriteBilinear(LogicalDecide &decide) {
	vector<LogicalDecide::BilinearLink> links;
	if (decide.decide_objective) {
		auto objective = std::move(decide.decide_objective);
		FindAndReplaceBilinear(objective, decide, links, string());
		decide.SetObjective(optimizer.context, std::move(objective));
	}
	if (decide.decide_constraints) {
		FindAndReplaceBilinear(decide.decide_constraints, decide, links, string());
	}
	decide.bilinear_links = std::move(links);
}

//! Identify whether a bound expression is a single DECIDE variable reference
//! and return its index; INVALID_INDEX if it is not one. The casts DuckDB inserts
//! when operand types differ (e.g. INTEGER * DOUBLE) are looked through by the cast
//! policy, which owns that decision -- this pass does not make it again.
static idx_t GetSingleDecideVarIdx(const Expression &expr, idx_t decide_index) {
	auto *colref = GetBareDecideColumnRef(expr, decide_index);
	return colref ? colref->binding.column_index : DConstants::INVALID_INDEX;
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
                                              vector<LogicalDecide::BilinearLink> &links,
                                              const string &source_alias) {
	if (!expr) return;
	auto clause_alias = DescendSourceAlias(*expr, source_alias);

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
						FindAndReplaceBilinear(child, decide, links, clause_alias);
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
					MarkFormulationConstraint(*c1, clause_alias);
					AppendConstraint(decide, std::move(c1));

					// w <= b2
					auto c2 = make_uniq<BoundComparisonExpression>(
					    ExpressionType::COMPARE_LESSTHANOREQUALTO,
					    make_uniq<BoundColumnRefExpression>(w_ref.alias, w_ref.return_type, w_ref.binding),
					    make_uniq<BoundColumnRefExpression>(b2_ref.alias, b2_ref.return_type, b2_ref.binding));
					MarkFormulationConstraint(*c2, clause_alias);
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
					MarkFormulationConstraint(*c3, clause_alias);
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
					TryParseSourceClauseTag(clause_alias, link.source_clause_id);
					TryParseRemovalGroupTag(clause_alias, link.removal_group_id);
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
		FindAndReplaceBilinear(child, decide, links, clause_alias);
	});
}

} // namespace duckdb
