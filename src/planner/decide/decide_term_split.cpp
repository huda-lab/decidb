#include "duckdb/planner/decide/decide_term_split.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_binder/decide/decide_degree.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Rebuilding a coefficient
//===--------------------------------------------------------------------===//

//! Rebuilding a `BoundFunctionExpression` by hand — reusing another node's
//! `function` / `return_type` / `bind_info` over different children — does not
//! fail when the types disagree. It reinterprets the children's *physical*
//! representation, which silently yields a wrong number and can read past the
//! end of a narrower vector. DECIDE has hit that failure mode three times, always
//! where a subtree was rebuilt after terms were dropped or distributed. Binding
//! through `FunctionBinder` picks the implementation for these argument types,
//! computes the matching return type and bind data, and inserts whatever casts the
//! signature needs.
unique_ptr<Expression> DecideRebindOperator(ClientContext &context, const string &name,
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
	return DecideRebindOperator(context, "*", std::move(children));
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
	auto result = factors[0]->Copy();
	for (idx_t i = 1; i < factors.size(); i++) {
		result = RebindMultiply(context, std::move(result), factors[i]->Copy());
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Structural helpers
//===--------------------------------------------------------------------===//

static bool IsBoundMultiply(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       expr.Cast<BoundFunctionExpression>().function.name == "*";
}

static bool IsAbsFunction(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       StringUtil::Lower(expr.Cast<BoundFunctionExpression>().function.name) == "abs" &&
	       expr.Cast<BoundFunctionExpression>().children.size() == 1;
}

static void CollectMultiplicativeFactors(const Expression &expr, idx_t decide_index,
                                         vector<const Expression *> &factors) {
	const Expression *cur = UnwrapDecideCasts(expr, decide_index);
	if (IsBoundMultiply(*cur)) {
		for (auto &child : cur->Cast<BoundFunctionExpression>().children) {
			CollectMultiplicativeFactors(*child, decide_index, factors);
		}
		return;
	}
	factors.push_back(cur);
}

//! Distribute multiplication over addition/subtraction: when a `*` chain has an additive (`+` / `-` /
//! unary-`-`) factor, expand into (sign, product) pairs, each a pure `*` chain with the additive factor replaced
//! by one of its addends, so `K * (a - b*x)` becomes `(+1, K*a)` and `(-1, K*b*x)`. Without it product
//! classification would refuse the `(a - b*x)` factor although the term is linear in decisions. Empty when no
//! factor is additive.
static vector<pair<int, unique_ptr<Expression>>>
TryDistributeMultiplyOverAdd(ClientContext &context, const BoundFunctionExpression &mul_expr, idx_t decide_index) {
	vector<pair<int, unique_ptr<Expression>>> out;
	vector<const Expression *> factors;
	CollectMultiplicativeFactors(mul_expr, decide_index, factors);

	int additive_idx = -1;
	for (idx_t i = 0; i < factors.size(); i++) {
		const Expression *f = factors[i];
		if (f->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			continue;
		}
		auto &ff = f->Cast<BoundFunctionExpression>();
		if (ff.function.name == "+" && ff.children.size() >= 1) {
			additive_idx = (int)i;
			break;
		}
		if (ff.function.name == "-" && (ff.children.size() == 1 || ff.children.size() == 2)) {
			additive_idx = (int)i;
			break;
		}
	}
	if (additive_idx < 0) {
		return out;
	}

	auto &add_func = factors[additive_idx]->Cast<BoundFunctionExpression>();
	vector<pair<int, const Expression *>> addends;
	if (add_func.function.name == "+") {
		for (auto &c : add_func.children) {
			addends.push_back({1, c.get()});
		}
	} else if (add_func.children.size() == 2) {
		addends.push_back({1, add_func.children[0].get()});
		addends.push_back({-1, add_func.children[1].get()});
	} else {
		addends.push_back({-1, add_func.children[0].get()});
	}

	for (auto &kv : addends) {
		vector<const Expression *> new_factors;
		for (idx_t j = 0; j < factors.size(); j++) {
			if ((int)j != additive_idx) {
				new_factors.push_back(factors[j]);
			}
		}
		new_factors.push_back(kv.second);
		out.push_back({kv.first, BuildCoefficientFromFactors(context, new_factors)});
	}
	return out;
}

static DecideSplitTerm UnknownTerm(int sign, string reason, bool user_error) {
	DecideSplitTerm term;
	term.kind = DecideTermKind::UNKNOWN;
	term.sign = sign;
	term.reason = std::move(reason);
	term.user_error = user_error;
	return term;
}

//! A product factor that holds a decision without being one, such as `ABS(x) * y`. Degree 2 is legal at bind
//! time; the refusal is a formulation limit, so it is the user's to fix.
static DecideSplitTerm UnsupportedProductFactor() {
	return UnknownTerm(1,
	                   "DECIDE expression contains an unsupported product factor that still "
	                   "references decision variables after normalization (total degree > 2 "
	                   "or unexpanded nonlinear product). Products must be data factors times "
	                   "one DECIDE variable, or data factors times two different DECIDE variables.",
	                   true);
}

static void FlipSigns(vector<DecideSplitTerm> &terms, idx_t begin) {
	for (idx_t i = begin; i < terms.size(); i++) {
		terms[i].sign *= -1;
	}
}

//===--------------------------------------------------------------------===//
// DecideTermSplitter
//===--------------------------------------------------------------------===//

DecideTermSplitter::DecideTermSplitter(ClientContext &context, idx_t decide_index,
                                       const vector<unique_ptr<Expression>> &decide_variables)
    : context(context), decide_index(decide_index), decide_variables(decide_variables) {
	for (idx_t i = 0; i < decide_variables.size(); i++) {
		variable_indexes[decide_variables[i]->Cast<BoundColumnRefExpression>().binding] = i;
	}
}

idx_t DecideTermSplitter::FindVariable(const Expression &expr) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto entry = variable_indexes.find(expr.Cast<BoundColumnRefExpression>().binding);
		if (entry != variable_indexes.end()) {
			return entry->second;
		}
	}
	idx_t result = DConstants::INVALID_INDEX;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (result == DConstants::INVALID_INDEX) {
			result = FindVariable(child);
		}
	});
	return result;
}

