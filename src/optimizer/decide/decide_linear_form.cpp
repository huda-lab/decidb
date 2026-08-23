#include "duckdb/optimizer/decide_linear_form.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/decidb/decide_cast_policy.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_binder/decide_degree.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_decide.hpp"

#include <functional>
#include <unordered_map>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Rebuilding a coefficient
//===--------------------------------------------------------------------===//

//! Re-resolve a binary operator against the children it is actually being given.
//!
//! Rebuilding a `BoundFunctionExpression` by hand — reusing another node's
//! `function` / `return_type` / `bind_info` over different children — does not
//! fail when the types disagree. It reinterprets the children's *physical*
//! representation, which silently yields a wrong number and can read past the
//! end of a narrower vector. DECIDE has now hit that failure mode three times
//! (see `06_issues/bugs/done.md`), always where a subtree was rebuilt after
//! terms were dropped or distributed. Binding through `FunctionBinder` is the
//! only rebuild that stays correct for arbitrary children: it picks the
//! implementation for these argument types, computes the matching return type
//! and bind data, and inserts whatever casts the signature needs.
//!
//! This is the reason the whole pass belongs here rather than at execution time:
//! a binder is in scope, so nothing has to be reconstructed by hand.
static unique_ptr<Expression> RebindOperator(ClientContext &context, const string &name,
                                             vector<unique_ptr<Expression>> children) {
	FunctionBinder function_binder(context);
	ErrorData error;
	auto result = function_binder.BindScalarFunction(DEFAULT_SCHEMA, name, std::move(children), error);
	if (error.HasError()) {
		throw InternalException("DECIDE failed to rebind '%s' while rebuilding a coefficient: %s", name,
		                        error.Message());
	}
	return result;
}

static unique_ptr<Expression> RebindMultiply(ClientContext &context, unique_ptr<Expression> lhs,
                                             unique_ptr<Expression> rhs) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(lhs));
	children.push_back(std::move(rhs));
	return RebindOperator(context, "*", std::move(children));
}

