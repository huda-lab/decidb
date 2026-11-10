#include "duckdb/planner/decide/decide_shape_check.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_product_form.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

DecideShapeCheck::DecideShapeCheck(ClientContext &context, idx_t decide_index, const DecideSourceNames &names)
    : context(context), decide_index(decide_index), names(names) {
}

void DecideShapeCheck::Refuse(const Expression &term, const string &problem, const string &clause) const {
	throw InvalidInputException(term, "%s In %s.", problem, clause);
}

//! An operator as SQL writes it: `COALESCE`, `IS NULL`, `%`; a function as the user wrote it (`mod` for `%`).
static string OperatorName(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		return DecideWrittenName(func.GetAlias(), func.function.name);
	}
	auto name = EnumUtil::ToString(expr.type).substr(strlen("OPERATOR_"));
	return StringUtil::Replace(name, "_", " ");
}

//! What the user called a conditional: `CASE`, or the `if` or `iif` that reaches the binder as the parser's CASE.
static string WrittenConditional(const Expression &expr, const DecideSourceText &text) {
	string written;
	if (!text.Cut(expr.GetQueryLocation(), DecideSpanRole::EXPR, written)) {
		return "CASE";
	}
	idx_t length = 0;
	while (length < written.size() &&
	       (isalnum(static_cast<unsigned char>(written[length])) || written[length] == '_')) {
		length++;
	}
	return length == 0 ? "CASE" : StringUtil::Upper(written.substr(0, length));
}

void DecideShapeCheck::CheckVocabulary(const Expression &expr, const string &clause) const {
	if (!BoundExpressionReferencesDecide(expr, decide_index)) {
		return;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CASE:
		Refuse(expr,
		       StringUtil::Format("%s over a DECIDE variable is not supported. Write the condition as a scope in "
		                          "front of the clause (when category = 'A': sum(x) >= 1), split rows into classes "
		                          "with per, or pre-compute conditional values in a CTE or subquery before the "
		                          "DECIDE clause.",
		                          WrittenConditional(expr, names.text)),
		       clause);
	case ExpressionClass::BOUND_OPERATOR:
		Refuse(expr, StringUtil::Format("DECIDE: operator '%s' cannot take a DECIDE variable.", OperatorName(expr)),
		       clause);
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		auto &name = func.function.name;
		idx_t exponent;
		if (IsDecidePowerCall(func)) {
			if (!TryGetDecideWholeExponent(context, *func.children[1], exponent)) {
				Refuse(expr,
				       StringUtil::Format("POWER over a DECIDE variable needs an exponent that is a whole number known "
				                          "before the query runs, found '%s'.",
				                          RenderDecideSource(*func.children[1], names)),
				       clause);
			}
		} else if (name == "/") {
			if (BoundExpressionReferencesDecide(*func.children[1], decide_index)) {
				Refuse(expr, "DECIDE: cannot divide by a DECIDE variable.", clause);
			}
		} else if (name != "+" && name != "-" && name != "*" && name != "@") {
			// The vocabulary of the model.
			Refuse(expr,
			       StringUtil::Format("DECIDE: %s '%s' cannot take a DECIDE variable.",
			                          func.is_operator ? "operator" : "function", OperatorName(expr)),
			       clause);
		}
		break;
	}
	default:
		break;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { CheckVocabulary(child, clause); });
}

//! The smallest node under `term` whose degree is still `degree`: the product the caret points at.
static const Expression &HighestDegreeNode(ClientContext &context, const Expression &term, idx_t decide_index,
                                           idx_t degree) {
	const Expression *found = nullptr;
	ExpressionIterator::EnumerateChildren(term, [&](const Expression &child) {
		if (!found && DecideDegreeOf(context, child, decide_index).degree == degree) {
			found = &child;
		}
	});
	return found ? HighestDegreeNode(context, *found, decide_index, degree) : term;
}

void DecideShapeCheck::CheckSide(const Expression &side, const string &clause) const {
	CheckVocabulary(side, clause);
	for (auto &atom : ReadCanonicalAtoms(side, decide_index)) {
		auto degree = DecideDegreeOf(context, *atom.term, decide_index).degree;
		if (degree > 2) {
			throw InvalidInputException(HighestDegreeNode(context, *atom.term, decide_index, degree),
			                            "DECIDE: %s has a term of degree %llu, which no solver DeciDB supports can "
			                            "optimize. Instead, write each term with at most two decision factors, as in "
			                            "x, POWER(x, 2) or x * y.",
			                            clause, static_cast<uint64_t>(degree));
		}
	}
}

void DecideShapeCheck::CheckConstraints(const Expression &constraints,
                                        const vector<ConstraintSourceInfo> &sources) const {
	ForEachConstraintLeaf(constraints, [&](const Expression &leaf) {
		// A bound IN marker or a bare BOOL decision is a row with nothing to judge.
		if (leaf.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return;
		}
		// InitializeConstraintSourceInfo tagged every comparison of the clause tree.
		idx_t source_id = DConstants::INVALID_INDEX;
		TryParseSourceClauseTag(leaf.GetAlias(), source_id);
		D_ASSERT(source_id < sources.size());
		auto clause = "the SUCH THAT clause `" + QuoteWrittenClause(sources[source_id]) + "`";
		auto &comparison = leaf.Cast<BoundComparisonExpression>();
		CheckSide(*comparison.left, clause);
		CheckSide(*comparison.right, clause);
	});
}

void DecideShapeCheck::CheckObjective(const Expression &objective, const string &written) const {
	CheckSide(objective, "the objective `" + written + "`");
}

} // namespace duckdb
