#include "duckdb/planner/decide/decide_term_split.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/decide/decide_cast_policy.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/decide/decide_product_form.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

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
                                            vector<unique_ptr<Expression>> children, const string &written_name) {
	FunctionBinder function_binder(context);
	ErrorData error;
	auto result = function_binder.BindScalarFunction(DEFAULT_SCHEMA, name, std::move(children), error);
	if (error.HasError()) {
		throw InternalException("DECIDE failed to bind '%s': %s", name, error.Message());
	}
	if (!written_name.empty()) {
		auto tags = result->GetAlias();
		AddDecideTag(tags, string(WRITTEN_NAME_TAG_PREFIX) + written_name + "__");
		result->SetAlias(std::move(tags));
	}
	return result;
}

unique_ptr<Expression> DecideMergeLikeCoefficients(ClientContext &context, int first_sign, unique_ptr<Expression> first,
                                                   int next_sign, unique_ptr<Expression> next) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(first));
	children.push_back(std::move(next));
	return DecideRebindOperator(context, first_sign == next_sign ? "+" : "-", std::move(children));
}

static unique_ptr<Expression> RebindMultiply(ClientContext &context, unique_ptr<Expression> lhs,
                                             unique_ptr<Expression> rhs) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(lhs));
	children.push_back(std::move(rhs));
	return DecideRebindOperator(context, "*", std::move(children));
}

//===--------------------------------------------------------------------===//
// Structural helpers
//===--------------------------------------------------------------------===//

static bool IsAbsFunction(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       expr.Cast<BoundFunctionExpression>().function.name == "@" &&
	       expr.Cast<BoundFunctionExpression>().children.size() == 1;
}

bool DecideSplitTerm::IsAbs() const {
	return opaque && IsAbsFunction(*opaque);
}

//! A function the optimizer gave a helper decision of its own: an ABS (ABS_FUNCTION_TAG_PREFIX) or a definition
//! (DEFINITION_FUNCTION_TAG_PREFIX). It reads as that decision.
static bool TryGetFunctionAuxIndex(const Expression &expr, idx_t &var_idx) {
	return (IsAbsFunction(expr) && TryParseDecideIndexTag(expr.GetAlias(), ABS_FUNCTION_TAG_PREFIX, var_idx)) ||
	       TryParseDecideIndexTag(expr.GetAlias(), DEFINITION_FUNCTION_TAG_PREFIX, var_idx);
}

//! `left * right` for two coefficients, `right` null for 1. A bare decision's literal 1 stays out of the product, so
//! `w * x` reads `w`: a product of a coefficient and 1 could overflow where the coefficient cannot.
static unique_ptr<Expression> MultiplyCoefficients(ClientContext &context, const Expression &left,
                                                   unique_ptr<Expression> right) {
	if (!right || (right->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
	               right->Cast<BoundConstantExpression>().value == Value::INTEGER(1))) {
		return left.Copy();
	}
	return RebindMultiply(context, left.Copy(), std::move(right));
}

//===--------------------------------------------------------------------===//
// DecideTermSplitter
//===--------------------------------------------------------------------===//

DecideTermSplitter::DecideTermSplitter(ClientContext &context, const LogicalDecide &decide)
    : context(context), decide_index(decide.decide_index), decide_variables(decide.decide_variables),
      bilinear_groups(decide.bilinear_function_groups) {
	for (idx_t i = 0; i < decide_variables.size(); i++) {
		variable_indexes[decide_variables[i]->Cast<BoundColumnRefExpression>().binding] = i;
	}
}

