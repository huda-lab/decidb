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

#include <algorithm>
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
		FindAndReplaceBilinear(objective, decide, links, OBJECTIVE_ROW_TAG);
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

//! Whether a reducer stands anywhere in `expr`.
static bool ContainsReducer(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](const Expression &child) { found = found || ContainsReducer(child); });
	return found;
}

// A factor of a product is read as an affine expression in the decisions: `sum of coefficient * decision + constant`,
// each coefficient and the constant decision-free (data, possibly a column that varies by row). `b * (x - 1)` is read
// as `b * x - b`, `(2 * b) * x` as `2 * (b * x)`. Reading the factors this way is what lets one product rewrite stand
// for every shape of factor: the product distributes over the terms, each product of two decisions is linearized once,
// and every other term stays linear.
namespace {

//! One term of an affine expression: `coefficient * decision`; a null coefficient is 1.
struct AffineTerm {
	idx_t var;
	unique_ptr<Expression> coefficient;
};

struct Affine {
	vector<AffineTerm> terms;
	//! The decision-free part, or null when there is none.
	unique_ptr<Expression> constant;
};

//! `a * b` of two coefficients; a null coefficient is 1.
unique_ptr<Expression> MultiplyCoefficients(Optimizer &optimizer, unique_ptr<Expression> a, unique_ptr<Expression> b) {
	if (!a || !b) {
		return a ? std::move(a) : std::move(b);
	}
	return optimizer.BindScalarFunction("*", std::move(a), std::move(b));
}

//! `a + b` of two coefficients; a null coefficient is 1.
unique_ptr<Expression> AddCoefficients(Optimizer &optimizer, unique_ptr<Expression> a, unique_ptr<Expression> b) {
	if (!a) {
		a = make_uniq<BoundConstantExpression>(Value::INTEGER(1));
	}
	if (!b) {
		b = make_uniq<BoundConstantExpression>(Value::INTEGER(1));
	}
	return optimizer.BindScalarFunction("+", std::move(a), std::move(b));
}

unique_ptr<Expression> NegateCoefficient(Optimizer &optimizer, unique_ptr<Expression> a) {
	return a ? optimizer.BindScalarFunction("-", std::move(a)) : make_uniq<BoundConstantExpression>(Value::INTEGER(-1));
}

//! `into += other`. A decision may stand in several terms: the product merges them by pair.
void AddAffine(Optimizer &optimizer, Affine &into, Affine other) {
	for (auto &term : other.terms) {
		into.terms.push_back(std::move(term));
	}
	if (other.constant) {
		into.constant = into.constant ? optimizer.BindScalarFunction("+", std::move(into.constant),
		                                                             std::move(other.constant))
		                              : std::move(other.constant);
	}
}

//! `affine * factor` for a decision-free `factor` (null: 1), or `affine / factor` with `divides`.
void ScaleAffine(Optimizer &optimizer, Affine &affine, const Expression &factor, bool divides) {
	auto scale = [&]() -> unique_ptr<Expression> {
		if (!divides) {
			return factor.Copy();
		}
		return optimizer.BindScalarFunction("/", make_uniq<BoundConstantExpression>(Value::DOUBLE(1.0)),
		                                    factor.Copy());
	};
	for (auto &term : affine.terms) {
		term.coefficient = MultiplyCoefficients(optimizer, scale(), std::move(term.coefficient));
	}
	if (affine.constant) {
		affine.constant = MultiplyCoefficients(optimizer, scale(), std::move(affine.constant));
	}
}

//! Read `expr` as an affine expression in the decisions; false when it is not one (a square, a product of two
//! decision-bearing factors, a decision in a divisor).
bool ReadAffine(Optimizer &optimizer, const Expression &expr, idx_t decide_index, Affine &out) {
	vector<idx_t> vars;
	CollectDecideVarIndices(expr, decide_index, vars);
	if (vars.empty()) {
		out.constant = expr.Copy();
		return true;
	}
	if (auto *colref = GetBareDecideColumnRef(expr, decide_index)) {
		out.terms.push_back({colref->binding.column_index, nullptr});
		return true;
	}
	auto *cur = UnwrapDecideCasts(expr, decide_index);
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = cur->Cast<BoundFunctionExpression>();
	const auto name = StringUtil::Lower(func.function.name);
	auto read_child = [&](idx_t child, Affine &affine) {
		return ReadAffine(optimizer, *func.children[child], decide_index, affine);
	};
	auto bears_decision = [&](idx_t child) {
		vector<idx_t> child_vars;
		CollectDecideVarIndices(*func.children[child], decide_index, child_vars);
		return !child_vars.empty();
	};
	if ((name == "+" || name == "-") && func.children.size() == 2) {
		Affine left, right;
		if (!read_child(0, left) || !read_child(1, right)) {
			return false;
		}
		if (name == "-") {
			ScaleAffine(optimizer, right, BoundConstantExpression(Value::INTEGER(-1)), false);
		}
		AddAffine(optimizer, out, std::move(left));
		AddAffine(optimizer, out, std::move(right));
		return true;
	}
	if (name == "-" && func.children.size() == 1) {
		if (!read_child(0, out)) {
			return false;
		}
		ScaleAffine(optimizer, out, BoundConstantExpression(Value::INTEGER(-1)), false);
		return true;
	}
	if (name == "*" && func.children.size() == 2 && bears_decision(0) != bears_decision(1)) {
		const idx_t decision_side = bears_decision(0) ? 0 : 1;
		if (!read_child(decision_side, out)) {
			return false;
		}
		ScaleAffine(optimizer, out, *func.children[1 - decision_side], false);
		return true;
	}
	if (name == "/" && func.children.size() == 2 && bears_decision(0) && !bears_decision(1)) {
		if (!read_child(0, out)) {
			return false;
		}
		ScaleAffine(optimizer, out, *func.children[1], true);
		return true;
	}
	return false;
}

//! The auxiliary `w = first * second` for two decisions, one of them Boolean (`first` and `second` are in the order
//! the user wrote them): AND for two Booleans, else the McCormick link whose rows stage 06 emits against the other
//! factor's box. Returns the auxiliary's decision index.
idx_t AddBooleanProduct(Optimizer &optimizer, LogicalDecide &decide, vector<LogicalDecide::BilinearLink> &links,
                        const string &clause_alias, idx_t first, idx_t second) {
	auto is_bool = [&](idx_t var) { return var < decide.is_boolean_var.size() && decide.is_boolean_var[var]; };
	const bool both_bool = is_bool(first) && is_bool(second);
	const idx_t bool_var_idx = is_bool(first) ? first : second;
	const idx_t other_var_idx = bool_var_idx == first ? second : first;

	// The auxiliary, named by the column it will take.
	string aux_name = "__bilinear_aux_" + to_string(decide.decide_variables.size()) + "__";
	// Bool×Bool auxiliary is semantically boolean but uses INTEGER type to match
	// how user BOOLEAN variables are represented (INTEGER with 0/1 bounds).
	// Using BOOLEAN would cause type-mismatch errors when binding arithmetic.
	//
	// Bool×Integer: the product b * y with b ∈ {0,1} and y ∈ ℤ always takes
	// integer values, so declare the aux as INTEGER rather than DOUBLE. This
	// preserves integer-valuedness of the LHS through McCormick linearization,
	// which matters for the strict-inequality rewrite (`< K → <= K-1`) in
	// ilp_model_builder.cpp.
	auto &other_type = decide.decide_variables[other_var_idx]->return_type;
	const bool other_is_integer_typed = !(other_type == LogicalType::DOUBLE || other_type == LogicalType::FLOAT);
	LogicalType aux_type = (both_bool || other_is_integer_typed) ? LogicalType::INTEGER : LogicalType::DOUBLE;
	// F6: a diagnosis, and a plan, name the auxiliary by the user's product (b * x).
	string label = "(" + RenderForPlan(*decide.decide_variables[first], decide) + " * " +
	               RenderForPlan(*decide.decide_variables[second], decide) + ")";
	idx_t aux_idx = AddAuxiliaryDecision(decide, aux_name, aux_type, both_bool, DecideVarScopeInfo::Row(), label);
	AddRewriteAccount(decide, clause_alias,
	                  both_bool ? LogicalDecide::RewriteAccount::Kind::PRODUCT_AND
	                            : LogicalDecide::RewriteAccount::Kind::PRODUCT_MCCORMICK,
	                  label);

	if (both_bool) {
		// AND-linearization: w <= b1, w <= b2, w >= b1 + b2 - 1
		auto &b1_ref = decide.decide_variables[bool_var_idx]->Cast<BoundColumnRefExpression>();
		auto &b2_ref = decide.decide_variables[other_var_idx]->Cast<BoundColumnRefExpression>();
		auto &w_ref = decide.decide_variables[aux_idx]->Cast<BoundColumnRefExpression>();
		auto ref = [](const BoundColumnRefExpression &source) {
			return make_uniq<BoundColumnRefExpression>(source.alias, source.return_type, source.binding);
		};

		// w <= b1
		auto c1 = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, ref(w_ref),
		                                               ref(b1_ref));
		MarkFormulationConstraint(*c1, clause_alias);
		decide.AddConstraint(optimizer.context, std::move(c1));

		// w <= b2
		auto c2 = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, ref(w_ref),
		                                               ref(b2_ref));
		MarkFormulationConstraint(*c2, clause_alias);
		decide.AddConstraint(optimizer.context, std::move(c2));