bool DecideTermSplitter::ContainsVariable(const Expression &expr, idx_t var_idx) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &variable = decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
		return expr.Cast<BoundColumnRefExpression>().binding == variable.binding;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		found = found || ContainsVariable(child, var_idx);
	});
	return found;
}

bool DecideTermSplitter::TryGetBareDecideFactor(const Expression &expr, idx_t &var_idx) const {
	auto *colref = GetBareDecideColumnRef(expr, decide_index);
	if (!colref) {
		return false;
	}
	var_idx = FindVariable(*colref);
	return var_idx != DConstants::INVALID_INDEX;
}

//! Layer 2 decides degree on the bound tree before any rewrite runs; this only checks that what reaches term
//! splitting is still what layer 2 admitted, because rewrites (the IN expansion, ABS linearization, the norm
//! lowering) synthesize expressions layer 2 never saw.
bool DecideTermSplitter::IsLinear(const Expression &expr) const {
	return DecideExpressionDegree(expr, decide_index).IsLinear();
}

bool DecideTermSplitter::ClassifyProduct(const Expression &expr, ProductFactors &result,
                                         DecideSplitTerm &failure) const {
	result = ProductFactors();
	vector<const Expression *> factors;
	CollectMultiplicativeFactors(*UnwrapDecideCasts(expr, decide_index), decide_index, factors);
	for (auto *factor : factors) {
		idx_t var_idx = DConstants::INVALID_INDEX;
		if (TryGetBareDecideFactor(*factor, var_idx)) {
			result.decide_factors.push_back(var_idx);
			continue;
		}
		if (FindVariable(*factor) == DConstants::INVALID_INDEX) {
			result.coefficient_factors.push_back(factor);
			continue;
		}
		if (IsAbsFunction(*factor) && !result.abs_factor) {
			result.abs_factor = factor;
			continue;
		}
		failure = UnsupportedProductFactor();
		return false;
	}
	if (result.abs_factor && !result.decide_factors.empty()) {
		failure = UnsupportedProductFactor();
		return false;
	}
	if (result.decide_factors.size() > 2) {
		// Degree, so layer 2's judgement, and it already refused this at bind time with a located message.
		// Reaching it here means a rewrite built the product.
		failure = UnknownTerm(1,
		                      StringUtil::Format("DECIDE product reached term extraction with %llu decision factors. "
		                                         "Layer 2 refuses total degree > 2 at bind time, so this tree was "
		                                         "produced by a rewrite in this layer rather than written by a user.",
		                                         static_cast<uint64_t>(result.decide_factors.size())),
		                      false);
		return false;
	}
	if (result.decide_factors.size() == 2 && result.decide_factors[0] == result.decide_factors[1]) {
		failure = UnknownTerm(1,
		                      "DECIDE expression contains a same-variable product that is not in a supported "
		                      "quadratic form. Use POWER(linear_expr, 2) or (linear_expr) * (linear_expr) "
		                      "for quadratic terms.",
		                      true);
		return false;
	}
	return true;
}