bool DecideTermSplitter::ExpandBilinearGroups(const Expression &expr, unique_ptr<Expression> &expanded) const {
	idx_t definition_aux;
	if (TryParseDecideIndexTag(expr.GetAlias(), DEFINITION_FUNCTION_TAG_PREFIX, definition_aux)) {
		if (definition_aux >= decide_variables.size()) {
			throw InternalException("DECIDE term splitter: definition function references invalid helper %llu",
			                        definition_aux);
		}
		return false;
	}
	idx_t group_idx;
	if (TryParseDecideIndexTag(expr.GetAlias(), BILINEAR_FUNCTION_GROUP_TAG_PREFIX, group_idx)) {
		if (group_idx >= bilinear_groups.size() || !bilinear_groups[group_idx].expansion) {
			throw InternalException("DECIDE term splitter: bilinear function group %llu is missing", group_idx);
		}
		expanded = bilinear_groups[group_idx].expansion->Copy();
		unique_ptr<Expression> nested;
		if (ExpandBilinearGroups(*expanded, nested)) {
			expanded = std::move(nested);
		}
		return true;
	}

	vector<const Expression *> originals;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { originals.push_back(&child); });
	if (originals.empty()) {
		return false;
	}
	auto copy = expr.Copy();
	idx_t next = 0;
	bool changed = false;
	ExpressionIterator::EnumerateChildren(*copy, [&](unique_ptr<Expression> &child) {
		unique_ptr<Expression> child_expanded;
		if (ExpandBilinearGroups(*originals[next], child_expanded)) {
			child = std::move(child_expanded);
			changed = true;
		}
		next++;
	});
	if (changed) {
		expanded = std::move(copy);
	}
	return changed;
}

bool DecideTermSplitter::IsLinear(const Expression &expr) const {
	unique_ptr<Expression> lowered;
	if (!ExpandBilinearGroups(expr, lowered)) {
		lowered = expr.Copy();
	}
	ReplaceTaggedFunctions(lowered);
	return DecideDegreeOf(context, *lowered, decide_index).IsLinear();
}

void DecideTermSplitter::ReplaceTaggedFunctions(unique_ptr<Expression> &expression) const {
	if (!expression) {
		return;
	}
	idx_t aux_idx;
	if (TryGetFunctionAuxIndex(*expression, aux_idx)) {
		if (aux_idx >= decide_variables.size()) {
			throw InternalException("DECIDE term splitter: function references invalid helper %llu", aux_idx);
		}
		const auto type = expression->return_type;
		expression = BoundCastExpression::AddCastToType(context, decide_variables[aux_idx]->Copy(), type);
		return;
	}
	ExpressionIterator::EnumerateChildren(*expression,
	                                      [&](unique_ptr<Expression> &child) { ReplaceTaggedFunctions(child); });
}