//! Fold a flattened factor list back into a product. Each binary node is bound
//! for its own operands rather than inheriting the original `*`'s signature:
//! `CollectMultiplicativeFactors` looks through binder casts over decision algebra
//! and (via its callers) drops factors, so neither the operand types nor the arity
//! survive the round trip. Data casts remain complete factors.
static unique_ptr<Expression> BuildCoefficientFromFactors(ClientContext &context,
                                                          const vector<const Expression *> &factors) {
	if (factors.empty()) {
		return nullptr;
	}
	if (factors.size() == 1) {
		return factors[0]->Copy();
	}

	auto result = factors[0]->Copy();
	for (idx_t i = 1; i < factors.size(); i++) {
		result = RebindMultiply(context, std::move(result), factors[i]->Copy());
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Structural helpers
//===--------------------------------------------------------------------===//

// ExpressionIterator::EnumerateChildren has no const overload; this wrapper
// isolates the const_cast so no call site needs to mention it.
static void EnumerateChildrenConst(const Expression &expr,
                                   const std::function<void(unique_ptr<Expression> &)> &callback) {
	ExpressionIterator::EnumerateChildren(const_cast<Expression &>(expr), callback);
}

static bool IsBoundMultiply(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	return func.function.name == "*";
}

static void CollectMultiplicativeFactors(const Expression &expr, idx_t decide_index,
                                         vector<const Expression *> &factors) {
	const Expression *cur = UnwrapDecideCasts(expr, decide_index);
	if (IsBoundMultiply(*cur)) {
		auto &func = cur->Cast<BoundFunctionExpression>();
		for (auto &child : func.children) {
			CollectMultiplicativeFactors(*child, decide_index, factors);
		}
		return;
	}
	factors.push_back(cur);
}

// Distribute multiplication over addition/subtraction: when a `*` chain has
// an additive (`+` / `-` / unary-`-`) factor, expand into a vector of
// (sign, product) pairs, each a pure `*` chain with the additive factor
// replaced by one of its addends. The caller recurses into each pair with
// its sign applied, so `K * (a - b*x)` becomes `(+1, K*a)` and `(-1, K*b*x)`.
//
// Without this expansion, `ClassifyNormalizedProduct` rejects the `(a - b*x)`
// factor as "unexpanded nonlinear product" because it isn't a bare decide-var
// reference, even though the algebraic form is linear in decision vars.
//
// Returns empty when no additive factor is present (caller falls through to
// the existing classification logic).
static vector<pair<int, unique_ptr<Expression>>>
TryDistributeMultiplyOverAdd(ClientContext &context, const BoundFunctionExpression &mul_expr, idx_t decide_index) {
	vector<pair<int, unique_ptr<Expression>>> out;
	vector<const Expression *> factors;
	CollectMultiplicativeFactors(mul_expr, decide_index, factors);

	int additive_idx = -1;
	for (idx_t i = 0; i < factors.size(); i++) {
		const Expression *f = factors[i];
		if (f->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) continue;
		auto &ff = f->Cast<BoundFunctionExpression>();
		if (ff.function.name == "+" && ff.children.size() >= 1) {
			additive_idx = (int)i; break;
		}
		if (ff.function.name == "-" &&
		    (ff.children.size() == 1 || ff.children.size() == 2)) {
			additive_idx = (int)i; break;
		}
	}
	if (additive_idx < 0) return out;

	auto &add_func = factors[additive_idx]->Cast<BoundFunctionExpression>();
	vector<pair<int, const Expression *>> addends;
	if (add_func.function.name == "+") {
		for (auto &c : add_func.children) addends.push_back({1, c.get()});
	} else { // "-"
		if (add_func.children.size() == 2) {
			addends.push_back({1, add_func.children[0].get()});
			addends.push_back({-1, add_func.children[1].get()});
		} else {
			addends.push_back({-1, add_func.children[0].get()});
		}
	}

	for (auto &kv : addends) {
		int s = kv.first;
		const Expression *ad = kv.second;
		vector<const Expression *> new_factors;
		for (idx_t j = 0; j < factors.size(); j++) {
			if ((int)j == additive_idx) continue;
			new_factors.push_back(factors[j]);
		}
		new_factors.push_back(ad);
		auto prod = BuildCoefficientFromFactors(context, new_factors);
		out.push_back({s, std::move(prod)});
	}
	return out;
}

struct NormalizedProductTerm {
	const BoundFunctionExpression *mul_func = nullptr;
	vector<const Expression *> coefficient_factors;
	vector<idx_t> decide_factors;
};

//! Collect DECIDE variable references from a bound expression, tracking sign
//! through subtraction operators. Used for multi-variable per-row constraints.
struct ExprVarRef {
	idx_t var_idx;
	int sign; // +1 or -1
};

//===--------------------------------------------------------------------===//
// DecideLinearFormBuilder
//===--------------------------------------------------------------------===//

//! Flattens the canonical constraint tree and the objective into the prepared
//! linear form. Everything it needs is a type or a structure: the decision
//! variable list, their scopes, and the tags earlier passes stamped on the tree.
//! It never reads a data row.
class DecideLinearFormBuilder {
public:
	DecideLinearFormBuilder(ClientContext &context, LogicalDecide &op)
	    : context(context), op(op), decide_index(op.decide_index),
	      canonicalizer(context, op.decide_index, op.variable_scopes) {
		for (idx_t i = 0; i < op.decide_variables.size(); i++) {
			auto &colref = op.decide_variables[i]->Cast<BoundColumnRefExpression>();
			decide_variable_map[colref.binding] = i;
		}
	}

	void Build() {
		if (op.decide_constraints) {
			AnalyzeConstraint(op.decide_constraints);
		}
		if (op.decide_objective) {
			AnalyzeObjective(op.decide_objective);
		}
		// The composed MIN/MAX terms are emitted by RewriteComposedMinMax as bare
		// inner expressions. Flatten them here too, so stage 08 reads prepared terms
		// for every construct rather than for all but one.
		for (auto &spec : op.composed_minmax_constraints) {
			for (auto &term : spec.terms) {
				ExtractTerms(*term.inner_expr, term.inner_terms);
			}
		}
		for (auto &term : op.composed_minmax_objective_terms) {
			ExtractTerms(*term.inner_expr, term.inner_terms);
		}

		// Group like terms, once, over every list that becomes a linear solver row.
		for (auto &constraint : op.prepared.constraints) {
			CollectLikeTerms(constraint->lhs_terms);
		}
		if (op.prepared.objective) {
			CollectLikeTerms(op.prepared.objective->terms);
		}
		for (auto &spec : op.composed_minmax_constraints) {
			for (auto &term : spec.terms) {
				CollectLikeTerms(term.inner_terms);
			}
		}
		for (auto &term : op.composed_minmax_objective_terms) {
			CollectLikeTerms(term.inner_terms);
		}
	}

private:
	//===------------------------------------------------------------------===//
	// Variable lookup and degree
	//===------------------------------------------------------------------===//

	idx_t FindDecideVariable(const Expression &expr) const {
		// Base case: check if this is a column reference to a DECIDE variable
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &colref = expr.Cast<BoundColumnRefExpression>();
			auto it = decide_variable_map.find(colref.binding);
			if (it != decide_variable_map.end()) {
				return it->second;
			}
		}

		// Recursive case: search in children
		idx_t result = DConstants::INVALID_INDEX;
		EnumerateChildrenConst(expr, [&](unique_ptr<Expression> &child) {
			if (result == DConstants::INVALID_INDEX && child) {
				result = FindDecideVariable(*child);
			}
		});
		return result;
	}

	bool ContainsVariable(const Expression &expr, idx_t var_idx) const {
		// Check if this expression is the variable we're looking for
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &colref = expr.Cast<BoundColumnRefExpression>();
			auto &decide_var = op.decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
			return colref.binding == decide_var.binding;
		}

		// Recursively check children
		bool found = false;
		EnumerateChildrenConst(expr, [&](unique_ptr<Expression> &child) {
			if (!found && child && ContainsVariable(*child, var_idx)) {
				found = true;
			}
		});
		return found;
	}

	//! Assert that the inner expression of a squared or self-product term is degree <= 1.
	//!
	//! This layer does not decide degree — layer 2 does, on the bound tree, before any
	//! rewrite runs, which is why the refusal a user sees names the term they wrote and
	//! points a caret at it. What remains here is the other half of that contract: the
	//! rewrites in this layer synthesize their own expressions (the IN expansion, ABS
	//! linearization, the norm lowering), and nothing at layer 2 can vouch for those. So
	//! this asserts, using layer 2's definition, that what reaches term extraction is
	//! still what layer 2 admitted.
	void AssertSquaredInnerIsLinear(const Expression &inner, const char *shape) const {
		if (!DecideExpressionDegree(inner, decide_index).IsLinear()) {
			throw InternalException(
			    "DECIDE %s reached term extraction with a non-linear inner expression. Layer 2 "
			    "refuses total degree > 2 at bind time, so this tree was produced by a rewrite "
			    "in this layer rather than written by a user.",
			    shape);
		}
	}

	bool TryGetBareDecideFactor(const Expression &expr, idx_t &var_idx) const {
		const Expression *cur = UnwrapDecideCasts(expr, decide_index);
		if (cur->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		var_idx = FindDecideVariable(*cur);
		return var_idx != DConstants::INVALID_INDEX;
	}

	bool ClassifyNormalizedProduct(const Expression &expr, NormalizedProductTerm &result) const {
		const Expression *root = UnwrapDecideCasts(expr, decide_index);
		if (!IsBoundMultiply(*root)) {
			return false;
		}

		result = NormalizedProductTerm();
		result.mul_func = &root->Cast<BoundFunctionExpression>();

		vector<const Expression *> factors;
		CollectMultiplicativeFactors(*root, decide_index, factors);
		for (auto *factor : factors) {
			idx_t var_idx = DConstants::INVALID_INDEX;
			if (TryGetBareDecideFactor(*factor, var_idx)) {
				result.decide_factors.push_back(var_idx);
				continue;
			}
			if (FindDecideVariable(*factor) != DConstants::INVALID_INDEX) {
				throw InvalidInputException(
				    "DECIDE expression contains an unsupported product factor that still "
				    "references decision variables after normalization (total degree > 2 "
				    "or unexpanded nonlinear product). Products must be data factors times "
				    "one DECIDE variable, or data factors times two different DECIDE variables.");
			}
			result.coefficient_factors.push_back(factor);
		}

		if (result.decide_factors.size() > 2) {
			// Degree, so layer 2's judgement, and it already refused this at bind time with
			// a located message. Reaching it here means a rewrite in this layer built the
			// product. (The two refusals below are different: they are formulation limits
			// of this layer over shapes layer 2 legitimately admits, so they stay
			// user-facing.)
			throw InternalException(
			    "DECIDE product reached term extraction with %llu decision factors. Layer 2 "
			    "refuses total degree > 2 at bind time, so this tree was produced by a rewrite "
			    "in this layer rather than written by a user.",
			    static_cast<uint64_t>(result.decide_factors.size()));
		}
		if (result.decide_factors.size() == 2 && result.decide_factors[0] == result.decide_factors[1]) {
			throw InvalidInputException(
			    "DECIDE expression contains a same-variable product that is not in a supported "
			    "quadratic form. Use POWER(linear_expr, 2) or (linear_expr) * (linear_expr) "
			    "for quadratic terms.");
		}
		return true;
	}

	void CollectDecideVarRefs(const Expression &expr, int sign, vector<ExprVarRef> &refs) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			idx_t var_idx = FindDecideVariable(expr);
			if (var_idx != DConstants::INVALID_INDEX) {
				refs.push_back({var_idx, sign});
			}
			return;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &func = expr.Cast<BoundFunctionExpression>();
			if (func.function.name == "-" && func.children.size() == 2) {
				CollectDecideVarRefs(*func.children[0], sign, refs);
				CollectDecideVarRefs(*func.children[1], -sign, refs);
				return;
			}
			if (func.function.name == "+" && func.children.size() == 2) {
				CollectDecideVarRefs(*func.children[0], sign, refs);
				CollectDecideVarRefs(*func.children[1], sign, refs);
				return;
			}
			if (func.function.name == "*" && func.children.size() == 2) {
				// Multiplication: descend into both children to find decide variables.
				// Sign propagates unchanged — * doesn't flip algebraic sign, it changes
				// the coefficient magnitude, which this walk does not measure.
				CollectDecideVarRefs(*func.children[0], sign, refs);
				CollectDecideVarRefs(*func.children[1], sign, refs);
				return;
			}
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = expr.Cast<BoundCastExpression>();
			CollectDecideVarRefs(*cast.child, sign, refs);
			return;
		}
		// Constants, data columns, etc.: no DECIDE vars
	}

	static bool BoundExpressionContainsAggregate(const Expression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			return true;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = expr.Cast<BoundCastExpression>();
			return BoundExpressionContainsAggregate(*cast.child);
		}
		bool found = false;
		EnumerateChildrenConst(expr, [&](unique_ptr<Expression> &child) {
			if (!found && child && BoundExpressionContainsAggregate(*child)) {
				found = true;
			}
		});
		return found;
	}

	//===------------------------------------------------------------------===//
	// Coefficient extraction
	//===------------------------------------------------------------------===//

	unique_ptr<Expression> ExtractCoefficientWithoutVariable(const Expression &expr, idx_t var_idx) const {
		// If this IS the variable itself, return constant 1
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &colref = expr.Cast<BoundColumnRefExpression>();
			auto &decide_var = op.decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
			if (colref.binding == decide_var.binding) {
				return make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
			}
		}

		// If it's a multiplication, filter out children containing the variable
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &func = expr.Cast<BoundFunctionExpression>();
			if (func.function.name == "*") {
				vector<unique_ptr<Expression>> filtered_children;
				for (auto &child : func.children) {
					if (!ContainsVariable(*child, var_idx)) {
						filtered_children.push_back(child->Copy());
						continue;
					}
					// Child contains the variable: recurse to keep its non-variable
					// scalar/data factors — `(2*x)` yields `2`, a bare `x` yields `1`.
					// Dropping the whole child (the previous behavior) silently lost
					// nested coefficients like the `2` in `(2*x)*v`, which reaches here
					// un-normalized on the composed MIN/MAX path. The already-normalized
					// `x*(2*v)` form is unchanged (its variable child is the bare `x`).
					auto sub = ExtractCoefficientWithoutVariable(*child, var_idx);
					if (sub->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
						auto &cv = sub->Cast<BoundConstantExpression>().value;
						if (!cv.IsNull() && cv.type().IsNumeric() &&
						    cv.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>() == 1.0) {
							continue; // bare variable contributes no scalar factor
						}
					}
					filtered_children.push_back(std::move(sub));
				}

				if (filtered_children.empty()) {
					return make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
				}
				if (filtered_children.size() == 1) {
					return std::move(filtered_children[0]);
				}

				// Rebuild the multiplication by re-binding it for the children that
				// actually remain. Dropping the variable also drops the casts above it,
				// so a child can come back narrower than the original signature expects
				// (`CAST(x * price AS DECIMAL(38,2))` yields a bare DECIMAL(15,2)
				// `price`), and dropping a child shifts the rest out of alignment with
				// `function.arguments`. Reusing the original bound function through
				// either of those does not fail — it reinterprets the physical
				// representation and silently computes a wrong coefficient. See
				// `RebindOperator`.
				auto result = std::move(filtered_children[0]);
				for (idx_t i = 1; i < filtered_children.size(); i++) {
					result = RebindMultiply(context, std::move(result), std::move(filtered_children[i]));
				}
				return result;
			}
		}

		// A complete-side semantic cast has already been consumed by constraint
		// analysis before coefficient extraction. Preserve a decision-free cast;
		// the remaining decision cast is a wrapper inside the already-classified
		// coefficient product.
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = expr.Cast<BoundCastExpression>();
			if (FindDecideVariable(expr) == DConstants::INVALID_INDEX) {
				return expr.Copy();
			}
			return ExtractCoefficientWithoutVariable(*cast.child, var_idx);
		}

		// Otherwise, return a copy of the entire expression (no variable in it)
		return expr.Copy();
	}

	//! Result of DetectQuadraticPattern. `inner_linear_expr` is a non-owning
	//! pointer into the tree rooted at the caller's expression (valid only
	//! while that tree is alive). `sign` carries the scalar multiplier from
	//! negation and constant-times-quadratic patterns (e.g. `-POWER(x,2)` → -1,
	//! `(-2)*POWER` → -2). When `inner_linear_expr == nullptr`, no pattern
	//! matched.
	struct QuadraticPattern {
		const Expression *inner_linear_expr = nullptr;
		double sign = 1.0;
	};

	QuadraticPattern DetectQuadraticPattern(const Expression &expr) const {
		// Look through binder-generated wrappers over the decision-bearing expression.
		const Expression *cur = UnwrapDecideCasts(expr, decide_index);
		if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			return {};
		}
		auto &func = cur->Cast<BoundFunctionExpression>();
		string fname = StringUtil::Lower(func.function.name);

		// Fast path: nothing below this point can match on names outside this set.
		// Without the gate every recursive additive (`+`) node in the objective
		// tree would pay for the self-product `ToString() == ToString()` compare,
		// which is O(subtree-size) — turning the walker into O(n^2) on deep sums.
		if (fname != "-" && fname != "*" && fname != "power" && fname != "pow" && fname != "**") {
			return {};
		}

		// -(quadratic)
		if (fname == "-" && func.children.size() == 1) {
			auto inner = DetectQuadraticPattern(*func.children[0]);
			if (inner.inner_linear_expr) {
				return {inner.inner_linear_expr, -inner.sign};
			}
		}

		// K * quadratic or quadratic * K (constant on either side)
		if (fname == "*" && func.children.size() == 2) {
			for (idx_t side = 0; side < 2; side++) {
				double cval;
				if (TryEvaluateFoldableDouble(context, *func.children[side], cval)) {
					if (cval != 0.0) {
						auto inner = DetectQuadraticPattern(*func.children[1 - side]);
						if (inner.inner_linear_expr) {
							return {inner.inner_linear_expr, cval * inner.sign};
						}
					}
				}
			}
		}

		// POWER / POW / **  with literal exponent 2
		if ((fname == "power" || fname == "pow" || fname == "**") && func.children.size() == 2) {
			double exponent;
			if (TryEvaluateFoldableDouble(context, *func.children[1], exponent)) {
				if (exponent == 2.0) {
					const Expression *inner = UnwrapDecideCasts(*func.children[0], decide_index);
					if (FindDecideVariable(*inner) != DConstants::INVALID_INDEX) {
						AssertSquaredInnerIsLinear(*inner, "POWER(..., 2)");
						return {inner, 1.0};
					}
				}
			}
		}

		// (expr) * (expr) with identical children containing a DECIDE variable
		if (fname == "*" && func.children.size() == 2 &&
		    Expression::Equals(*func.children[0], *func.children[1]) &&
		    FindDecideVariable(*func.children[0]) != DConstants::INVALID_INDEX) {
			const Expression *inner = UnwrapDecideCasts(*func.children[0], decide_index);
			AssertSquaredInnerIsLinear(*inner, "self-product (expr) * (expr)");
			return {inner, 1.0};
		}

		return {};
	}

	//===------------------------------------------------------------------===//
	// Linear flattening
	//===------------------------------------------------------------------===//

	void ExtractTerms(const Expression &expr, vector<DecideTerm> &out_terms) const {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &func = expr.Cast<BoundFunctionExpression>();

			// Addition: recursively process all children
			if (func.function.name == "+") {
				for (auto &child : func.children) {
					ExtractTerms(*child, out_terms);
				}
				return;
			}

			// Subtraction: first child positive, second child negated
			if (func.function.name == "-" && func.children.size() == 2) {
				ExtractTerms(*func.children[0], out_terms);
				idx_t before = out_terms.size();
				ExtractTerms(*func.children[1], out_terms);
				for (idx_t i = before; i < out_terms.size(); i++) {
					out_terms[i].sign *= -1;
				}
				return;
			}

			// Unary minus: recurse and flip sign of every produced term.
			if (func.function.name == "-" && func.children.size() == 1) {
				idx_t before = out_terms.size();
				ExtractTerms(*func.children[0], out_terms);
				for (idx_t i = before; i < out_terms.size(); i++) {
					out_terms[i].sign *= -1;
				}
				return;
			}

			// Multiplication: extract variable and coefficient
			if (func.function.name == "*") {
				// If the `*` chain has an additive factor (e.g. `K * (1 - pick)`),
				// distribute first so each resulting product is `coef * var`-shaped.
				// Without this, ExtractCoefficientWithoutVariable would silently
				// drop the additive structure and produce a wrong coefficient.
				auto distributed = TryDistributeMultiplyOverAdd(context, func, decide_index);
				if (!distributed.empty()) {
					for (auto &kv : distributed) {
						idx_t before = out_terms.size();
						ExtractTerms(*kv.second, out_terms);
						if (kv.first == -1) {
							for (idx_t i = before; i < out_terms.size(); i++) {
								out_terms[i].sign *= -1;
							}
						}
					}
					return;
				}

				idx_t var_idx = FindDecideVariable(func);

				if (var_idx == DConstants::INVALID_INDEX) {
					// No variable found - this is a constant term
					out_terms.push_back(DecideTerm {DConstants::INVALID_INDEX, func.Copy()});
				} else {
					// Variable found - extract coefficient
					auto coef = ExtractCoefficientWithoutVariable(func, var_idx);
					out_terms.push_back(DecideTerm {var_idx, std::move(coef)});
				}
				return;
			}

			// Division by a DECIDE-variable-free expression: recurse into the
			// numerator and wrap every produced term's coefficient in `coef / divisor`.
			// Division where the divisor itself contains a decide variable is
			// non-linear and is already rejected upstream by the bind-time validator.
			// Cast both sides to the `/` function's expected argument types so
			// an extracted integer coefficient doesn't silently turn into
			// integer-division truncation (e.g., `x/2` gave 0 when coef was INT 1),
			// then bind the result for the operands it actually has.
			if (func.function.name == "/" && func.children.size() == 2 &&
			    FindDecideVariable(*func.children[1]) == DConstants::INVALID_INDEX) {
				idx_t before = out_terms.size();
				ExtractTerms(*func.children[0], out_terms);
				D_ASSERT(func.function.arguments.size() == 2);
				const auto &num_type = func.function.arguments[0];
				const auto &denom_type = func.function.arguments[1];
				for (idx_t i = before; i < out_terms.size(); i++) {
					vector<unique_ptr<Expression>> div_children;
					div_children.push_back(
					    BoundCastExpression::AddDefaultCastToType(std::move(out_terms[i].coefficient), num_type));
					div_children.push_back(
					    BoundCastExpression::AddDefaultCastToType(func.children[1]->Copy(), denom_type));
					out_terms[i].coefficient = RebindOperator(context, "/", std::move(div_children));
				}
				return;
			}
		}

		// Handle casts
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = expr.Cast<BoundCastExpression>();
			if (FindDecideVariable(expr) == DConstants::INVALID_INDEX) {
				// A decision-free cast is a real DuckDB value operation and belongs
				// in the coefficient/fixed-offset expression exactly as written.
				out_terms.push_back(DecideTerm {DConstants::INVALID_INDEX, expr.Copy()});
			} else {
				ExtractTerms(*cast.child, out_terms);
			}
			return;
		}

		// Base case: constant or simple column reference
		idx_t var_idx = FindDecideVariable(expr);
		if (var_idx == DConstants::INVALID_INDEX) {
			// Constant term
			out_terms.push_back(DecideTerm {DConstants::INVALID_INDEX, expr.Copy()});
		} else {
			// Just a variable (coefficient = 1)
			out_terms.push_back(
			    DecideTerm {var_idx, make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))});
		}
	}

	//===------------------------------------------------------------------===//
	// Like-term collection
	//===------------------------------------------------------------------===//

	//! Whether two terms describe the same contribution and may be summed into one.
	//!
	//! Naming the same variable is not enough. A term also carries *which rows it
	//! applies to* and *which reducer produced it*, and two terms that disagree on
	//! any of that are different contributions that happen to share a column:
	//!
	//! - `reduction` separates a reducer term from a row-invariant one. `SUM(x)` and
	//!   a query-wide `x` in the same clause are summed differently downstream.
	//! - `filter` is the aggregate-local `WHEN`. `SUM(x) WHEN a` and `SUM(x) WHEN b`
	//!   name one column over two row sets; merging them would apply one mask to both.
	//! - `avg_scale` divides by the group's row count, so an AVG term and a SUM term
	//!   are not summable before that scaling happens.
	//! - `qualifier_scope_idx` selects a de-duplication mask (`sum(D: ...)`), which is
	//!   again a statement about which rows contribute.
	//!
	//! Constants (`INVALID_INDEX`) are deliberately left alone: they are a fixed
	//! offset folded into the RHS, not a repeated column, and they are not what any
	//! consumer iterating `variable_indices` can trip over.
	static bool TermsAreLike(const DecideTerm &a, const DecideTerm &b) {
		if (a.variable_index == DConstants::INVALID_INDEX || a.variable_index != b.variable_index) {
			return false;
		}
		if (a.reduction != b.reduction || a.avg_scale != b.avg_scale ||
		    a.qualifier_scope_idx != b.qualifier_scope_idx) {
			return false;
		}
		if ((a.filter == nullptr) != (b.filter == nullptr)) {
			return false;
		}
		return !a.filter || a.filter->Equals(*b.filter);
	}

	//! Sum like terms into one, in place, preserving first-occurrence order.
	//!
	//! `2*ship + 3*ship` used to reach the solver as two terms naming one column. The
	//! model builder folded the duplicate when writing the matrix row, so the emitted
	//! model was always right -- but every other consumer had to remember that an
	//! index can repeat, and one of them did not: the implied-bound derivation read a
	//! single term's coefficient instead of the sum (fixed 2026-08-15 by a defensive
	//! accumulate, which stays). Collecting here removes the trap at its source.
	//!
	//! A term contributes `sign * coefficient`, so a group merges as
	//! `sign_first * (coef_first ± coef_next ± ...)`, taking `-` exactly when a term's
	//! sign differs from the group's. The result is bound through `FunctionBinder`
	//! like every other rebuilt coefficient.
	void CollectLikeTerms(vector<DecideTerm> &terms) const {
		if (terms.size() < 2) {
			return;
		}
		vector<DecideTerm> out;
		out.reserve(terms.size());
		// Bucket by variable so the scan for a match stays short on a wide clause.
		unordered_map<idx_t, vector<idx_t>> candidates;
		for (auto &term : terms) {
			idx_t target = DConstants::INVALID_INDEX;
			auto bucket = candidates.find(term.variable_index);
			if (bucket != candidates.end()) {
				for (auto slot : bucket->second) {
					if (TermsAreLike(out[slot], term)) {
						target = slot;
						break;
					}
				}
			}
			if (target == DConstants::INVALID_INDEX) {
				candidates[term.variable_index].push_back(out.size());
				out.push_back(std::move(term));
				continue;
			}
			vector<unique_ptr<Expression>> children;
			children.push_back(std::move(out[target].coefficient));
			children.push_back(std::move(term.coefficient));
			out[target].coefficient =
			    RebindOperator(context, out[target].sign == term.sign ? "+" : "-", std::move(children));
		}
		terms = std::move(out);
	}

	//===------------------------------------------------------------------===//
	// Reducer metadata and peeled scales
	//===------------------------------------------------------------------===//

	//! Entity scope a reducer is qualified by (`sum(D: ...)`), read back from the tag the
	//! binder stamped on the aggregate; INVALID_INDEX when the reducer is unqualified.
	static idx_t QualifierScopeOf(const BoundAggregateExpression &agg) {
		idx_t scope_idx = DConstants::INVALID_INDEX;
		TryParseQualifiedReducerTag(agg.alias, scope_idx);
		return scope_idx;
	}

	static void ApplyAggregateMetadata(vector<DecideTerm> &terms, idx_t begin, const BoundAggregateExpression &agg) {
		bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
		idx_t qualifier_scope = QualifierScopeOf(agg);
		for (idx_t i = begin; i < terms.size(); i++) {
			if (agg.filter) {
				terms[i].filter = agg.filter->Copy();
			}
			terms[i].avg_scale = is_avg;
			terms[i].qualifier_scope_idx = qualifier_scope;
			terms[i].reduction = LinearTermReduction::SUM;
		}
	}

	//! Name an expression the way the user wrote it: strip the casts the binder added.
	static string ScaleUserName(const Expression &expr) {
		const Expression *cur = StripCastsForIdentity(expr);
		auto name = StripDecideTags(cur->GetName());
		return name.empty() ? cur->ToString() : name;
	}

	//! Multiply a coefficient by a factor that stayed outside a reducer, keeping the
	//! operand types the original `*` / `/` node was bound for and then binding the
	//! rebuilt node through `FunctionBinder`. Casting first preserves the division
	//! semantics the user's expression was bound with; binding after is what removes
	//! the need to reuse another node's `FunctionData`.
	unique_ptr<Expression> ScaleCoefficient(const BoundFunctionExpression &scale_func, const Expression &scale,
	                                        bool divides, unique_ptr<Expression> coef) const {
		const auto &coef_type = scale_func.function.arguments[divides ? 0 : 1];
		const auto &scale_type = scale_func.function.arguments[divides ? 1 : 0];
		vector<unique_ptr<Expression>> children;
		// `scale * coef` keeps the factor on the left, matching the canonical
		// spelling; `coef / scale` has to keep the operand order division needs.
		if (divides) {
			children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(coef), coef_type));
			children.push_back(BoundCastExpression::AddDefaultCastToType(scale.Copy(), scale_type));
		} else {
			children.push_back(BoundCastExpression::AddDefaultCastToType(scale.Copy(), scale_type));
			children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(coef), coef_type));
		}
		return RebindOperator(context, divides ? "/" : "*", std::move(children));
	}

	//! Multiply everything the aggregate under a peeled scale just produced by that
	//! scale. Folding the factor into the reducer's body is what the canonicalizer
	//! refuses to do, because at the parsed level the aggregate may still be MIN/MAX
	//! and `MAX(-2x)` is `-2*MIN(x)`, not `-2*MAX(x)`. Here it is safe and exact: the
	//! optimizer has already rewritten every MIN/MAX to SUM (asserted below), and a
	//! sum distributes over any factor regardless of sign.
	void ApplyScaleToExtracted(const BoundFunctionExpression &scale_func, const Expression &scale, bool divides,
	                           DecideConstraint &constraint, idx_t linear_before, idx_t bilinear_before,
	                           idx_t quadratic_before) {
		auto scaled = [&](unique_ptr<Expression> coef) {
			return ScaleCoefficient(scale_func, scale, divides, std::move(coef));
		};
		for (idx_t i = linear_before; i < constraint.lhs_terms.size(); i++) {
			constraint.lhs_terms[i].coefficient = scaled(std::move(constraint.lhs_terms[i].coefficient));
		}
		for (idx_t i = bilinear_before; i < constraint.bilinear_terms.size(); i++) {
			auto &bt = constraint.bilinear_terms[i];
			// A null coefficient means 1.0; the scale becomes the whole coefficient.
			bt.coefficient = bt.coefficient
			                     ? scaled(std::move(bt.coefficient))
			                     : scaled(make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1)));
		}
		if (quadratic_before == constraint.quadratic_groups.size()) {
			return;
		}
		// Fold a literal immediately. A query-wide subquery scale cannot be evaluated
		// until the relational input has run, so retain it on the quadratic group.
		if (!scale.IsFoldable()) {
			for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
				constraint.quadratic_groups[i].scale = scale.Copy();
				constraint.quadratic_groups[i].scale_divides = divides;
			}
			return;
		}
		double factor =
		    ExpressionExecutor::EvaluateScalar(context, scale).DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		if (divides && factor == 0.0) {
			throw InvalidInputException("DECIDE constraint: division by zero in a squared term.");
		}
		for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
			constraint.quadratic_groups[i].sign *= divides ? 1.0 / factor : factor;
		}
	}

	//! Objective twin of ApplyScaleToExtracted: multiply a factor that stayed outside a
	//! reducer into everything the reducer produced. `quadratic_sign` is a number
	//! rather than an expression, so a squared term needs the factor's value here.
	void ApplyScaleToObjective(const BoundFunctionExpression &scale_func, const Expression &scale, bool divides,
	                           DecideObjective &obj, idx_t linear_before, idx_t bilinear_before, idx_t squared_before) {
		auto scaled = [&](unique_ptr<Expression> coef) {
			return ScaleCoefficient(scale_func, scale, divides, std::move(coef));
		};
		for (idx_t i = linear_before; i < obj.terms.size(); i++) {
			obj.terms[i].coefficient = scaled(std::move(obj.terms[i].coefficient));
		}
		for (idx_t i = bilinear_before; i < obj.bilinear_terms.size(); i++) {
			auto &bt = obj.bilinear_terms[i];
			bt.coefficient = bt.coefficient
			                     ? scaled(std::move(bt.coefficient))
			                     : scaled(make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1)));
		}
		if (squared_before == obj.squared_terms.size()) {
			return;
		}
		double factor;
		if (!TryEvaluateFoldableDouble(context, scale, factor)) {
			throw InvalidInputException(
			    "DECIDE objective: a squared term cannot be multiplied by '%s', whose value is "
			    "not known until the query runs. Use a constant factor, or move it inside the "
			    "aggregate as SUM(%s * POWER(...)).",
			    scale.GetName(), scale.GetName());
		}
		if (divides && factor == 0.0) {
			throw InvalidInputException("DECIDE objective: division by zero in a squared term.");
		}
		obj.quadratic_sign *= divides ? 1.0 / factor : factor;
	}

	//! True when the decision referenced by `expr` is query-wide (`scalar`).
	//! Such a term is a complete objective contribution on its own: it maps to a
	//! single solver column, so there is no reducer to collapse it.
	bool IsScalarDecideTerm(const Expression &expr) const {
		idx_t var_idx = FindDecideVariable(expr);
		return var_idx != DConstants::INVALID_INDEX && var_idx < op.variable_scopes.size() &&
		       op.variable_scopes[var_idx].IsScalar();
	}

	//===------------------------------------------------------------------===//
	// Constraints
	//===------------------------------------------------------------------===//

	void ExtractAggregateConstraintTerms(const Expression &expr, DecideConstraint &constraint, int sign) {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			if (FindDecideVariable(expr) != DConstants::INVALID_INDEX) {
				ExtractAggregateConstraintTerms(*expr.Cast<BoundCastExpression>().child, constraint, sign);
			} else {
				idx_t before = constraint.lhs_terms.size();
				ExtractTerms(expr, constraint.lhs_terms);
				if (sign == -1) {
					for (idx_t i = before; i < constraint.lhs_terms.size(); i++) {
						constraint.lhs_terms[i].sign *= -1;
					}
				}
			}
			return;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &func = expr.Cast<BoundFunctionExpression>();
			if (func.function.name == "+") {
				for (auto &child : func.children) {
					ExtractAggregateConstraintTerms(*child, constraint, sign);
				}
				return;
			}
			if (func.function.name == "-" && func.children.size() == 2) {
				ExtractAggregateConstraintTerms(*func.children[0], constraint, sign);
				ExtractAggregateConstraintTerms(*func.children[1], constraint, -sign);
				return;
			}
			if (func.function.name == "-" && func.children.size() == 1) {
				ExtractAggregateConstraintTerms(*func.children[0], constraint, -sign);
				return;
			}
		}
		// A scaled reducer: unwrap to the reducer, extract it, then apply the factor to
		// everything that came out.
		{
			ScaledAggregateMatch scale_match;
			if (TryMatchScaledAggregate(expr, decide_index, scale_match)) {
				auto scale = scale_match.scale;
				// Defensive invariant: both user-written and optimizer-generated
				// constraints pass canonical validation. Keep this guard so an
				// in-place optimizer mutation cannot turn a decision into a coefficient
				// and crash during evaluation.
				if (FindDecideVariable(*scale) != DConstants::INVALID_INDEX) {
					throw InternalException(
					    "DECIDE constraint: '%s' is a decision, so it cannot multiply an "
					    "aggregate. Only constants and query-wide values can scale "
					    "SUM/AVG/MIN/MAX.",
					    ScaleUserName(*scale));
				}
				idx_t linear_before = constraint.lhs_terms.size();
				idx_t bilinear_before = constraint.bilinear_terms.size();
				idx_t quadratic_before = constraint.quadratic_groups.size();
				ExtractAggregateConstraintTerms(*scale_match.aggregate, constraint, sign);
				ApplyScaleToExtracted(*scale_match.function, *scale, scale_match.divides, constraint, linear_before,
				                      bilinear_before, quadratic_before);
				return;
			}
		}
		// A query-wide (`scalar`) decision is row-invariant, so it is a complete term of
		// an aggregate constraint on its own -- there is nothing for a reducer to collapse.
		// This is K3's "reducer or row-invariant" rule; the objective path already reads
		// the same way (see ExtractAggregateObjectiveTerms).
		if (IsScalarDecideTerm(expr)) {
			ExtractConstraintTerms(expr, constraint, sign);
			return;
		}
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			throw InternalException("DECIDE aggregate constraint LHS contains a non-reducer term after canonical "
			                        "verification: %s",
			                        expr.ToString());
		}

		auto &agg = expr.Cast<BoundAggregateExpression>();
		auto agg_name = StringUtil::Lower(agg.function.name);
		if (agg_name != "sum") {
			throw InternalException("DECIDE optimizer did not rewrite aggregate '%s' to SUM before execution",
			                        agg.function.name);
		}
		bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
		idx_t qualifier_scope = QualifierScopeOf(agg);

		idx_t linear_before = constraint.lhs_terms.size();
		idx_t bilinear_before = constraint.bilinear_terms.size();
		idx_t quadratic_before = constraint.quadratic_groups.size();
		ExtractConstraintTerms(*agg.children[0], constraint, sign);
		ApplyAggregateMetadata(constraint.lhs_terms, linear_before, agg);
		for (idx_t i = bilinear_before; i < constraint.bilinear_terms.size(); i++) {
			if (agg.filter) {
				constraint.bilinear_terms[i].filter = agg.filter->Copy();
			}
			constraint.bilinear_terms[i].avg_scale = is_avg;
			constraint.bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
		}
		for (idx_t i = quadratic_before; i < constraint.quadratic_groups.size(); i++) {
			if (agg.filter) {
				constraint.quadratic_groups[i].filter = agg.filter->Copy();
			}
			constraint.quadratic_groups[i].avg_scale = is_avg;
			constraint.quadratic_groups[i].qualifier_scope_idx = qualifier_scope;
		}

		string minmax_payload;
		if (ExtractDecideTagPayload(agg.alias, MINMAX_INDICATOR_TAG_PREFIX, minmax_payload)) {
			// "<idx>_<agg>" when a Big-M indicator was allocated, bare "<agg>" when the
			// backend states MIN/MAX itself and no indicator exists. The aggregate name is
			// the part that is always there, and it is what marks the row downstream.
			auto sep = minmax_payload.find('_');
			if (sep == string::npos) {
				constraint.minmax_agg_type = minmax_payload;
			} else {
				constraint.minmax_clause_idx = std::stoull(minmax_payload.substr(0, sep));
				constraint.minmax_agg_type = minmax_payload.substr(sep + 1);
			}
			constraint.kind = ConstraintKind::USER_MECHANISM;
		}
	}

	//! Extract linear and bilinear terms from a SUM argument in a constraint.
	void ExtractConstraintTerms(const Expression &expr, DecideConstraint &constr, int sign,
	                            const Expression *filter = nullptr) {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &func = expr.Cast<BoundFunctionExpression>();
			string fname = func.function.name;

			if (fname == "+") {
				for (auto &child : func.children) {
					ExtractConstraintTerms(*child, constr, sign, filter);
				}
				return;
			}
			if (fname == "-" && func.children.size() == 2) {
				ExtractConstraintTerms(*func.children[0], constr, sign, filter);
				ExtractConstraintTerms(*func.children[1], constr, -sign, filter);
				return;
			}
			if (fname == "-" && func.children.size() == 1) {
				ExtractConstraintTerms(*func.children[0], constr, -sign, filter);
				return;
			}
			// Helper: try to detect POWER(expr, 2), POW(expr, 2), expr ** 2,
			// or (expr)*(expr) self-product. Returns the inner expression on success.
			auto TryDetectConstraintQuadratic = [&](const Expression *test_expr) -> const Expression * {
				test_expr = UnwrapDecideCasts(*test_expr, decide_index);
				if (test_expr->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) return nullptr;
				auto &qf = test_expr->Cast<BoundFunctionExpression>();
				string qname = StringUtil::Lower(qf.function.name);
				// POWER/POW/** with exponent 2
				if ((qname == "power" || qname == "pow" || qname == "**") && qf.children.size() == 2) {
					double exponent;
					if (TryEvaluateFoldableDouble(context, *qf.children[1], exponent)) {
						if (exponent == 2.0) {
							const Expression *inner = UnwrapDecideCasts(*qf.children[0], decide_index);
							if (FindDecideVariable(*inner) != DConstants::INVALID_INDEX) {
								AssertSquaredInnerIsLinear(*inner, "POWER(..., 2)");
								return inner;
							}
						}
					}
				}
				// Self-product: (expr)*(expr) with identical sides
				if (qname == "*" && qf.children.size() == 2 &&
				    Expression::Equals(*qf.children[0], *qf.children[1]) &&
				    FindDecideVariable(*qf.children[0]) != DConstants::INVALID_INDEX) {
					const Expression *inner = UnwrapDecideCasts(*qf.children[0], decide_index);
					AssertSquaredInnerIsLinear(*inner, "self-product (expr) * (expr)");
					return inner;
				}
				return nullptr;
			};

			// Direct POWER/self-product detection
			{
				const Expression *inner = TryDetectConstraintQuadratic(&func);
				if (inner) {
					DecideConstraint::QuadraticGroup qg;
					qg.sign = static_cast<double>(sign);
					if (filter) {
						qg.filter = filter->Copy();
					}
					ExtractTerms(*inner, qg.inner_terms);
					constr.quadratic_groups.push_back(std::move(qg));
					constr.has_quadratic = true;
					return;
				}
			}
			if (fname == "*") {
				// Scaled quadratic: const * POWER(expr, 2) or POWER(expr, 2) * const
				if (func.children.size() == 2) {
					for (idx_t side = 0; side < 2; side++) {
						double cval;
						if (TryEvaluateFoldableDouble(context, *func.children[side], cval)) {
							if (cval != 0.0) {
								const Expression *inner = TryDetectConstraintQuadratic(func.children[1 - side].get());
								if (inner) {
									DecideConstraint::QuadraticGroup qg;
									qg.sign = static_cast<double>(sign) * cval;
									if (filter) {
										qg.filter = filter->Copy();
									}
									ExtractTerms(*inner, qg.inner_terms);
									constr.quadratic_groups.push_back(std::move(qg));
									constr.has_quadratic = true;
									return;
								}
							}
						}
					}
				}
				// Distribution must come BEFORE ClassifyNormalizedProduct, since
				// the classifier throws on additive factors instead of returning
				// false. Shapes like `K * (1 - pick)` reach here from MIN/MAX
				// hard-direction rewrites and other paths the symbolic normalizer
				// didn't fully expand.
				{
					auto distributed = TryDistributeMultiplyOverAdd(context, func, decide_index);
					if (!distributed.empty()) {
						for (auto &kv : distributed) {
							ExtractConstraintTerms(*kv.second, constr, sign * kv.first, filter);
						}
						return;
					}
				}
				NormalizedProductTerm product;
				if (ClassifyNormalizedProduct(func, product)) {
					if (product.decide_factors.size() == 2) {
						BilinearConstraintTerm bt;
						bt.var_a = product.decide_factors[0];
						bt.var_b = product.decide_factors[1];
						bt.coefficient = BuildCoefficientFromFactors(context, product.coefficient_factors);
						bt.sign = sign;
						if (filter) {
							bt.filter = filter->Copy();
						}
						constr.bilinear_terms.push_back(std::move(bt));
						constr.has_bilinear = true;
						return;
					}
				}
			}
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = expr.Cast<BoundCastExpression>();
			if (FindDecideVariable(expr) == DConstants::INVALID_INDEX) {
				// A decision-free cast is a real value operation the executor performs,
				// so it stays whole as a typed fixed term. Peeling it would change the
				// coefficient -- see .claude/lessons.md on rebinding narrowed children.
				idx_t before = constr.lhs_terms.size();
				ExtractTerms(expr, constr.lhs_terms);
				if (sign == -1) {
					for (idx_t i = before; i < constr.lhs_terms.size(); i++) {
						constr.lhs_terms[i].sign *= -1;
					}
				}
			} else {
				// Decision-bearing casts surviving binding are DuckDB's internal
				// type-reconciliation wrappers; explicit source casts were rejected
				// on the parsed tree.
				ExtractConstraintTerms(*cast.child, constr, sign, filter);
			}
			return;
		}
		// Linear — delegate to ExtractTerms
		idx_t before = constr.lhs_terms.size();
		ExtractTerms(expr, constr.lhs_terms);
		if (sign == -1) {
			for (idx_t i = before; i < constr.lhs_terms.size(); i++) {
				constr.lhs_terms[i].sign *= -1;
			}
		}
		if (filter) {
			for (idx_t i = before; i < constr.lhs_terms.size(); i++) {
				constr.lhs_terms[i].filter = filter->Copy();
			}
		}
	}

	void AnalyzeConstraint(const unique_ptr<Expression> &expr_ptr, unique_ptr<Expression> when_condition = nullptr,
	                       vector<unique_ptr<Expression>> per_columns = {}) {
		auto &expr = *expr_ptr;
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::BOUND_CONJUNCTION: {
			auto &conj = expr.Cast<BoundConjunctionExpression>();
			// DecidB: PER wrapper — outermost layer
			if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
				// child[0] = the constraint (possibly WHEN-wrapped)
				// children[1..N] = the PER column expressions
				vector<unique_ptr<Expression>> per_cols;
				for (idx_t i = 1; i < conj.children.size(); i++) {
					per_cols.push_back(conj.children[i]->Copy());
				}
				AnalyzeConstraint(conj.children[0], std::move(when_condition), std::move(per_cols));
				break;
			}
			// DecidB: Check if this is a WHEN constraint wrapper
			if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) && conj.children.size() == 2) {
				// child[0] = the actual constraint, child[1] = the WHEN condition
				AnalyzeConstraint(conj.children[0], conj.children[1]->Copy(), std::move(per_columns));
				break;
			}
			// Regular conjunction: recursively analyze each child
			for (auto &child : conj.children) {
				AnalyzeConstraint(child);
			}
			break;
		}

		case ExpressionClass::BOUND_COMPARISON: {
			auto &comp = expr.Cast<BoundComparisonExpression>();

			// Skip comparisons the optimizer already folded into the column box.
			// Emitting a DecideConstraint here would add num_rows redundant model
			// rows. The comparison is still in the tree so EXPLAIN renders it; the
			// tag is the decision, and it was made by AbsorbVariableBounds.
			if (HasDecideTag(comp.alias, ABSORBED_BOUND_TAG)) {
				break;
			}

			auto constraint = make_uniq<DecideConstraint>();
			constraint->comparison_type = comp.type;
			constraint->rhs_expr = comp.right->Copy();
			TryParseSourceClauseTag(comp.GetAlias(), constraint->source_clause_id);

			// Parse not-equal indicator tag if present
			string payload;
			if (ExtractDecideTagPayload(comp.alias, NE_INDICATOR_TAG_PREFIX, payload)) {
				constraint->ne_clause_idx = std::stoull(payload);
				constraint->kind = ConstraintKind::USER_MECHANISM;
			}

			// Parse ABS MAXIMIZE upper-bound tag: marks a lower-bound ABS constraint
			// (aux >= inner or aux >= -inner) that needs Big-M upper bounds at finalization.
			if (HasDecideTag(comp.alias, STRUCTURAL_CONSTRAINT_TAG)) {
				constraint->kind = ConstraintKind::STRUCTURAL;
			}
			if (ExtractDecideTagPayload(comp.alias, ABS_UB_POS_TAG_PREFIX, payload)) {
				constraint->abs_aux_idx = std::stoull(payload);
				constraint->abs_is_pos_bound = true;
				constraint->kind = ConstraintKind::STRUCTURAL;
			} else if (ExtractDecideTagPayload(comp.alias, ABS_UB_NEG_TAG_PREFIX, payload)) {
				constraint->abs_aux_idx = std::stoull(payload);
				constraint->abs_is_pos_bound = false;
				constraint->kind = ConstraintKind::STRUCTURAL;
			}

			// Detect easy-direction MIN/MAX optimizer rewrite (see decide.hpp).
			if (HasDecideTag(comp.alias, MINMAX_EASY_REWRITE_TAG)) {
				constraint->was_minmax_easy = true;
			}

			// DecidB: Store WHEN condition and PER columns if present
			if (when_condition) {
				constraint->when_condition = std::move(when_condition);
			}
			if (!per_columns.empty()) {
				constraint->per_columns = std::move(per_columns);
			}

			// Extract terms from LHS
			// Only binder-generated wrappers over decision algebra are transparent.
			// A data cast is a SQL computation and UnwrapDecideCasts stops at it.
			Expression *lhs = UnwrapDecideCasts(*comp.left, decide_index);

			auto constraint_class = canonicalizer.ClassifyCanonicalComparison(comp);
			if (constraint_class == CanonicalConstraintClass::INVALID) {
				throw InternalException(
				    "DECIDE constraint reached term extraction with invalid aggregate/per-row "
				    "homogeneity: '%s'. Canonical validation must reject this during planning.",
				    comp.ToString());
			}

			if (constraint_class == CanonicalConstraintClass::AGGREGATE) {
				// Aggregate constraint. Handles both legacy single aggregates and
				// additive aggregate expressions with aggregate-local WHEN filters. The
				// classification comes from the canonical boundary rather than aggregate
				// presence alone, so a data-only RHS reducer cannot change row semantics.
				constraint->lhs_is_aggregate = true;
				ExtractAggregateConstraintTerms(*lhs, *constraint, 1);
			} else {
				// Per-row constraint (e.g., x <= 5, or multi-variable: d >= x - c)
				constraint->lhs_is_aggregate = false;

				// K1 guard. DecideCanonicalizer puts every decision-bearing term on
				// the left, so a decision variable reaching the RHS here means the
				// invariant was broken upstream -- by a new optimizer rewrite that
				// mutates a constraint in place instead of going through
				// LogicalDecide::AddConstraint, most likely. This used to be a second
				// implementation of the partition (the canonicalization refactor); it was
				// verified unreachable across the golden corpus and the full suite
				// before being replaced by the check, so a wrong answer here would
				// otherwise be silent.
				vector<ExprVarRef> rhs_refs;
				CollectDecideVarRefs(*comp.right, +1, rhs_refs);
				if (!rhs_refs.empty()) {
					throw InternalException(
					    "DECIDE constraint is not canonical: decision variable on the right-hand "
					    "side of '%s'. Constraints must be canonicalized by DecideCanonicalizer "
					    "before reaching term extraction.",
					    comp.right->ToString());
				}

				if (lhs->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
					// Simple single-variable constraint (e.g., x <= 5)
					idx_t var_idx = FindDecideVariable(*lhs);
					if (var_idx != DConstants::INVALID_INDEX) {
						constraint->lhs_terms.push_back(DecideTerm {
						    var_idx, make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1))});
					}
				} else {
					// Multi-variable per-row constraint with complex LHS
					// (e.g., z_0 + z_1 = 1, or x + (-3)*z_0 + (-5)*z_1 = 0,
					//  or POWER(x - target, 2) <= K quadratic constraint)
					ExtractConstraintTerms(*lhs, *constraint, 1);
				}
			}

			op.prepared.constraints.push_back(std::move(constraint));
			break;
		}

		default:
			break;
		}
	}

	//===------------------------------------------------------------------===//
	// Objective
	//===------------------------------------------------------------------===//

	//! Walk a SUM argument expression tree and split into linear terms and bilinear terms.
	//! Bilinear terms (x * y where both are decide variables) go to objective->bilinear_terms.
	//! Linear terms (c * x, constants) go to objective->terms via ExtractTerms.
	void ExtractLinearAndBilinearTerms(const Expression &expr, DecideObjective &obj, int sign,
	                                   const Expression *filter = nullptr) {
		// DecidB: detect quadratic patterns (POWER / x*x / negated / const * POWER)
		// *before* any linear-structure traversal. This allows mixed shapes like
		// SUM(POWER(x-t, 2) + penalty*x) to route the POWER leaf into squared_terms
		// while the `+` recursion below sends the linear sibling into terms.
		//
		// The objective currently supports exactly one quadratic group per
		// objective (the inner expression of a single SUM(POWER(...))), with a
		// single scalar quadratic_sign. Additional quadratic groups (e.g.
		// `SUM(POWER(x,2)) + SUM(POWER(y,2))`) would need per-group Q matrices
		// downstream and are explicitly rejected.
		auto quad_pattern = DetectQuadraticPattern(expr);
		if (quad_pattern.inner_linear_expr) {
			double effective_sign = quad_pattern.sign * static_cast<double>(sign);
			if (obj.has_quadratic) {
				throw InvalidInputException(
				    "DECIDE objective contains multiple quadratic (POWER / (expr)*(expr)) "
				    "groups. Only a single quadratic group plus linear terms is supported; "
				    "combine them mathematically or rewrite the objective.");
			}
			obj.has_quadratic = true;
			obj.quadratic_sign = effective_sign;
			idx_t before = obj.squared_terms.size();
			ExtractTerms(*quad_pattern.inner_linear_expr, obj.squared_terms);
			if (filter) {
				for (idx_t i = before; i < obj.squared_terms.size(); i++) {
					obj.squared_terms[i].filter = filter->Copy();
				}
			}
			return;
		}

		if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &func = expr.Cast<BoundFunctionExpression>();
			string fname = func.function.name;

			// Addition: recurse on all children
			if (fname == "+") {
				for (auto &child : func.children) {
					ExtractLinearAndBilinearTerms(*child, obj, sign, filter);
				}
				return;
			}

			// Subtraction: first child same sign, second negated
			if (fname == "-" && func.children.size() == 2) {
				ExtractLinearAndBilinearTerms(*func.children[0], obj, sign, filter);
				ExtractLinearAndBilinearTerms(*func.children[1], obj, -sign, filter);
				return;
			}

			// Unary negation
			if (fname == "-" && func.children.size() == 1) {
				ExtractLinearAndBilinearTerms(*func.children[0], obj, -sign, filter);
				return;
			}

			// Multiplication: the parsed normalizer is responsible for algebraic
			// expansion; here we only flatten already-normalized product factors
			// for classification.
			if (fname == "*") {
				// Distribute before ClassifyNormalizedProduct (which throws on
				// additive factors). See ExtractConstraintTerms for rationale.
				{
					auto distributed = TryDistributeMultiplyOverAdd(context, func, decide_index);
					if (!distributed.empty()) {
						for (auto &kv : distributed) {
							ExtractLinearAndBilinearTerms(*kv.second, obj, sign * kv.first, filter);
						}
						return;
					}
				}
				NormalizedProductTerm product;
				if (ClassifyNormalizedProduct(func, product)) {
					if (product.decide_factors.size() == 2) {
						DecideObjective::BilinearTerm bt;
						bt.var_a = product.decide_factors[0];
						bt.var_b = product.decide_factors[1];
						bt.coefficient = BuildCoefficientFromFactors(context, product.coefficient_factors);
						bt.sign = sign;
						if (filter) {
							bt.filter = filter->Copy();
						}
						obj.bilinear_terms.push_back(std::move(bt));
						obj.has_bilinear = true;
						return;
					}
				}
			}
		}

		// A decision-bearing cast here is binder noise. Preserve a data-only cast
		// as a complete fixed term so DuckDB performs the written computation.
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = expr.Cast<BoundCastExpression>();
			if (FindDecideVariable(expr) != DConstants::INVALID_INDEX) {
				ExtractLinearAndBilinearTerms(*cast.child, obj, sign, filter);
				return;
			}
		}

		// Not bilinear — delegate to linear extraction
		idx_t before = obj.terms.size();
		ExtractTerms(expr, obj.terms);
		// Apply sign to newly added terms
		if (sign == -1) {
			for (idx_t i = before; i < obj.terms.size(); i++) {
				obj.terms[i].sign *= -1;
			}
		}
		if (filter) {
			for (idx_t i = before; i < obj.terms.size(); i++) {
				obj.terms[i].filter = filter->Copy();
			}
		}
	}

	void ExtractAggregateObjectiveTerms(const Expression &expr, DecideObjective &obj, int sign) {
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = expr.Cast<BoundCastExpression>();
			if (FindDecideVariable(expr) != DConstants::INVALID_INDEX) {
				ExtractAggregateObjectiveTerms(*cast.child, obj, sign);
			} else {
				ExtractLinearAndBilinearTerms(expr, obj, sign);
			}
			return;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &func = expr.Cast<BoundFunctionExpression>();
			if (func.function.name == "+") {
				for (auto &child : func.children) {
					ExtractAggregateObjectiveTerms(*child, obj, sign);
				}
				return;
			}
			if (func.function.name == "-" && func.children.size() == 2) {
				ExtractAggregateObjectiveTerms(*func.children[0], obj, sign);
				ExtractAggregateObjectiveTerms(*func.children[1], obj, -sign);
				return;
			}
			if (func.function.name == "-" && func.children.size() == 1) {
				ExtractAggregateObjectiveTerms(*func.children[0], obj, -sign);
				return;
			}
		}
		// A factor left outside a reducer (`2 * SUM(x*p)`): extract the reducer, then
		// multiply the factor into everything it produced. Mirrors the constraint
		// side; objectives are not canonicalized, so both spellings arrive as written.
		{
			ScaledAggregateMatch scale_match;
			if (TryMatchScaledAggregate(expr, decide_index, scale_match)) {
				auto obj_scale = scale_match.scale;
				// The canonicalizer vets a factor before it gets here -- but it only runs
				// on CONSTRAINTS, so an objective arrives unvetted and this is the first
				// place that can say no. A decision on both sides of the `*` is a product
				// of two decisions (bilinear), not a scaled reducer; treating it as a
				// coefficient reads a decision column as data and crashes in evaluation.
				if (FindDecideVariable(*obj_scale) != DConstants::INVALID_INDEX) {
					throw InvalidInputException("DECIDE objective: '%s' is a decision, so it cannot multiply an "
					                            "aggregate. Only constants and query-wide values can scale "
					                            "SUM/AVG/MIN/MAX.",
					                            ScaleUserName(*obj_scale));
				}
				idx_t linear_before = obj.terms.size();
				idx_t bilinear_before = obj.bilinear_terms.size();
				idx_t squared_before = obj.squared_terms.size();
				ExtractAggregateObjectiveTerms(*scale_match.aggregate, obj, sign);
				ApplyScaleToObjective(*scale_match.function, *obj_scale, scale_match.divides, obj, linear_before,
				                      bilinear_before, squared_before);
				return;
			}
		}
		// A query-wide decision contributes without a reducer.
		if (IsScalarDecideTerm(expr)) {
			ExtractLinearAndBilinearTerms(expr, obj, sign, nullptr);
			return;
		}
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			throw InvalidInputException(
			    "DECIDE objective contains a non-aggregate term: %s.\n"
			    "The objective must be a SUM/MIN/MAX/AVG of an expression in decision variables.\n"
			    "If you wrapped an aggregate inside another function (e.g. POWER(AVG(x), 2)), "
			    "use the supported shape SUM(POWER(x, 2)) instead.",
			    expr.ToString());
		}

		auto &agg = expr.Cast<BoundAggregateExpression>();
		auto agg_name = StringUtil::Lower(agg.function.name);
		if (agg_name != "sum") {
			throw InvalidInputException(
			    "DECIDE optimizer should rewrite objective aggregate '%s' to SUM before execution",
			    agg.function.name);
		}
		bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
		idx_t qualifier_scope = QualifierScopeOf(agg);

		idx_t before = obj.terms.size();
		idx_t bilinear_before = obj.bilinear_terms.size();
		idx_t squared_before = obj.squared_terms.size();
		ExtractLinearAndBilinearTerms(*agg.children[0], obj, sign, agg.filter.get());
		for (idx_t i = before; i < obj.terms.size(); i++) {
			obj.terms[i].avg_scale = is_avg;
			obj.terms[i].qualifier_scope_idx = qualifier_scope;
		}
		for (idx_t i = bilinear_before; i < obj.bilinear_terms.size(); i++) {
			obj.bilinear_terms[i].avg_scale = is_avg;
			obj.bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
		}
		for (idx_t i = squared_before; i < obj.squared_terms.size(); i++) {
			obj.squared_terms[i].avg_scale = is_avg;
			obj.squared_terms[i].qualifier_scope_idx = qualifier_scope;
		}
	}

	void AnalyzeObjective(const unique_ptr<Expression> &expr_ptr) {
		auto *expr = UnwrapDecideCasts(*expr_ptr, decide_index);

		// DecidB: Check for PER wrapper on objective (outermost layer)
		vector<unique_ptr<Expression>> per_cols;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			if (IsPerConstraintTag(conj.alias) && conj.children.size() >= 2) {
				for (idx_t i = 1; i < conj.children.size(); i++) {
					per_cols.push_back(conj.children[i]->Copy());
				}
				expr = UnwrapDecideCasts(*conj.children[0], decide_index);
			}
		}

		// DecidB: Check for WHEN wrapper on objective (inside PER, if present)
		unique_ptr<Expression> when_cond;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			if (HasDecideTag(conj.alias, WHEN_CONSTRAINT_TAG) && conj.children.size() == 2) {
				when_cond = conj.children[1]->Copy();
				// Unwrap to get the actual objective expression
				expr = UnwrapDecideCasts(*conj.children[0], decide_index);
			}
		}

		auto &objective = op.prepared.objective;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			auto &agg = expr->Cast<BoundAggregateExpression>();

			objective = make_uniq<DecideObjective>();

			// Walk the SUM argument. ExtractLinearAndBilinearTerms recognises
			// quadratic patterns (POWER/(expr)*(expr)/negated/K*POWER) at any
			// position in `+`/`-` trees and routes them into squared_terms, so
			// the same walker handles pure QP, pure linear+bilinear, and the
			// mixed forms (e.g. SUM(POWER(x-t, 2) + penalty*x)) uniformly.
			idx_t before = objective->terms.size();
			idx_t bilinear_before = objective->bilinear_terms.size();
			idx_t squared_before = objective->squared_terms.size();
			ExtractLinearAndBilinearTerms(*agg.children[0], *objective, 1, agg.filter.get());
			bool is_avg = HasDecideTag(agg.alias, AVG_REWRITE_TAG);
			idx_t qualifier_scope = QualifierScopeOf(agg);
			for (idx_t i = before; i < objective->terms.size(); i++) {
				objective->terms[i].avg_scale = is_avg;
				objective->terms[i].qualifier_scope_idx = qualifier_scope;
			}
			for (idx_t i = bilinear_before; i < objective->bilinear_terms.size(); i++) {
				objective->bilinear_terms[i].avg_scale = is_avg;
				objective->bilinear_terms[i].qualifier_scope_idx = qualifier_scope;
			}
			for (idx_t i = squared_before; i < objective->squared_terms.size(); i++) {
				objective->squared_terms[i].avg_scale = is_avg;
				objective->squared_terms[i].qualifier_scope_idx = qualifier_scope;
			}

			objective->when_condition = std::move(when_cond);
			objective->per_columns = std::move(per_cols);
		} else if (BoundExpressionContainsAggregate(*expr) || IsScalarDecideTerm(*expr)) {
			// The second arm covers an objective made only of query-wide decisions
			// (e.g. `minimize max_shortfall`), which carries no aggregate at all.
			objective = make_uniq<DecideObjective>();
			ExtractAggregateObjectiveTerms(*expr, *objective, 1);
			objective->when_condition = std::move(when_cond);
			objective->per_columns = std::move(per_cols);
		}
	}

private:
	ClientContext &context;
	LogicalDecide &op;
	idx_t decide_index;
	column_binding_map_t<idx_t> decide_variable_map;
	DecideCanonicalizer canonicalizer;
};

void BuildDecidePreparedModel(ClientContext &context, LogicalDecide &decide) {
	DecideLinearFormBuilder builder(context, decide);
	builder.Build();
}

} // namespace duckdb