unique_ptr<Expression> DecideTermSplitter::ExtractCoefficientWithoutVariable(const Expression &expr,
                                                                             idx_t var_idx) const {
	// If this IS the variable itself, return constant 1
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		auto &decide_var = decide_variables[var_idx]->Cast<BoundColumnRefExpression>();
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
				// Child contains the variable: recurse to keep its non-variable scalar/data factors — `(2*x)`
				// yields `2`, a bare `x` yields `1`. Dropping the whole child silently lost nested coefficients
				// like the `2` in `(2*x)*v`, which reaches here un-normalized on the composed MIN/MAX path.
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
			// Rebind for the children that remain: dropping the variable also drops the casts above it, so a
			// child can come back narrower than the original signature expects.
			auto result = std::move(filtered_children[0]);
			for (idx_t i = 1; i < filtered_children.size(); i++) {
				result = RebindMultiply(context, std::move(result), std::move(filtered_children[i]));
			}
			return result;
		}
	}

	// A decision-free cast is part of the coefficient; a remaining decision cast wraps the variable.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		if (FindVariable(expr) == DConstants::INVALID_INDEX) {
			return expr.Copy();
		}
		return ExtractCoefficientWithoutVariable(*expr.Cast<BoundCastExpression>().child, var_idx);
	}
	return expr.Copy();
}

DecideTermSplitter::QuadraticPattern DecideTermSplitter::DetectQuadraticPattern(const Expression &expr) const {
	// Look through binder-generated wrappers over the decision-bearing expression.
	const Expression *cur = UnwrapDecideCasts(expr, decide_index);
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return {};
	}
	auto &func = cur->Cast<BoundFunctionExpression>();
	string fname = StringUtil::Lower(func.function.name);

	// Nothing below can match other names. Without this gate every additive node would pay for the
	// self-product comparison, which is O(subtree size) and makes the walk quadratic on deep sums.
	if (fname != "-" && fname != "*" && fname != "power" && fname != "pow" && fname != "**") {
		return {};
	}

	// -(quadratic)
	if (fname == "-" && func.children.size() == 1) {
		auto inner = DetectQuadraticPattern(*func.children[0]);
		if (inner.inner_linear_expr) {
			inner.sign = -inner.sign;
			return inner;
		}
	}

	// K * quadratic or quadratic * K (constant on either side)
	if (fname == "*" && func.children.size() == 2) {
		for (idx_t side = 0; side < 2; side++) {
			double cval;
			if (TryEvaluateFoldableDouble(context, *func.children[side], cval) && cval != 0.0) {
				auto inner = DetectQuadraticPattern(*func.children[1 - side]);
				if (inner.inner_linear_expr) {
					inner.sign *= cval;
					return inner;
				}
			}
		}
	}

	// POWER / POW / **  with literal exponent 2
	if ((fname == "power" || fname == "pow" || fname == "**") && func.children.size() == 2) {
		double exponent;
		if (TryEvaluateFoldableDouble(context, *func.children[1], exponent) && exponent == 2.0) {
			const Expression *inner = UnwrapDecideCasts(*func.children[0], decide_index);
			if (FindVariable(*inner) != DConstants::INVALID_INDEX) {
				return {inner, 1.0, IsLinear(*inner) ? nullptr : "POWER(..., 2)"};
			}
		}
	}

	// (expr) * (expr) with identical children containing a DECIDE variable
	if (fname == "*" && func.children.size() == 2 && Expression::Equals(*func.children[0], *func.children[1]) &&
	    FindVariable(*func.children[0]) != DConstants::INVALID_INDEX) {
		const Expression *inner = UnwrapDecideCasts(*func.children[0], decide_index);
		return {inner, 1.0, IsLinear(*inner) ? nullptr : "self-product (expr) * (expr)"};
	}
	return {};
}