		// w >= b1 + b2 - 1  (i.e., b1 + b2 - w <= 1)
		auto b1_plus_b2 = optimizer.BindScalarFunction("+", ref(b1_ref), ref(b2_ref));
		auto b1_plus_b2_minus_w = optimizer.BindScalarFunction("-", std::move(b1_plus_b2), ref(w_ref));
		auto c3 = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                                               std::move(b1_plus_b2_minus_w),
		                                               make_uniq<BoundConstantExpression>(Value::INTEGER(1)));
		MarkFormulationConstraint(*c3, clause_alias);
		decide.AddConstraint(optimizer.context, std::move(c3));
	} else {
		// Bool × Non-Bool McCormick: w <= x (structural, at plan time)
		// w <= U*b and w >= x - U*(1-b) generated at execution time via BilinearLink
		//
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
	return aux_idx;
}

} // namespace

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

			// A reducer under a factor (`b * SUM(x)`) is not a product of two variables: it is the value
			// rewrite's, which holds the reducer in a helper decision and hands the product back here.
			if (!left_vars.empty() && !right_vars.empty() && !ContainsReducer(*func.children[0]) &&
			    !ContainsReducer(*func.children[1]) && func.children[0]->ToString() != func.children[1]->ToString()) {
				// (Identical factors are a square: the QP pipeline's.) Each factor is an affine expression in the
				// decisions, and the product distributes over its terms.
				Affine left, right;
				auto is_bool = [&](idx_t var) {
					return var < decide.is_boolean_var.size() && decide.is_boolean_var[var];
				};
				auto has_bool = [&](const Affine &affine) {
					return std::any_of(affine.terms.begin(), affine.terms.end(),
					                   [&](const AffineTerm &term) { return is_bool(term.var); });
				};
				if (ReadAffine(optimizer, *func.children[0], decide.decide_index, left) &&
				    ReadAffine(optimizer, *func.children[1], decide.decide_index, right) &&
				    (has_bool(left) || has_bool(right))) {
					// The terms of the product: `coefficient * first * second` for a pair of decisions (`second` is
					// INVALID for one decision, both INVALID for the constant), merged by the pair.
					struct ProductTerm {
						idx_t first;
						idx_t second;
						unique_ptr<Expression> coefficient;
					};
					vector<ProductTerm> product;
					auto add = [&](idx_t first, idx_t second, unique_ptr<Expression> coefficient) {
						// A Boolean squared is itself.
						if (first == second && first != DConstants::INVALID_INDEX && is_bool(first)) {
							second = DConstants::INVALID_INDEX;
						}
						auto same = std::find_if(product.begin(), product.end(), [&](const ProductTerm &term) {
							const bool swapped = term.first == second && term.second == first &&
							                     second != DConstants::INVALID_INDEX;
							return (term.first == first && term.second == second) || swapped;
						});
						if (same == product.end()) {
							product.push_back({first, second, std::move(coefficient)});
						} else {
							same->coefficient =
							    AddCoefficients(optimizer, std::move(same->coefficient), std::move(coefficient));
						}
					};
					auto copy = [](const unique_ptr<Expression> &coefficient) {
						return coefficient ? coefficient->Copy() : nullptr;
					};
					for (auto &a : left.terms) {
						for (auto &b : right.terms) {
							add(a.var, b.var,
							    MultiplyCoefficients(optimizer, copy(a.coefficient), copy(b.coefficient)));
						}
						if (right.constant) {
							add(a.var, DConstants::INVALID_INDEX,
							    MultiplyCoefficients(optimizer, copy(a.coefficient), right.constant->Copy()));
						}
					}
					if (left.constant) {
						for (auto &b : right.terms) {
							add(b.var, DConstants::INVALID_INDEX,
							    MultiplyCoefficients(optimizer, left.constant->Copy(), copy(b.coefficient)));
						}
						if (right.constant) {
							add(DConstants::INVALID_INDEX, DConstants::INVALID_INDEX,
							    MultiplyCoefficients(optimizer, left.constant->Copy(), right.constant->Copy()));
						}
					}

					auto column = [&](idx_t var) -> unique_ptr<Expression> {
						auto &ref = decide.decide_variables[var]->Cast<BoundColumnRefExpression>();
						return make_uniq<BoundColumnRefExpression>(ref.alias, ref.return_type, ref.binding);
					};
					unique_ptr<Expression> sum;
					for (auto &term : product) {
						unique_ptr<Expression> factor;
						if (term.first == DConstants::INVALID_INDEX) {
							factor = nullptr;
						} else if (term.second == DConstants::INVALID_INDEX) {
							factor = column(term.first);
						} else if (is_bool(term.first) || is_bool(term.second)) {
							factor = column(AddBooleanProduct(optimizer, decide, links, clause_alias, term.first,
							                                  term.second));
						} else {
							// Two decisions, neither Boolean: the Q matrix's.
							factor = optimizer.BindScalarFunction("*", column(term.first), column(term.second));
						}
						auto value = MultiplyCoefficients(optimizer, std::move(term.coefficient), std::move(factor));
						sum = sum ? optimizer.BindScalarFunction("+", std::move(sum), std::move(value))
						          : std::move(value);
					}
					expr = std::move(sum);
					return;
				}
			}
		}
	}

	// Recurse into children
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		FindAndReplaceBilinear(child, decide, links, clause_alias);
	});
}

} // namespace duckdb
