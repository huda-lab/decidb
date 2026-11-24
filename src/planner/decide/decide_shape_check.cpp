#include "duckdb/planner/decide/decide_shape_check.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_product_form.hpp"
#include "duckdb/planner/decide/decide_reducer_path.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/common/exception/binder_exception.hpp"

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
		return DecideWrittenName(func);
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
		CheckFramePart(*expr.Cast<BoundDecideFrameExpression>().children[0]);
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

//===--------------------------------------------------------------------===//
// Integrality of a compared side, from declared types
//===--------------------------------------------------------------------===//

string FractionalComparisonMessage(const string &comparison, const string &operand) {
	return StringUtil::Format("'%s' compares '%s', which can take fractional values", comparison, operand);
}

//! Does this type guarantee whole-numbered values?
//!
//! `DECIMAL` carries its scale in the type, so `DECIMAL(15, 0)` qualifies and `DECIMAL(15, 2)` does not, whatever its
//! rows hold. No floating type qualifies: `DOUBLE` can hold `3.0`, but nothing in the type says it must, and a rule
//! that read the stored values would make a query's validity change when a row is inserted.
static bool DecideTypeIsWholeNumbered(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
		return true;
	case LogicalTypeId::DECIMAL:
		return DecimalType::GetScale(type) == 0;
	default:
		return false;
	}
}

//! Is this bound node the inert `norm(e, 0, M)` marker? The L0 "norm" counts nonzeros, so its value is a count whatever
//! `e` is typed as.
static bool IsBoundL0NormMarker(const Expression &expr) {
	DecideNorm norm;
	return IsDecideNormMarker(expr, norm) && norm.IsCount();
}

//! Is this bound node an AVG aggregate? A range frame's marker is a value of the row, which no rewrite multiplies
//! through.
static bool IsBoundAvgAggregate(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_DECIDE_REDUCER) {
		return false;
	}
	return expr.Cast<BoundDecideReducerExpression>().kind == DecideReducerKind::AVG;
}

//! Is `expr` provably whole-numbered from declared types alone?
//!
//! `allow_avg_hoist` is set only for `<>`, whose rewrite multiplies both sides by the group size and so compares
//! `SUM(e)` against `K*n`: integral whenever `e` is. A strict comparison has no such hoist and keeps AVG's fractional
//! 1/n coefficients.
//!
//! `addends_count` is set where a decision-free addend is part of the value rather than of the bound: inside a MIN or
//! MAX (canonicalization has already moved the constant addends beside it), and under anything that is not linear in
//! it: a power, a product of decisions, a norm of order 1, 2 or 'inf', and a decision-free factor of a product.
static bool DecideSideIsWholeNumbered(const Expression &expr, bool allow_avg_hoist, idx_t decide_index,
                                      bool addends_count = false) {
	if (IsBoundL0NormMarker(expr)) {
		return true;
	}
	if (allow_avg_hoist && IsBoundAvgAggregate(expr)) {
		return DecideSideIsWholeNumbered(*expr.Cast<BoundDecideReducerExpression>().children[0], false, decide_index,
		                                 addends_count);
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CAST: {
		// A cast to a whole-numbered type makes the value one; a widening cast of an already-whole operand keeps it
		// one.
		auto &cast = expr.Cast<BoundCastExpression>();
		return DecideTypeIsWholeNumbered(cast.return_type) ||
		       DecideSideIsWholeNumbered(*cast.child, allow_avg_hoist, decide_index, addends_count);
	}
	case ExpressionClass::BOUND_DECIDE_FRAME: {
		// A frame is a value of the row, every addend and its `else` part of it: whole when the type its payload and
		// `else` share is, and a range's SUM, MIN or MAX of them; an AVG range divides, so its value is no whole type.
		auto &frame = expr.Cast<BoundDecideFrameExpression>();
		return frame.children.size() == 1 && DecideTypeIsWholeNumbered(frame.children[0]->return_type) &&
		       DecideTypeIsWholeNumbered(frame.return_type);
	}
	case ExpressionClass::BOUND_DECIDE_REDUCER: {
		auto &agg = expr.Cast<BoundDecideReducerExpression>();
		// A norm is whole when its body is: a count, a sum or a largest of whole values.
		if (agg.kind == DecideReducerKind::NORM || (IsDecideExtremum(agg.kind) && agg.children.size() == 1)) {
			return DecideSideIsWholeNumbered(*agg.children[0], false, decide_index, true);
		}
		if (agg.kind == DecideReducerKind::SUM) {
			return DecideSideIsWholeNumbered(*agg.children[0], false, decide_index, addends_count);
		}
		return DecideTypeIsWholeNumbered(expr.return_type);
	}
	case ExpressionClass::BOUND_FUNCTION: {
		// Sums, differences and products of whole numbers are whole. Division is not.
		auto &func = expr.Cast<BoundFunctionExpression>();
		const auto &name = func.function.name;
		if (name == "+" || name == "-") {
			for (const auto &child : func.children) {
				// An addend that reads no decision is part of the bound: `x + 1000003.50 < K` is exactly
				// `x < K - 1000003.50`, and `x` is still on the lattice.
				if (!addends_count && !BoundExpressionReferencesDecide(*child, decide_index)) {
					continue;
				}
				if (!DecideSideIsWholeNumbered(*child, allow_avg_hoist, decide_index, addends_count)) {
					return false;
				}
			}
			return true;
		}
		if (name == "*") {
			// Every factor matters, decision-bearing or not: a fractional multiplier rescales the decision off the
			// lattice (`0.5 * x`). A product of decisions is not linear in either factor's addends.
			idx_t decision_factors = 0;
			for (const auto &child : func.children) {
				decision_factors += BoundExpressionReferencesDecide(*child, decide_index);
			}
			for (const auto &child : func.children) {
				// A factor that reads no decision is a coefficient: its addends are part of its value, so `x * (w
				// + 1.5)` is off the lattice, and no offset moves to the bound.
				const bool coefficient = !BoundExpressionReferencesDecide(*child, decide_index);
				if (!DecideSideIsWholeNumbered(*child, allow_avg_hoist, decide_index,
				                               addends_count || decision_factors > 1 || coefficient)) {
					return false;
				}
			}
			return true;
		}
		// A whole number raised to a whole power is whole, so `POWER(x, 2)` stays on the lattice though the function's
		// return type is DOUBLE.
		// (The vocabulary check has refused any other exponent.)
		if (name == "**") {
			return DecideSideIsWholeNumbered(*func.children[0], allow_avg_hoist, decide_index, true);
		}
		return DecideTypeIsWholeNumbered(expr.return_type);
	}
	default:
		return DecideTypeIsWholeNumbered(expr.return_type);
	}
}