idx_t DecideTermSplitter::FindVariable(const Expression &expr) const {
	idx_t aux_idx;
	if (TryGetFunctionAuxIndex(expr, aux_idx)) {
		return aux_idx;
	}
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

void DecideTermSplitter::AddOpaque(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const {
	DecideSplitTerm term;
	term.sign = sign;
	term.degree = DecideDegreeOf(context, expr, decide_index).degree;
	term.opaque = expr.Copy();
	if (IsAbsFunction(expr)) {
		Split(*expr.Cast<BoundFunctionExpression>().children[0], 1, term.inner);
	}
	out.push_back(std::move(term));
}

//! A product in the one spelling: its coefficient, then its decision factors. A coefficient on one factor is
//! distributed over that factor's terms; two factors, or one squared, are a term of degree 2.
void DecideTermSplitter::ReadProduct(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const {
	auto form = ReadDecideProduct(context, expr, decide_index);
	auto coefficient = BuildDecideProductCoefficient(context, form);
	sign *= form.sign;
	if (form.factors.size() == 1 && form.factors[0].power == 1) {
		// A product of one factor carries a coefficient: canonicalization writes the factor alone otherwise.
		if (!coefficient) {
			throw InternalException("DECIDE: a product of one factor with no coefficient reached the term reader; "
			                        "canonicalization writes the factor alone, so a rewrite built it");
		}
		auto begin = out.size();
		Split(*form.factors[0].base, sign, out);
		for (idx_t i = begin; i < out.size(); i++) {
			out[i].coefficient = MultiplyCoefficients(context, *coefficient, std::move(out[i].coefficient));
		}
		return;
	}
	idx_t degree = 0;
	for (auto &factor : form.factors) {
		degree += factor.power * DecideDegreeOf(context, *factor.base, decide_index).degree;
	}
	if (degree != 2) {
		throw InternalException("DECIDE: a term of degree %llu reached the term reader; the shape check refuses a "
		                        "degree above 2, so a rewrite built it",
		                        static_cast<uint64_t>(degree));
	}
	DecideSplitTerm term;
	term.sign = sign;
	term.coefficient = std::move(coefficient);
	term.degree = 2;
	for (auto &factor : form.factors) {
		DecideSplitFactor read;
		read.power = factor.power;
		Split(*factor.base, 1, read.form);
		term.factors.push_back(std::move(read));
	}
	out.push_back(std::move(term));
}

//! Division by a decision-free expression: read the numerator and divide every coefficient. Each side is cast to the
//! `/` function's argument types so an integer coefficient does not turn into integer division (`x/2` gave 0 when the
//! coefficient was INT 1), then bound for the operands it has.
void DecideTermSplitter::ReadQuotient(const BoundFunctionExpression &quotient, int sign,
                                      vector<DecideSplitTerm> &out) const {
	auto begin = out.size();
	Split(*quotient.children[0], sign, out);
	D_ASSERT(quotient.function.arguments.size() == 2);
	const auto &num_type = quotient.function.arguments[0];
	const auto &denom_type = quotient.function.arguments[1];
	for (idx_t i = begin; i < out.size(); i++) {
		auto numerator = out[i].coefficient ? std::move(out[i].coefficient)
		                                    : make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
		vector<unique_ptr<Expression>> children;
		children.push_back(BoundCastExpression::AddDefaultCastToType(std::move(numerator), num_type));
		children.push_back(BoundCastExpression::AddDefaultCastToType(quotient.children[1]->Copy(), denom_type));
		out[i].coefficient = DecideRebindOperator(context, "/", std::move(children));
	}
}

void DecideTermSplitter::Split(const Expression &expr, int sign, vector<DecideSplitTerm> &out) const {
	if (FindVariable(expr) == DConstants::INVALID_INDEX) {
		// A decision-free term, a decision-free cast included: a real DuckDB value operation, kept as written.
		DecideSplitTerm term;
		term.sign = sign;
		term.coefficient = expr.Copy();
		out.push_back(std::move(term));
		return;
	}
	idx_t aux_idx;
	if (TryGetFunctionAuxIndex(expr, aux_idx)) {
		if (aux_idx >= decide_variables.size()) {
			throw InternalException("DECIDE term splitter: function references invalid helper %llu", aux_idx);
		}
		DecideSplitTerm term;
		term.sign = sign;
		term.degree = 1;
		term.var_a = aux_idx;
		term.coefficient = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
		out.push_back(std::move(term));
		return;
	}
	unique_ptr<Expression> expanded;
	if (ExpandBilinearGroups(expr, expanded)) {
		Split(*expanded, sign, out);
		return;
	}
	idx_t frame;
	if (TryGetDecideFrame(expr, frame) && expr.Cast<BoundAggregateExpression>().children.size() == 1) {
		// A frame selector over decisions: an opaque term whose payload's terms are read at the selected row.
		DecideSplitTerm term;
		term.sign = sign;
		term.degree = 1;
		term.frame = frame;
		term.marker = &expr;
		term.opaque = expr.Copy();
		term.coefficient = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
		Split(*expr.Cast<BoundAggregateExpression>().children[0], 1, term.inner);
		out.push_back(std::move(term));
		return;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF: {
		DecideSplitTerm term;
		term.sign = sign;
		term.degree = 1;
		term.var_a = FindVariable(expr);
		term.coefficient = make_uniq_base<Expression, BoundConstantExpression>(Value::INTEGER(1));
		out.push_back(std::move(term));
		return;
	}
	case ExpressionClass::BOUND_CAST:
		// A decision-bearing cast is DuckDB's type reconciliation; a cast the user wrote was refused at binding.
		Split(*expr.Cast<BoundCastExpression>().child, sign, out);
		return;
	case ExpressionClass::BOUND_FUNCTION:
		break;
	default:
		AddOpaque(expr, sign, out);
		return;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	auto &name = func.function.name;
	if (name == "+") {
		for (auto &child : func.children) {
			Split(*child, sign, out);
		}
	} else if (name == "-" && func.children.size() == 2) {
		Split(*func.children[0], sign, out);
		Split(*func.children[1], -sign, out);
	} else if (name == "-") {
		Split(*func.children[0], -sign, out);
	} else if (IsDecideProductNode(context, func, decide_index)) {
		ReadProduct(func, sign, out);
	} else if (name == "/" && FindVariable(*func.children[1]) == DConstants::INVALID_INDEX) {
		ReadQuotient(func, sign, out);
	} else {
		AddOpaque(func, sign, out);
	}
}

} // namespace duckdb
