//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/optimizer/decide/decide_rewrite_bilinear.cpp
//
// DECIDE bilinear McCormick linearization. See decide_optimizer.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/optimizer/decide/decide_optimizer.hpp"

#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_product_form.hpp"

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
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/optimizer/decide/decide_optimizer_internal.hpp"

namespace duckdb {

using namespace decide_rewrite; // NOLINT: internal DECIDE rewrite helpers

// ---------------------------------------------------------------------------
// Bilinear McCormick linearization (Boolean × anything)
// ---------------------------------------------------------------------------

bool decide_rewrite::BilinearReadsBody(const LogicalDecide &decide, const DecideOwnedBody &body) {
	return body.owner != DecideOwnedBody::Owner::HELPER ||
	       decide.helper_definitions[body.index]->frame == DConstants::INVALID_INDEX;
}

void DecideOptimizer::RewriteBilinear(LogicalDecide &decide) {
	// The one pass over the bodies as they stand when the McCormick rows are decided. A product that appears later is
	// tagged where it appears, through FindAndReplaceBilinear: the value rewrite hands over the products its helpers
	// expose (RewriteScopedReducers), and RewriteAbs those that read an ABS helper (AbsProductWorklist).
	//
	// The bodies that stand outside the written clauses have the same bilinear semantics as any other row: L0's three
	// structural links, which the indicator F node owns and lowers later; definition bodies, which survive outside the
	// clause list; and a retained reducer's body.
	//
	// The tags this pass adds are read by no canonicalization, so the objective is not reinstalled.
	decide.EnumerateBodies([&](DecideOwnedBody &body) {
		if (BilinearReadsBody(decide, body)) {
			FindAndReplaceBilinear(body.expr, decide, body.Origin());
		}
	});
}

// A factor of a product is read as an affine expression in the decisions: `sum of coefficient * decision + constant`,
// each coefficient and the constant decision-free (data, possibly a column that varies by row). `b * (x - 1)` is read
// as `b * x - b`, `(2 * b) * x` as `2 * (b * x)`. Reading the factors this way is what lets one product rewrite stand
// for every shape of factor: the product distributes over the terms, each product of two decisions is linearized once,
// and every other term stays linear. The terms are the term splitter's (DecideTermSplitter::Split), the one reader of
// what a decision expression is made of.
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

//! The terms of one side of a product, as an affine expression. False for an ABS over decisions that is not tagged yet:
//! the one side of a degree-2 product that is not affine, which the pass after RewriteAbs reads. Anything else the
//! splitter does not read as a constant or a linear term is a broken promise of the stages before: the shape check
//! refuses a degree above two, the binder refuses a decision in a divisor, and a function the optimizer gave a helper
//! decision (a definition, an ABS once RewriteAbs tagged it) reads as that decision. `side` is the expression the terms
//! were read from.
bool AffineFromTerms(Optimizer &optimizer, vector<DecideSplitTerm> &terms, const Expression &side, Affine &out) {
	for (auto &term : terms) {
		if (term.IsAbs()) {
			return false;
		}
		if (!term.IsConstant() && !term.IsLinear()) {
			throw InternalException("DECIDE bilinear factor '%s' is not affine in the decisions", side.ToString());
		}
	}
	for (auto &term : terms) {
		// A bare decision's coefficient is the literal 1, which stays out of the products.
		auto coefficient = std::move(term.coefficient);
		if (coefficient && coefficient->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
		    coefficient->Cast<BoundConstantExpression>().value == Value::INTEGER(1) && term.IsLinear()) {
			coefficient = nullptr;
		}
		if (term.sign < 0) {
			coefficient = MultiplyCoefficients(optimizer, make_uniq<BoundConstantExpression>(Value::INTEGER(-1)),
			                                   std::move(coefficient));
		}
		if (term.IsLinear()) {
			out.terms.push_back({term.var_a, std::move(coefficient)});
		} else {
			out.constant = out.constant
			                   ? optimizer.BindScalarFunction("+", std::move(out.constant), std::move(coefficient))
			                   : std::move(coefficient);
		}
	}
	return true;
}

//! The auxiliary `w = first * second` for two decisions, one of them Boolean (`first` and `second` are in the order
//! the user wrote them): AND for two Booleans, else the McCormick link whose rows stage 06 emits against the other
//! factor's box. Returns the auxiliary's decision index.
idx_t AddBooleanProduct(LogicalDecide &decide, const DecideRowOrigin &origin, idx_t first, idx_t second) {
	auto is_bool = [&](idx_t var) {
		return decide.bound.is_boolean_var[var];
	};
	const bool both_bool = is_bool(first) && is_bool(second);
	const idx_t bool_var_idx = is_bool(first) ? first : second;
	const idx_t other_var_idx = bool_var_idx == first ? second : first;

	// The auxiliary, named by the column it will take.
	string aux_name = BILINEAR_AUX_NAME_PREFIX + to_string(decide.bound.variables.size()) + "__";
	// Bool×Bool auxiliary is semantically boolean but uses INTEGER type to match
	// how user BOOLEAN variables are represented (INTEGER with 0/1 bounds).
	// Using BOOLEAN would cause type-mismatch errors when binding arithmetic.
	//
	// Bool×Integer: the product b * y with b ∈ {0,1} and y ∈ ℤ always takes
	// integer values, so declare the aux as INTEGER rather than DOUBLE. This
	// preserves integer-valuedness of the LHS through McCormick linearization,
	// which matters for the strict-inequality rewrite (`< K → <= K-1`) in
	// ilp_model_builder.cpp.
	auto &other_type = decide.bound.variables[other_var_idx]->return_type;
	const bool other_is_integer_typed = other_type != LogicalType::DOUBLE;
	LogicalType aux_type = (both_bool || other_is_integer_typed) ? LogicalType::INTEGER : LogicalType::DOUBLE;
	// F6: a diagnosis, and a plan, name the auxiliary by the user's product (b * x).
	string label = "(" + RenderForPlan(*decide.bound.variables[first], decide) + " * " +
	               RenderForPlan(*decide.bound.variables[second], decide) + ")";
	// The auxiliary has one instance per instance of the finer of its factors (a query-wide factor has one for all): a
	// query-wide product is one column and a keyed one a column per key, not a copy per row.
	auto scope = JoinScopes(decide.bound.variable_scopes[first], decide.bound.variable_scopes[second]);
	idx_t aux_idx = AddAuxiliaryDecision(decide, aux_name, aux_type, both_bool, scope, label);
	// A product takes its factor's sign (`b * x` is x where b = 1, -5 for x >= -5): no default floor of 0. Its
	// column is floored at the product's range, min(0, the factor's floor) (LinearizeBilinear). An AND of two
	// Booleans keeps its BOOL box.
	decide.signed_helper_vars.push_back(aux_idx);
	AddRewriteAccount(decide, origin.source_clause_id, LogicalDecide::RewriteAccount::Kind::HELPER, label);

	// Both kinds are linked per instance of the auxiliary by LinearizeBilinear, which reads the factors' boxes
	// there: the McCormick rows against the other factor's box (the structural `w <= x` is not emitted here, being
	// wrong for a negative-domain x: it would force x >= 0 when b = 0), and for two Booleans the AND (`w <= b1`,
	// `w <= b2`, `w >= b1 + b2 - 1`).
	auto product = make_uniq<DecideHelperDefinition>(DecideHelperKind::BOOLEAN_PRODUCT);
	product->helper = aux_idx;
	product->first_factor = first;
	product->second_factor = second;
	product->origin = origin;
	decide.helper_definitions.push_back(std::move(product));
	return aux_idx;
}

} // namespace

