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
	throw BinderException(term, "%s In %s.", problem, clause);
}

//! An operator as SQL writes it: `COALESCE`, `IS NULL`, `%`.
static string OperatorName(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		return expr.Cast<BoundFunctionExpression>().function.name;
	}
	auto name = ExpressionTypeToOperator(expr.type);
	if (!name.empty()) {
		return name;
	}
	name = EnumUtil::ToString(expr.type);
	if (StringUtil::StartsWith(name, "OPERATOR_")) {
		name = name.substr(strlen("OPERATOR_"));
	}
	return StringUtil::Replace(name, "_", " ");
}

void DecideShapeCheck::CheckVocabulary(const Expression &expr, const string &clause) const {
	if (!BoundExpressionReferencesDecide(expr, decide_index)) {
		return;
	}
	// A NULL guard the binder wrapped around a decision is not the user's CASE or COALESCE.
	const bool guard = DecideIsGuarded(expr);
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CASE:
		if (!guard) {
			Refuse(expr,
			       "CASE over a DECIDE variable is not supported. Write the condition as a scope in front of the "
			       "clause (when category = 'A': sum(x) >= 1), split rows into classes with per, or pre-compute "
			       "conditional values in a CTE or subquery before the DECIDE clause.",
			       clause);
		}
		break;
	case ExpressionClass::BOUND_OPERATOR:
		// An IN over a decision is a marker the optimizer formulates.
		if (!guard && expr.type != ExpressionType::COMPARE_IN) {
			Refuse(expr, StringUtil::Format("DECIDE: operator '%s' cannot take a DECIDE variable.", OperatorName(expr)),
			       clause);
		}
		break;
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		auto name = StringUtil::Lower(func.function.name);
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
			if (func.children.size() == 2 && BoundExpressionReferencesDecide(*func.children[1], decide_index)) {
				Refuse(expr,
				       "Division by a DECIDE variable is not supported: it would make the model non-linear. The "
				       "divisor must not reference a decision variable.",
				       clause);
			}
		} else if (name != "+" && name != "-" && name != "*" && name != "abs" &&
		           !(StringUtil::StartsWith(name, "__") && StringUtil::EndsWith(name, "__"))) {
			// The vocabulary of the model; DeciDB's own markers are `__`-delimited.
			Refuse(expr,
			       func.is_operator
			           ? StringUtil::Format("DECIDE: operator '%s' cannot take a DECIDE variable.", OperatorName(expr))
			           : StringUtil::Format("Scalar function '%s' over a DECIDE variable is not supported: it would "
			                                "make the model non-linear. Only ABS() and POWER(..., 2) can wrap a "
			                                "decision variable.",
			                                func.function.name),
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