void DecideTermSplitter::AddAbs(const Expression &abs_function, unique_ptr<Expression> coefficient, int sign,
                                vector<DecideSplitTerm> &out) const {
	DecideSplitTerm term;
	term.kind = DecideTermKind::ABS;
	term.sign = sign;
	term.coefficient = std::move(coefficient);
	SplitLinear(*UnwrapDecideCasts(*abs_function.Cast<BoundFunctionExpression>().children[0], decide_index),
	            term.inner);
	out.push_back(std::move(term));
}

void DecideTermSplitter::Split(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		auto &fname = func.function.name;
		if (fname == "+") {
			for (auto &child : func.children) {
				Split(*child, sign, out);
			}
			return;
		}
		if (fname == "-" && func.children.size() == 2) {
			Split(*func.children[0], sign, out);
			Split(*func.children[1], -sign, out);
			return;
		}
		if (fname == "-" && func.children.size() == 1) {
			Split(*func.children[0], -sign, out);
			return;
		}
		// POWER(e,2) / POW / ** / (e)*(e), on its own or scaled by a constant, which the detector folds into
		// the pattern's sign.
		auto quadratic = DetectQuadraticPattern(expr);
		if (quadratic.inner_linear_expr) {
			if (quadratic.nonlinear_shape) {
				out.push_back(UnknownTerm(sign,
				                          StringUtil::Format("DECIDE %s reached term extraction with a non-linear "
				                                             "inner expression. Layer 2 refuses total degree > 2 "
				                                             "at bind time, so this tree was produced by a "
				                                             "rewrite in this layer rather than written by a user.",
				                                             quadratic.nonlinear_shape),
				                          false));
				return;
			}
			DecideSplitTerm term;
			term.kind = DecideTermKind::SQUARE;
			term.sign = sign;
			term.square_scale = quadratic.sign;
			SplitLinear(*quadratic.inner_linear_expr, term.inner);
			out.push_back(std::move(term));
			return;
		}
		if (IsAbsFunction(func) && FindVariable(func) != DConstants::INVALID_INDEX) {
			AddAbs(func, nullptr, sign, out);
			return;
		}
		if (fname == "*") {
			// Distribute before classifying: `K * (1 - pick)` reaches here from the MIN/MAX hard-direction
			// rewrites and other paths that leave an additive factor.
			auto distributed = TryDistributeMultiplyOverAdd(context, func, decide_index);
			if (!distributed.empty()) {
				for (auto &kv : distributed) {
					Split(*kv.second, sign * kv.first, out);
				}
				return;
			}
			ProductFactors product;
			DecideSplitTerm failure;
			if (!ClassifyProduct(func, product, failure)) {
				failure.sign = sign;
				out.push_back(std::move(failure));
				return;
			}
			if (product.abs_factor) {
				AddAbs(*product.abs_factor, BuildCoefficientFromFactors(context, product.coefficient_factors), sign,
				       out);
				return;
			}
			if (product.decide_factors.size() == 2) {
				DecideSplitTerm term;
				term.kind = DecideTermKind::PRODUCT;
				term.sign = sign;
				term.var_a = product.decide_factors[0];
				term.var_b = product.decide_factors[1];
				term.coefficient = BuildCoefficientFromFactors(context, product.coefficient_factors);
				out.push_back(std::move(term));
				return;
			}
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST && FindVariable(expr) != DConstants::INVALID_INDEX) {
		// Decision-bearing casts surviving binding are DuckDB's internal type-reconciliation wrappers; explicit
		// source casts were rejected on the parsed tree. A data cast stays whole as a typed constant term.
		Split(*expr.Cast<BoundCastExpression>().child, sign, out);
		return;
	}
	auto before = out.size();
	SplitLinear(expr, out);
	if (sign == -1) {
		FlipSigns(out, before);
	}
}