void DecideOptimizer::FindAndReplaceBilinear(unique_ptr<Expression> &expr, LogicalDecide &decide,
                                             const DecideRowOrigin &origin) {
	if (!expr) {
		return;
	}
	idx_t definition_aux;
	if (TryParseDecideIndexTag(expr->GetAlias(), DEFINITION_FUNCTION_TAG_PREFIX, definition_aux)) {
		return; // its source body is compiled from its definition, exactly once
	}
	idx_t existing_group;
	if (TryParseDecideIndexTag(expr->GetAlias(), BILINEAR_FUNCTION_GROUP_TAG_PREFIX, existing_group)) {
		if (existing_group >= decide.helper_definitions.size()) {
			throw InternalException("DECIDE bilinear function group %llu is missing", existing_group);
		}
		return;
	}

	if (IsDecideProductNode(optimizer.context, *expr, decide.decide_index)) {
		// A product of degree 2 in the canonical spelling: `coefficient * l * r`, or `coefficient * POWER(l, 2)`,
		// whose two sides are one linear form.
		auto form = ReadDecideProduct(optimizer.context, *expr, decide.decide_index);
		vector<const Expression *> sides;
		for (auto &factor : form.factors) {
			for (idx_t i = 0; i < factor.power; i++) {
				sides.push_back(factor.base);
			}
		}
		// A reducer in a factor (`b * SUM(x)`, `POWER(SUM(x) - 3, 2)`) is not a product of two variables: it is the
		// value rewrite's, which holds the reducer in a helper decision and hands the product back here.
		if (sides.size() == 2 && !DecideCanonicalizer::ContainsReducer(*sides[0]) &&
		    !DecideCanonicalizer::ContainsReducer(*sides[1])) {
			// Each side is an affine expression in the decisions, and the product distributes over its terms.
			Affine left, right;
			auto is_bool = [&](idx_t var) {
				return decide.bound.is_boolean_var[var];
			};
			auto has_bool = [&](const Affine &affine) {
				return std::any_of(affine.terms.begin(), affine.terms.end(),
				                   [&](const AffineTerm &term) { return is_bool(term.var); });
			};
			DecideTermSplitter splitter(optimizer.context, decide);
			vector<DecideSplitTerm> left_terms, right_terms;
			splitter.Split(*sides[0], 1, left_terms);
			splitter.Split(*sides[1], 1, right_terms);
			if (AffineFromTerms(optimizer, left_terms, *sides[0], left) &&
			    AffineFromTerms(optimizer, right_terms, *sides[1], right) && (has_bool(left) || has_bool(right))) {
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
						const bool swapped = term.first == second && term.second == first;
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
						add(a.var, b.var, MultiplyCoefficients(optimizer, copy(a.coefficient), copy(b.coefficient)));
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
					auto &ref = decide.bound.variables[var]->Cast<BoundColumnRefExpression>();
					return make_uniq<BoundColumnRefExpression>(ref.alias, ref.return_type, ref.binding);
				};
				// The product's coefficient, its data factors and sign, scales every term.
				unique_ptr<Expression> scale =
				    form.sign < 0 ? make_uniq<BoundConstantExpression>(Value::INTEGER(-1)) : nullptr;
				for (auto &factor : form.coefficient) {
					// A data factor has power 1: the canonical spelling holds `POWER(w, 2)` as one base, and a
					// power of a form over a decision with data in it is a degree the shape check refuses.
					if (factor.power != 1) {
						throw InternalException("DECIDE: a data factor of a product has power %llu; ReadDecideProduct "
						                        "promises power 1 for data",
						                        static_cast<uint64_t>(factor.power));
					}
					scale = MultiplyCoefficients(optimizer, std::move(scale), factor.base->Copy());
				}
				unique_ptr<Expression> sum;
				for (auto &term : product) {
					unique_ptr<Expression> factor;
					if (term.first == DConstants::INVALID_INDEX) {
						factor = nullptr;
					} else if (term.second == DConstants::INVALID_INDEX) {
						factor = column(term.first);
					} else if (is_bool(term.first) || is_bool(term.second)) {
						factor = column(AddBooleanProduct(decide, origin, term.first, term.second));
					} else {
						// Two decisions, neither Boolean: the Q matrix's.
						factor = optimizer.BindScalarFunction("*", column(term.first), column(term.second));
					}
					auto value = MultiplyCoefficients(
					    optimizer, MultiplyCoefficients(optimizer, copy(scale), std::move(term.coefficient)),
					    std::move(factor));
					sum = sum ? optimizer.BindScalarFunction("+", std::move(sum), std::move(value)) : std::move(value);
				}
				// Keep the product the user wrote. The copied expansion is the term splitter's recipe; every
				// Boolean cross term in it names a helper that tree lowering turns into a BOOLEAN_PRODUCT F read.
				// If there is no Boolean cross term, the expansion still exposes the distributed quadratic terms.
				auto kept = make_uniq<DecideHelperDefinition>(DecideHelperKind::PRODUCT);
				kept->body = std::move(sum);
				auto alias = expr->GetAlias();
				AddDecideTag(alias, MakeBilinearFunctionGroupTag(decide.helper_definitions.size()));
				expr->SetAlias(std::move(alias));
				decide.helper_definitions.push_back(std::move(kept));
				return;
			}
		}
	}

	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { FindAndReplaceBilinear(child, decide, origin); });
}

} // namespace duckdb
