#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/optimizer/decide/direct/direct_bound_reader.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, unique_ptr<Expression> message) {
	ExpressionBuilder calc(optimizer.context);
	return calc.Out(calc.Cast(calc.Fn("error", calc.Wrap(std::move(message))), LogicalType::BOOLEAN));
}

unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, const string &message) {
	ExpressionBuilder calc(optimizer.context);
	return DirectErrorPredicate(optimizer, calc.Out(calc.Constant(Value(message))));
}

unique_ptr<Expression> DirectRaiseUnless(const ExpressionBuilder &calc, unique_ptr<Expression> ok,
                                         unique_ptr<Expression> error) {
	return calc.Out(calc.If(calc.Wrap(std::move(ok)), calc.Bool(true), calc.Wrap(std::move(error))));
}

unique_ptr<Expression> DirectRaiseIf(const ExpressionBuilder &calc, unique_ptr<Expression> bad,
                                     unique_ptr<Expression> error) {
	return calc.Out(calc.If(calc.Wrap(std::move(bad)), calc.Wrap(std::move(error)), calc.Bool(true)));
}

unique_ptr<Expression> DirectWarnIf(const ExpressionBuilder &calc, unique_ptr<Expression> unstable) {
	return calc.Out(calc.If(calc.Wrap(std::move(unstable)),
	                        calc.Fn("__internal_decide_warn_numbers", vector<ExprPtr> {}), calc.Bool(true)));
}

ExprPtr DirectBelowNormalRange(const ExpressionBuilder &calc, const ExprPtr &value) {
	return calc.Cmp(ExpressionType::COMPARE_LESSTHAN, calc.Fn("abs", value),
	                calc.Num(std::numeric_limits<double>::min()));
}

unique_ptr<Expression> DirectAllHold(const ExpressionBuilder &calc, vector<unique_ptr<Expression>> checks) {
	auto chain = calc.Bool(true);
	for (idx_t i = checks.size(); i > 0; i--) {
		chain = calc.If(calc.Wrap(std::move(checks[i - 1])), chain, calc.Bool(false));
	}
	return calc.Out(chain);
}

unique_ptr<Expression> DirectInfeasibleErrorPredicate(Optimizer &optimizer, const DirectStateErrors &errors) {
	return DirectErrorPredicate(optimizer, errors.infeasible);
}

unique_ptr<Expression> DirectUnboundedErrorPredicate(Optimizer &optimizer, const DirectStateErrors &errors) {
	return DirectErrorPredicate(optimizer, errors.unbounded);
}

//! What a number that is not finite is named: the first of its inputs that is not finite, then a constant that is not,
//! and otherwise the clause.
static ExprPtr NotFiniteMessage(const ExpressionBuilder &calc, const DirectNumberNames &names,
                                const std::function<ExprPtr(const Expression &)> &into) {
	auto message = calc.Constant(Value(DecideInvalidValueMessage(DecideValueRole::TOO_LARGE, names.clause)));
	// Built from the last input back, so the first that is not finite is the one named. A NULL input is no answer.
	for (idx_t i = names.inputs.size(); i > 0; i--) {
		auto input = calc.Cast(into(*names.inputs[i - 1]), LogicalType::DOUBLE);
		auto not_finite = calc.Cmp(ExpressionType::COMPARE_EQUAL, calc.Fn("isfinite", input), calc.Bool(false));
		auto named = calc.RaiseInvalidValue(DecideValueRole::COEFFICIENT, names.texts[i - 1], names.places[i - 1]);
		message = calc.If(not_finite, named, message);
	}
	// A constant that is not finite fails on every row, so it is named before any column's value.
	for (idx_t i = names.constants.size(); i > 0; i--) {
		auto constant = calc.Cast(calc.Of(*names.constants[i - 1]), LogicalType::DOUBLE);
		auto not_finite = calc.Cmp(ExpressionType::COMPARE_EQUAL, calc.Fn("isfinite", constant), calc.Bool(false));
		auto named = calc.RaiseInvalidValue(DecideValueRole::CONSTANT, names.constant_texts[i - 1],
		                                    names.constant_places[i - 1]);
		message = calc.If(not_finite, named, message);
	}
	return message;
}

ExprPtr DirectNumberMessage(const ExpressionBuilder &calc, const ExprPtr &number, const DirectNumberNames &names,
                            const std::function<ExprPtr(const Expression &)> &into) {
	auto none = calc.Constant(Value(LogicalType::VARCHAR));
	return calc.If(calc.IsNull(number), none,
	               calc.If(calc.Fn("isfinite", number), none, NotFiniteMessage(calc, names, into)));
}

ExprPtr DirectNumberMessage(const ExpressionBuilder &calc, const ExprPtr &number, const string &clause) {
	DirectNumberNames names;
	names.clause = clause;
	return DirectNumberMessage(calc, number, names, nullptr);
}

//! A leaf of a message that is no text: the call that raises a value's error where it is evaluated
//! (ExpressionBuilder::RaiseInvalidValue), which a row that reaches it never returns from.
static bool IsRaisingLeaf(const Expression &message) {
	return message.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION;
}

//! The texts and the calls that raise at the leaves of a message.
static void MessageLeaves(const ExpressionBuilder &calc, const Expression &message, std::set<string> &texts,
                          vector<ExprPtr> &raises) {
	if (message.GetExpressionClass() == ExpressionClass::BOUND_CASE) {
		auto &branch = message.Cast<BoundCaseExpression>();
		for (auto &check : branch.case_checks) {
			MessageLeaves(calc, *check.then_expr, texts, raises);
		}
		MessageLeaves(calc, *branch.else_expr, texts, raises);
		return;
	}
	if (IsRaisingLeaf(message)) {
		if (std::none_of(raises.begin(), raises.end(), [&](const ExprPtr &known) { return known->Equals(message); })) {
			raises.push_back(calc.Of(message));
		}
		return;
	}
	// The caller promises a CASE over constants and the calls that raise.
	auto &value = message.Cast<BoundConstantExpression>().value;
	if (!value.IsNull()) {
		texts.insert(StringValue::Get(value));
	}
}

