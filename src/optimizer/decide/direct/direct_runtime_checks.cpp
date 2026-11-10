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
		auto named = DecideInvalidValueMessage(DecideValueRole::COEFFICIENT, names.texts[i - 1]);
		message = calc.If(not_finite, calc.Constant(Value(named)), message);
	}
	// A constant that is not finite fails on every row, so it is named before any column's value.
	for (idx_t i = names.constants.size(); i > 0; i--) {
		auto constant = calc.Cast(calc.Of(*names.constants[i - 1]), LogicalType::DOUBLE);
		auto not_finite = calc.Cmp(ExpressionType::COMPARE_EQUAL, calc.Fn("isfinite", constant), calc.Bool(false));
		auto named = DecideInvalidValueMessage(DecideValueRole::CONSTANT, names.constant_texts[i - 1]);
		message = calc.If(not_finite, calc.Constant(Value(named)), message);
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

//! The texts at the leaves of a message.
static void MessageTexts(const Expression &message, std::set<string> &texts) {
	if (message.GetExpressionClass() == ExpressionClass::BOUND_CASE) {
		auto &branch = message.Cast<BoundCaseExpression>();
		for (auto &check : branch.case_checks) {
			MessageTexts(*check.then_expr, texts);
		}
		MessageTexts(*branch.else_expr, texts);
		return;
	}
	// The caller promises a CASE over constants.
	auto &value = message.Cast<BoundConstantExpression>().value;
	if (!value.IsNull()) {
		texts.insert(StringValue::Get(value));
	}
}

//! `message` with each text replaced by its code.
static unique_ptr<Expression> MessageWithCodes(const Expression &message, const std::map<string, idx_t> &codes) {
	if (message.GetExpressionClass() == ExpressionClass::BOUND_CASE) {
		auto &branch = message.Cast<BoundCaseExpression>();
		auto result = make_uniq<BoundCaseExpression>(DirectMessageCodes::Type());
		for (auto &check : branch.case_checks) {
			result->case_checks.push_back({check.when_expr->Copy(), MessageWithCodes(*check.then_expr, codes)});
		}
		result->else_expr = MessageWithCodes(*branch.else_expr, codes);
		return std::move(result);
	}
	auto &value = message.Cast<BoundConstantExpression>().value;
	return make_uniq<BoundConstantExpression>(
	    value.IsNull() ? Value(DirectMessageCodes::Type())
	                   : Value::UINTEGER(NumericCast<uint32_t>(codes.at(StringValue::Get(value)))));
}

ExprPtr DirectEncodeMessage(const ExpressionBuilder &calc, const ExprPtr &message, DirectMessageCodes &codes) {
	std::set<string> texts;
	MessageTexts(*message, texts);
	codes.texts.assign(texts.begin(), texts.end());
	std::map<string, idx_t> code_of;
	for (idx_t i = 0; i < codes.texts.size(); i++) {
		code_of[codes.texts[i]] = i;
	}
	return calc.Wrap(MessageWithCodes(*message, code_of));
}

ExprPtr DirectDecodeMessage(const ExpressionBuilder &calc, const DirectMessageCodes &codes, const ExprPtr &code) {
	auto message = calc.Constant(Value(LogicalType::VARCHAR));
	for (idx_t i = codes.texts.size(); i > 0; i--) {
		auto is_code =
		    calc.Cmp(ExpressionType::COMPARE_EQUAL, code, calc.Constant(Value::UINTEGER(NumericCast<uint32_t>(i - 1))));
		message = calc.If(is_code, calc.Constant(Value(codes.texts[i - 1])), message);
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
                                                 const string &written) {
	ExpressionBuilder calc(optimizer.context);
	auto bound = calc.Col(LogicalType::DOUBLE, bound_binding);
	return DirectRaiseIf(calc, calc.Out(calc.Fn("isnan", bound)),
	                     DirectErrorPredicate(optimizer, DecideInvalidValueMessage(DecideValueRole::BOUND, written)));
}

} // namespace duckdb
