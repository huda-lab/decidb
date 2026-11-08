#include "duckdb/planner/decide/decide_product_form.hpp"

#include "duckdb/common/decide_source_info.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <cmath>

namespace duckdb {

bool IsDecidePowerCall(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	auto name = StringUtil::Lower(func.function.name);
	return func.children.size() == 2 && (name == "pow" || name == "power" || name == "**" || name == "^");
}

bool TryGetDecideWholeExponent(ClientContext &context, const Expression &exponent, idx_t &out) {
	double value;
	if (!TryEvaluateFoldableDouble(context, exponent, value, FOLD_CATCH_ALL) || !std::isfinite(value) || value < 0.0 ||
	    value != std::floor(value) || value > 1e9) {
		return false;
	}
	out = static_cast<idx_t>(value);
	return true;
}

static bool IsFunctionNamed(const Expression &expr, const char *name, idx_t arity) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       expr.Cast<BoundFunctionExpression>().function.name == name &&
	       expr.Cast<BoundFunctionExpression>().children.size() == arity;
}

DecideTermDegree DecideDegreeOf(ClientContext &context, const Expression &expr, idx_t decide_index) {
	DecideTermDegree result;
	if (!BoundExpressionReferencesDecide(expr, decide_index)) {
		return result;
	}
	auto &node = *UnwrapDecideCasts(expr, decide_index);
	if (node.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		result.degree = 1;
		return result;
	}
	auto absorb = [&](const Expression &child, bool add) {
		auto degree = DecideDegreeOf(context, child, decide_index);
		result.degree = add ? result.degree + degree.degree : MaxValue(result.degree, degree.degree);
		result.polynomial = result.polynomial && degree.polynomial;
	};
	if (node.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		// A reducer combines rows without multiplying them: the degree of what it reduces.
		for (auto &child : node.Cast<BoundAggregateExpression>().children) {
			absorb(*child, false);
		}
		return result;
	}
	if (node.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		ExpressionIterator::EnumerateChildren(node, [&](const Expression &child) { absorb(child, false); });
		result.polynomial = false;
		return result;
	}
	auto &func = node.Cast<BoundFunctionExpression>();
	auto name = StringUtil::Lower(func.function.name);
	if (name == "+" || name == "-" || name == "abs") {
		for (auto &child : func.children) {
			absorb(*child, false);
		}
		return result;
	}
	if (name == "*") {
		for (auto &child : func.children) {
			absorb(*child, true);
		}
		return result;
	}
	idx_t exponent;
	if (IsDecidePowerCall(func) && TryGetDecideWholeExponent(context, *func.children[1], exponent)) {
		absorb(*func.children[0], false);
		result.degree *= exponent;
		return result;
	}
	if (name == "/" && func.children.size() == 2 && !BoundExpressionReferencesDecide(*func.children[1], decide_index)) {
		absorb(*func.children[0], false);
		return result;
	}
	for (auto &child : func.children) {
		absorb(*child, false);
	}
	result.polynomial = false;
	return result;
}

//! Equals ignores aliases. On a decision-bearing node they are structure (a reducer's `by` and `per`), so they must
//! agree there; on data they are provenance (where a column was written), which two equal values may differ in. A
//! respelled term's written text is provenance too.
static bool DecisionAliasesEqual(const Expression &left, const Expression &right, idx_t decide_index) {
	if (!BoundExpressionReferencesDecide(left, decide_index)) {
		return true;
	}
	if (StripWrittenTermTag(left.GetAlias()) != StripWrittenTermTag(right.GetAlias())) {
		return false;
	}
	vector<const Expression *> left_children;
	vector<const Expression *> right_children;
	ExpressionIterator::EnumerateChildren(left, [&](const Expression &child) { left_children.push_back(&child); });
	ExpressionIterator::EnumerateChildren(right, [&](const Expression &child) { right_children.push_back(&child); });
	if (left_children.size() != right_children.size()) {
		return false;
	}
	for (idx_t i = 0; i < left_children.size(); i++) {
		if (!DecisionAliasesEqual(*left_children[i], *right_children[i], decide_index)) {
			return false;
		}
	}
	return true;
}