void DecideTermSplitter::SplitLinear(const Expression &expr, vector<DecideSplitTerm> &out) const {
	auto constant = [&](const Expression &term) {
		DecideSplitTerm result;
		result.kind = DecideTermKind::CONSTANT;
		result.coefficient = term.Copy();
		out.push_back(std::move(result));
	};
	auto linear = [&](idx_t var_idx, unique_ptr<Expression> coefficient) {
		DecideSplitTerm result;
		result.kind = DecideTermKind::LINEAR;
		result.var_a = var_idx;
		result.coefficient = std::move(coefficient);
		out.push_back(std::move(result));
	};
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.function.name == "+") {
			for (auto &child : func.children) {
				SplitLinear(*child, out);
			}
			return;
		}
		if (func.function.name == "-" && func.children.size() == 2) {
			SplitLinear(*func.children[0], out);
			auto before = out.size();
			SplitLinear(*func.children[1], out);
			FlipSigns(out, before);
			return;
		}
		if (func.function.name == "-" && func.children.size() == 1) {
			auto before = out.size();
			SplitLinear(*func.children[0], out);
			FlipSigns(out, before);
			return;
		}
		if (func.function.name == "*") {
			// An additive factor (`K * (1 - pick)`) is distributed first so each product is `coef * var`-shaped;
			// otherwise the coefficient would silently drop the additive structure.
			auto distributed = TryDistributeMultiplyOverAdd(context, func, decide_index);
			if (!distributed.empty()) {
				for (auto &kv : distributed) {
					auto before = out.size();
					SplitLinear(*kv.second, out);
					if (kv.first == -1) {
						FlipSigns(out, before);
					}
				}
				return;
			}
			idx_t var_idx = FindVariable(func);
			if (var_idx == DConstants::INVALID_INDEX) {
				constant(func);
				return;
			}
			// Linear means exactly one factor is a decision, and it is bare.
			vector<const Expression *> factors;
			CollectMultiplicativeFactors(func, decide_index, factors);
			idx_t decision_factors = 0;
			bool bare = true;
			for (auto *factor : factors) {
				if (FindVariable(*factor) == DConstants::INVALID_INDEX) {
					continue;
				}
				decision_factors++;
				idx_t ignored;
				bare = bare && TryGetBareDecideFactor(*factor, ignored);
			}
			if (decision_factors != 1 || !bare) {
				out.push_back(UnknownTerm(1,
				                          StringUtil::Format("DECIDE linear term '%s' is not a data coefficient "
				                                             "times one decision variable",
				                                             func.ToString()),
				                          false));
				return;
			}
			linear(var_idx, ExtractCoefficientWithoutVariable(func, var_idx));
			return;
		}
		// Division by a decision-free expression: split the numerator and divide every coefficient. Each side is
		// cast to the `/` function's argument types so an integer coefficient does not turn into integer
		// division (`x/2` gave 0 when the coefficient was INT 1), then bound for the operands it has.
		if (func.function.name == "/" && func.children.size() == 2 &&
		    FindVariable(*func.children[1]) == DConstants::INVALID_INDEX) {
			auto before = out.size();
			SplitLinear(*func.children[0], out);
			D_ASSERT(func.function.arguments.size() == 2);
			const auto &num_type = func.function.arguments[0];
			const auto &denom_type = func.function.arguments[1];
			for (idx_t i = before; i < out.size(); i++) {
				if (!out[i].coefficient) {
					continue;
				}
				vector<unique_ptr<Expression>> div_children;
				div_children.push_back(
				    BoundCastExpression::AddDefaultCastToType(std::move(out[i].coefficient), num_type));
				div_children.push_back(BoundCastExpression::AddDefaultCastToType(func.children[1]->Copy(), denom_type));
				out[i].coefficient = DecideRebindOperator(context, "/", std::move(div_children));
			}
			return;
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		if (FindVariable(expr) == DConstants::INVALID_INDEX) {
			// A decision-free cast is a real DuckDB value operation and belongs in the term exactly as written.
			constant(expr);
		} else {
			SplitLinear(*expr.Cast<BoundCastExpression>().child, out);
		}
		return;
	}
	idx_t var_idx = FindVariable(expr);
	if (var_idx == DConstants::INVALID_INDEX) {
		constant(expr);
		return;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		out.push_back(UnknownTerm(1,
		                          StringUtil::Format("DECIDE term '%s' holds a decision variable under an "
		                                             "expression that is not linear algebra",
		                                             expr.ToString()),
		                          false));
		return;
	}
	linear(var_idx, make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1)));
}

} // namespace duckdb