bool DecideValueIsWholeNumbered(const Expression &expr, idx_t decide_index) {
	return DecideSideIsWholeNumbered(expr, false, decide_index, /*addends_count=*/true);
}

//! The first operand that is not whole-numbered, for the message: a named leaf over the composite above it
//! (`l_quantity`, not `sum((x * l_quantity))`), in the order the side reads.
static bool FindFractionalOperand(const Expression &expr, bool allow_avg_hoist, idx_t decide_index,
                                  const DecideSourceNames &names, string &out) {
	if (DecideSideIsWholeNumbered(expr, allow_avg_hoist, decide_index)) {
		return false;
	}
	// A norm is named as written, not by the definition canonicalization gave it.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_DECIDE_REDUCER &&
	    DecideWrittenNormBody(expr.Cast<BoundDecideReducerExpression>())) {
		out = RenderDecideSource(expr, names);
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!found) {
			found = FindFractionalOperand(child, allow_avg_hoist, decide_index, names, out);
		}
	});
	if (found) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		out = expr.Cast<BoundColumnRefExpression>().GetName();
		return true;
	}
	// No fractional leaf underneath: the node itself is what produces fractions, named as written.
	out = RenderDecideSource(expr, names);
	return true;
}

//! `<`, `>` and `<>` step the bound onto the whole numbers, which is exact only where the compared side, the left one
//! in the canonical form, is whole-numbered. A fractional bound is no error: no whole number equals 2.5.
void DecideShapeCheck::CheckIntegerStep(const BoundComparisonExpression &comparison) const {
	const auto type = comparison.type;
	const bool strict = type == ExpressionType::COMPARE_LESSTHAN || type == ExpressionType::COMPARE_GREATERTHAN;
	const bool not_equal = type == ExpressionType::COMPARE_NOTEQUAL;
	string operand;
	if ((!strict && !not_equal) || !FindFractionalOperand(*comparison.left, not_equal, decide_index, names, operand)) {
		return;
	}
	const char *op = not_equal ? "<>" : type == ExpressionType::COMPARE_LESSTHAN ? "<" : ">";
	throw NotImplementedException(comparison, "%s", FractionalComparisonMessage(op, operand));
}

void DecideShapeCheck::CheckClause(const BoundDecideClause &clause, const vector<const Expression *> &sides) const {
	bool reads_decision = false;
	for (auto side : sides) {
		reads_decision = reads_decision || BoundExpressionReferencesDecide(*side, decide_index);
	}
	if (!reads_decision) {
		throw BinderException(*clause.body, "'%s' reads no decision", RenderDecideClauseBody(clause, names));
	}
	for (auto side : sides) {
		CheckSide(*side);
	}
}

void DecideShapeCheck::CheckConstraints(const vector<BoundDecideClause> &clauses) const {
	for (auto &clause : clauses) {
		// A bound IN marker is a row with nothing to judge.
		if (clause.body->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
			continue;
		}
		auto &comparison = clause.body->Cast<BoundComparisonExpression>();
		CheckClause(clause, {comparison.left.get(), comparison.right.get()});
		CheckIntegerStep(comparison);
	}
}

void DecideShapeCheck::CheckObjective(const BoundDecideClause &objective) const {
	// The objective is the body of its clause: a `when` only chooses the rows.
	CheckClause(objective, {objective.body.get()});
}

} // namespace duckdb