bool DecideTermsEqual(const Expression &left, const Expression &right, idx_t decide_index) {
	return Expression::Equals(left, right) && DecisionAliasesEqual(left, right, decide_index);
}

bool DecideFactorsEqual(const Expression &left, const Expression &right, idx_t decide_index) {
	if (DecideTermsEqual(left, right, decide_index)) {
		return true;
	}
	// An additive form is a multiset of signed terms: `x + y` and `y + x` are one factor.
	auto left_atoms = ReadCanonicalAtoms(left, decide_index);
	auto right_atoms = ReadCanonicalAtoms(right, decide_index);
	if (left_atoms.size() < 2 || left_atoms.size() != right_atoms.size()) {
		return false;
	}
	vector<bool> matched(right_atoms.size(), false);
	for (auto &atom : left_atoms) {
		bool found = false;
		for (idx_t i = 0; i < right_atoms.size() && !found; i++) {
			if (!matched[i] && right_atoms[i].sign == atom.sign &&
			    DecideTermsEqual(*UnwrapDecideCasts(*atom.term, decide_index),
			                     *UnwrapDecideCasts(*right_atoms[i].term, decide_index), decide_index)) {
				matched[i] = true;
				found = true;
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

bool DecideFactorListsEqual(const vector<DecideProductFactor> &left, const vector<DecideProductFactor> &right,
                            idx_t decide_index) {
	if (left.size() != right.size()) {
		return false;
	}
	vector<bool> matched(right.size(), false);
	for (auto &factor : left) {
		bool found = false;
		for (idx_t i = 0; i < right.size() && !found; i++) {
			if (!matched[i] && right[i].power == factor.power &&
			    DecideFactorsEqual(*factor.base, *right[i].base, decide_index)) {
				matched[i] = true;
				found = true;
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

namespace {

struct ProductReader {
	ClientContext &context;
	idx_t decide_index;
	DecideProductForm &form;

	void AddFactor(vector<DecideProductFactor> &into, const Expression &base, idx_t power) {
		into.push_back({&base, power});
	}

	void Read(const Expression &expr, idx_t power) {
		if (!BoundExpressionReferencesDecide(expr, decide_index)) {
			// Data: a factor of the coefficient, as written.
			AddFactor(form.coefficient, expr, power);
			return;
		}
		auto &node = *UnwrapDecideCasts(expr, decide_index);
		if (IsFunctionNamed(node, "*", 2)) {
			for (auto &child : node.Cast<BoundFunctionExpression>().children) {
				Read(*child, power);
			}
			return;
		}
		if (IsFunctionNamed(node, "-", 1)) {
			if (power % 2 == 1) {
				form.sign = -form.sign;
			}
			Read(*node.Cast<BoundFunctionExpression>().children[0], power);
			return;
		}
		idx_t exponent;
		if (IsDecidePowerCall(node) &&
		    TryGetDecideWholeExponent(context, *node.Cast<BoundFunctionExpression>().children[1], exponent)) {
			auto &base = *node.Cast<BoundFunctionExpression>().children[0];
			if (power * exponent != 0 && DecideDegreeOf(context, base, decide_index).degree == 1) {
				// One linear form squared stays whole: POWER(b * x, 2) keeps coefficient 1.
				AddFactor(form.factors, *UnwrapDecideCasts(base, decide_index), power * exponent);
				return;
			}
			Read(base, power * exponent);
			return;
		}
		if (power == 0) {
			// A decision raised to 0 is 1: a decision is never NULL. Data keeps its POWER(w, 0), which SQL makes NULL
			// on a NULL.
			AddZeroDecisions(node);
			return;
		}
		AddFactor(form.factors, node, power);
	}

	//! The decisions of a term raised to 0, which the product keeps at coefficient 0, as the merge keeps `x - x`.
	//! A reducer is one decision-bearing value, so it is kept whole.
	void AddZeroDecisions(const Expression &expr) {
		if (!BoundExpressionReferencesDecide(expr, decide_index)) {
			return;
		}
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF ||
		    expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
			for (auto *seen : form.zero_terms) {
				if (DecideTermsEqual(*seen, expr, decide_index)) {
					return;
				}
			}
			form.zero_terms.push_back(&expr);
			return;
		}
		ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { AddZeroDecisions(child); });
	}
};

} // namespace

DecideProductForm ReadDecideProduct(ClientContext &context, const Expression &expr, idx_t decide_index) {
	DecideProductForm form;
	ProductReader reader {context, decide_index, form};
	reader.Read(expr, 1);
	// Equal decision-bearing factors are one factor raised to the sum of their powers.
	for (idx_t i = 0; i < form.factors.size(); i++) {
		for (idx_t j = i + 1; j < form.factors.size();) {
			if (!DecideFactorsEqual(*form.factors[i].base, *form.factors[j].base, decide_index)) {
				j++;
				continue;
			}
			form.factors[i].power += form.factors[j].power;
			form.factors.erase(form.factors.begin() + static_cast<int64_t>(j));
		}
	}
	return form;
}

static unique_ptr<Expression> Bind(ClientContext &context, const string &name, unique_ptr<Expression> left,
                                   unique_ptr<Expression> right) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(left));
	children.push_back(std::move(right));
	return DecideRebindOperator(context, name, std::move(children));
}

//! A factor as the spelling writes it: its base, or POWER of its base and the whole exponent as its number, however the
//! exponent was written (`CAST(2 AS INTEGER)` is 2).
static unique_ptr<Expression> BuildFactor(ClientContext &context, const DecideProductFactor &factor) {
	if (factor.power == 1) {
		return factor.base->Copy();
	}
	return Bind(context, "power", factor.base->Copy(),
	            make_uniq<BoundConstantExpression>(Value::INTEGER(static_cast<int32_t>(factor.power))));
}

unique_ptr<Expression> BuildDecideProductCoefficient(ClientContext &context, const DecideProductForm &form) {
	unique_ptr<Expression> result;
	for (auto &factor : form.coefficient) {
		auto term = BuildFactor(context, factor);
		result = result ? Bind(context, "*", std::move(result), std::move(term)) : std::move(term);
	}
	return result;
}

//! The form written as a left-deep chain: the coefficient's factors, then the decision-bearing ones.
unique_ptr<Expression> BuildDecideProductForm(ClientContext &context, const DecideProductForm &form) {
	auto result = BuildDecideProductCoefficient(context, form);
	for (auto &factor : form.factors) {
		auto term = BuildFactor(context, factor);
		result = result ? Bind(context, "*", std::move(result), std::move(term)) : std::move(term);
	}
	if (!result) {
		result = make_uniq<BoundConstantExpression>(Value::INTEGER(1));
	}
	if (form.sign < 0) {
		vector<unique_ptr<Expression>> children;
		children.push_back(std::move(result));
		result = DecideRebindOperator(context, "-", std::move(children));
	}
	for (auto *term : form.zero_terms) {
		auto zero = Bind(context, "*", make_uniq<BoundConstantExpression>(Value::INTEGER(0)), term->Copy());
		result = Bind(context, "+", std::move(result), std::move(zero));
	}
	return result;
}

//! A decision-bearing `*`, or a POWER with a whole exponent: the nodes the rule rewrites.
bool IsDecideProductNode(ClientContext &context, const Expression &expr, idx_t decide_index) {
	if (!BoundExpressionReferencesDecide(expr, decide_index)) {
		return false;
	}
	idx_t exponent;
	return IsFunctionNamed(expr, "*", 2) ||
	       (IsDecidePowerCall(expr) &&
	        TryGetDecideWholeExponent(context, *expr.Cast<BoundFunctionExpression>().children[1], exponent));
}

//! The alias of a node rebuilt in place of another. DECIDE tags are appended to an ordinary alias and stripped where a
//! node is printed (StripDecideTags), so the rebuilt node keeps its own ordinary alias (a column's name, when the
//! product is one decision), takes the replaced node's when it has none, and carries every tag the replaced node did.
static string RebuiltAlias(const string &own, const string &replaced) {
	auto replaced_name = StripDecideTags(replaced);
	auto alias = StripDecideTags(own).empty() ? replaced_name + own : own;
	return alias + replaced.substr(replaced_name.size());
}

//! `node` rewritten in the spelling, typed, named and located as `node`; null when it already is. With `written`, the
//! rewritten node also names its text as written, cut at its location.
static unique_ptr<Expression> RewriteProduct(ClientContext &context, idx_t decide_index, const Expression &node,
                                             optional_ptr<DecideWrittenTerms> written) {
	auto form = ReadDecideProduct(context, node, decide_index);
	auto product = BuildDecideProductForm(context, form);
	auto *spelled = product.get();
	auto rebuilt = std::move(product);
	if (rebuilt->return_type != node.return_type) {
		rebuilt = BoundCastExpression::AddCastToType(context, std::move(rebuilt), node.return_type);
	}
	if (Expression::Equals(*rebuilt, node)) {
		return nullptr;
	}
	auto alias = RebuiltAlias(rebuilt->GetAlias(), node.GetAlias());
	// The text goes on the product itself, below the cast back, which a reader looks through.
	idx_t fragment_id;
	string text;
	if (written && !TryParseWrittenTermTag(alias, fragment_id) &&
	    written->text.Cut(node.GetQueryLocation(), DecideSpanRole::TERM, text)) {
		auto tag = MakeWrittenTermTag(written->fragments.size());
		written->fragments.push_back(std::move(text));
		if (spelled == rebuilt.get()) {
			AddDecideTag(alias, tag);
		} else {
			auto spelled_alias = spelled->GetAlias();
			AddDecideTag(spelled_alias, tag);
			spelled->SetAlias(std::move(spelled_alias));
		}
	}
	rebuilt->SetAlias(std::move(alias));
	// The product is the written term respelled, so it points where the term was written, as the cast back does.
	spelled->SetQueryLocation(node.GetQueryLocation());
	rebuilt->SetQueryLocation(node.GetQueryLocation());
	return rebuilt;
}

void NormalizeProductsInPlace(ClientContext &context, idx_t decide_index, unique_ptr<Expression> &expr,
                              optional_ptr<DecideWrittenTerms> written) {
	if (!BoundExpressionReferencesDecide(*expr, decide_index)) {
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { NormalizeProductsInPlace(context, decide_index, child, written); });
	if (!IsDecideProductNode(context, *expr, decide_index)) {
		return;
	}
	auto rebuilt = RewriteProduct(context, decide_index, *expr, written);
	if (rebuilt) {
		expr = std::move(rebuilt);
	}
}

unique_ptr<Expression> NormalizeProducts(ClientContext &context, idx_t decide_index, const Expression &expr,
                                         optional_ptr<DecideWrittenTerms> written) {
	auto result = expr.Copy();
	NormalizeProductsInPlace(context, decide_index, result, written);
	return result;
}

unique_ptr<Expression> BuildDecideProduct(ClientContext &context, idx_t decide_index, unique_ptr<Expression> left,
                                          unique_ptr<Expression> right) {
	auto product = Bind(context, "*", std::move(left), std::move(right));
	auto rebuilt = RewriteProduct(context, decide_index, *product, nullptr);
	return rebuilt ? std::move(rebuilt) : std::move(product);
}

} // namespace duckdb
