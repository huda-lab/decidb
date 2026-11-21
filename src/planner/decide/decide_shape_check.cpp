#include "duckdb/planner/decide/decide_shape_check.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_constraint_walk.hpp"
#include "duckdb/planner/decide/decide_product_form.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

DecideShapeCheck::DecideShapeCheck(ClientContext &context, idx_t decide_index, const DecideSourceNames &names,
                                   const vector<DecideFrameSpec> &frames)
    : context(context), decide_index(decide_index), names(names), frames(frames) {
}

void DecideShapeCheck::Refuse(const Expression &term, const string &problem) const {
	throw NotImplementedException(term, "%s", problem);
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

void DecideShapeCheck::CheckVocabulary(const Expression &expr) const {
	if (!BoundExpressionReferencesDecide(expr, decide_index)) {
		return;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CASE:
		Refuse(expr, WrittenConditional(expr, names.text) + " over a decision is not supported");
	case ExpressionClass::BOUND_OPERATOR:
		Refuse(expr, "operator '" + OperatorName(expr) + "' over a decision is not supported");
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		auto &name = func.function.name;
		idx_t exponent;
		if (IsDecidePowerCall(func)) {
			if (!TryGetDecideWholeExponent(context, *func.children[1], exponent)) {
				Refuse(expr, "the exponent '" + RenderDecideSource(*func.children[1], names) +
				                 "' of POWER over a decision is not a constant whole number");
			}
		} else if (name == "/") {
			if (BoundExpressionReferencesDecide(*func.children[1], decide_index)) {
				Refuse(expr, "division by a decision is not supported");
			}
		} else if (name != "+" && name != "-" && name != "*" && name != "@") {
			// The vocabulary of the model.
			Refuse(expr, string(func.is_operator ? "operator" : "function") + " '" + OperatorName(expr) +
			                 "' over a decision is not supported");
		}
		break;
	}
	default:
		break;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { CheckVocabulary(child); });
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

//! The first ABS over a decision under `expr`, or null.
static const Expression *FindAbsOverDecision(const Expression &expr, idx_t decide_index) {
	if (!BoundExpressionReferencesDecide(expr, decide_index)) {
		return nullptr;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	    expr.Cast<BoundFunctionExpression>().function.name == "@") {
		return &expr;
	}
	const Expression *found = nullptr;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = FindAbsOverDecision(child, decide_index);
		}
	});
	return found;
}

//! A frame reads its payload, and its `else`, at another row than the one its term stands on, so each is linear in the
//! decisions: a square or a product there is a product across rows once the frame is written out (D80), and an ABS
//! would lower into rows of its own that stand at the anchor.
void DecideShapeCheck::CheckFramePart(const Expression &part) const {
	if (DecideDegreeOf(context, part, decide_index).degree > 1) {
		throw BinderException(part, "'%s' squares or multiplies decisions inside a frame, which is not allowed",
		                      RenderDecideSource(part, names));
	}
	auto abs = FindAbsOverDecision(part, decide_index);
	if (abs) {
		throw NotImplementedException(*abs,
		                              "'%s' inside a frame is not linear in the decisions, which is not supported",
		                              RenderDecideSource(*abs, names));
	}
}

void DecideShapeCheck::CheckFrames(const Expression &expr) const {
	idx_t frame;
	if (TryGetDecideFrame(expr, frame)) {
		CheckFramePart(*expr.Cast<BoundAggregateExpression>().children[0]);
		if (frames[frame].fallback) {
			CheckFramePart(*frames[frame].fallback);
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { CheckFrames(child); });
}

void DecideShapeCheck::CheckSide(const Expression &side) const {
	CheckVocabulary(side);
	CheckFrames(side);
	for (auto &atom : ReadCanonicalAtoms(side, decide_index)) {
		auto degree = DecideDegreeOf(context, *atom.term, decide_index).degree;
		if (degree > 2) {
			throw NotImplementedException(HighestDegreeNode(context, *atom.term, decide_index, degree),
			                              "a term of degree %llu is not supported", static_cast<uint64_t>(degree));
		}
	}
}

void DecideShapeCheck::CheckClause(const Expression &whole, const vector<const Expression *> &sides) const {
	bool reads_decision = false;
	for (auto side : sides) {
		reads_decision = reads_decision || BoundExpressionReferencesDecide(*side, decide_index);
	}
	if (!reads_decision) {
		throw BinderException(whole, "'%s' reads no decision", RenderDecideSource(whole, names));
	}
	for (auto side : sides) {
		CheckSide(*side);
	}
}

void DecideShapeCheck::CheckConstraints(const Expression &constraints) const {
	ForEachConstraintLeaf(constraints, [&](const Expression &leaf) {
		// A bound IN marker or a bare BOOL decision is a row with nothing to judge.
		if (leaf.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			return;
		}
		auto &comparison = leaf.Cast<BoundComparisonExpression>();
		CheckClause(comparison, {comparison.left.get(), comparison.right.get()});
	});
}

void DecideShapeCheck::CheckObjective(const Expression &objective) const {
	// The objective is the body of its clause: a `when` only chooses the rows.
	const Expression *body = &objective;
	if (body->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = body->Cast<BoundConjunctionExpression>();
		if (IsWhenFilterWrapper(conjunction) && conjunction.children.size() == 2) {
			body = conjunction.children[0].get();
		}
	}
	CheckClause(*body, {body});
}

} // namespace duckdb