//! `message` with each text and each call replaced by its code.
static unique_ptr<Expression> MessageWithCodes(const Expression &message, const std::map<string, idx_t> &codes,
                                               const DirectMessageCodes &known) {
	if (message.GetExpressionClass() == ExpressionClass::BOUND_CASE) {
		auto &branch = message.Cast<BoundCaseExpression>();
		auto result = make_uniq<BoundCaseExpression>(DirectMessageCodes::Type());
		for (auto &check : branch.case_checks) {
			result->case_checks.push_back({check.when_expr->Copy(), MessageWithCodes(*check.then_expr, codes, known)});
		}
		result->else_expr = MessageWithCodes(*branch.else_expr, codes, known);
		return std::move(result);
	}
	if (IsRaisingLeaf(message)) {
		// MessageLeaves collected every call of the message.
		idx_t j = 0;
		while (!known.raises[j]->Equals(message)) {
			j++;
		}
		return make_uniq<BoundConstantExpression>(Value::UINTEGER(NumericCast<uint32_t>(known.texts.size() + j)));
	}
	auto &value = message.Cast<BoundConstantExpression>().value;
	return make_uniq<BoundConstantExpression>(
	    value.IsNull() ? Value(DirectMessageCodes::Type())
	                   : Value::UINTEGER(NumericCast<uint32_t>(codes.at(StringValue::Get(value)))));
}

ExprPtr DirectEncodeMessage(const ExpressionBuilder &calc, const ExprPtr &message, DirectMessageCodes &codes) {
	std::set<string> texts;
	codes.raises.clear();
	MessageLeaves(calc, *message, texts, codes.raises);
	codes.texts.assign(texts.begin(), texts.end());
	std::map<string, idx_t> code_of;
	for (idx_t i = 0; i < codes.texts.size(); i++) {
		code_of[codes.texts[i]] = i;
	}
	return calc.Wrap(MessageWithCodes(*message, code_of, codes));
}

ExprPtr DirectDecodeMessage(const ExpressionBuilder &calc, const DirectMessageCodes &codes, const ExprPtr &code) {
	auto message = calc.Constant(Value(LogicalType::VARCHAR));
	auto is = [&](idx_t i) {
		return calc.Cmp(ExpressionType::COMPARE_EQUAL, code, calc.Constant(Value::UINTEGER(NumericCast<uint32_t>(i))));
	};
	for (idx_t j = codes.raises.size(); j > 0; j--) {
		message = calc.If(is(codes.texts.size() + j - 1), codes.raises[j - 1], message);
	}
	for (idx_t i = codes.texts.size(); i > 0; i--) {
		message = calc.If(is(i - 1), calc.Constant(Value(codes.texts[i - 1])), message);
	}
	return message;
}

unique_ptr<Expression> DirectRaiseMessage(Optimizer &optimizer, const ExpressionBuilder &calc, const ExprPtr &message) {
	return DirectRaiseUnless(calc, calc.Out(calc.IsNull(message)), DirectErrorPredicate(optimizer, calc.Out(message)));
}

//! The check of a number: true when it is finite, and otherwise the error `bad` names unless the number is NULL. The
//! gate is one BOOLEAN per row, and `bad` is built on the rows that fail alone, as a CASE evaluates a branch only where
//! it is chosen.
static unique_ptr<Expression> RaiseUnlessFinite(Optimizer &optimizer, const ExpressionBuilder &calc,
                                                const ExprPtr &number, const ExprPtr &bad) {
	auto raise = calc.Wrap(
	    DirectRaiseUnless(calc, calc.Out(calc.IsNull(number)), DirectErrorPredicate(optimizer, calc.Out(bad))));
	return calc.Out(calc.If(calc.Fn("isfinite", number), calc.Bool(true), raise));
}

unique_ptr<Expression> DirectRaiseTooLarge(Optimizer &optimizer, const ExprPtr &number, const string &clause) {
	ExpressionBuilder calc(optimizer.context);
	DirectNumberNames names;
	names.clause = clause;
	return RaiseUnlessFinite(optimizer, calc, number, NotFiniteMessage(calc, names, nullptr));
}

unique_ptr<Expression> DirectCheckNumber(Optimizer &optimizer, ColumnBinding number, const DirectNumberNames &names,
                                         const vector<DirectSignedValue> &values,
                                         const vector<ColumnBinding> &source_bindings, idx_t projection_index,
                                         const DirectFactor &factor) {
	if (DirectIsFiniteWhereKept(optimizer, values, factor)) {
		return nullptr;
	}
	ExpressionBuilder calc(optimizer.context);
	auto bad = NotFiniteMessage(calc, names, [&](const Expression &input) {
		return calc.Wrap(DirectOverProjection(input, source_bindings, projection_index));
	});
	return RaiseUnlessFinite(optimizer, calc, calc.Col(LogicalType::DOUBLE, number), bad);
}

unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const string &written, optional_idx place) {
	ExpressionBuilder calc(optimizer.context);
	auto bound = calc.Col(LogicalType::DOUBLE, bound_binding);
	auto raise = calc.Cast(calc.RaiseInvalidValue(DecideValueRole::BOUND, written, place), LogicalType::BOOLEAN);
	return DirectRaiseIf(calc, calc.Out(calc.Fn("isnan", bound)), calc.Out(raise));
}

} // namespace duckdb
