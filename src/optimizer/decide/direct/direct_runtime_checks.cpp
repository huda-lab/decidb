#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"

#include <algorithm>

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, unique_ptr<Expression> message) {
	auto error = optimizer.BindScalarFunction("error", std::move(message));
	return BoundCastExpression::AddCastToType(optimizer.context, std::move(error), LogicalType::BOOLEAN);
}

unique_ptr<Expression> DirectErrorPredicate(Optimizer &optimizer, const string &message) {
	return DirectErrorPredicate(optimizer, make_uniq<BoundConstantExpression>(Value(message)));
}

unique_ptr<Expression> DirectInfeasibleErrorPredicate(Optimizer &optimizer) {
	return DirectErrorPredicate(optimizer, "DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see "
	                                       "which clause to change.");
}

unique_ptr<Expression> DirectUnboundedErrorPredicate(Optimizer &optimizer) {
	return DirectErrorPredicate(optimizer, "DECIDE optimization is unbounded. Prefix the query with DIAGNOSE to see "
	                                       "which decision needs a bound.");
}

static unique_ptr<Expression> DirectVarchar(const string &text) {
	return make_uniq<BoundConstantExpression>(Value(text));
}

static unique_ptr<Expression> DirectBigint(int64_t value) {
	return make_uniq<BoundConstantExpression>(Value::BIGINT(value));
}

//! The error for a NaN or infinite number, naming the kind of coefficient the user wrote it in. The solver adds the
//! position of the row; a plan has no row position to name.
static unique_ptr<Expression> DirectInvalidNumberError(Optimizer &optimizer, DirectNumberKind kind) {
	return DirectErrorPredicate(optimizer, DecideInvalidValueMessage(kind == DirectNumberKind::OBJECTIVE
	                                                                     ? "objective coefficient"
	                                                                     : "constraint coefficient",
	                                                                 "NaN or Infinity", string()));
}

string DirectBoundNullMessage(const string &column_name) {
	return DecideNullValueMessage({column_name}, string());
}

string DirectBoundNaNMessage() {
	return DecideInvalidValueMessage("constraint right-hand side", "NaN", string());
}

static unique_ptr<Expression> DirectNullVarchar() {
	return make_uniq<BoundConstantExpression>(Value(LogicalType::VARCHAR));
}

static unique_ptr<Expression> DirectConcat(Optimizer &optimizer, vector<unique_ptr<Expression>> parts) {
	return optimizer.BindScalarFunction("concat", std::move(parts));
}

static unique_ptr<Expression> DirectMinus(Optimizer &optimizer, unique_ptr<Expression> value, idx_t amount) {
	return optimizer.BindScalarFunction("-", std::move(value), DirectBigint(NumericCast<int64_t>(amount)));
}

//! The NULL message of one value, as a VARCHAR expression over the row it is evaluated on: it names the columns of the
//! value that are NULL on that row, and quotes the value when none is. The wording comes from the pieces in
//! `decide_value_messages.hpp`, the ones `DecideNullValueMessage` joins for the solver.
//!
//! The expression grows with the number of columns, not with the number of sets of NULL columns. Each column
//! contributes `piece_i`, its quoted name when it is NULL on the row and NULL otherwise. Then
//!   last  = COALESCE(piece_n, ..., piece_1)         the last NULL column, in the order the solver names them
//!   all   = concat_ws(', ', piece_1, ..., piece_n)   every NULL column, comma-joined (concat_ws skips NULL pieces)
//! and the text follows from them: no NULL column when `last` is NULL, one when `all = last`, otherwise the names
//! before the last one, ` and `, and the last one.
unique_ptr<Expression> DirectNullMessageOf(Optimizer &optimizer, const Expression &value) {
	vector<const Expression *> leaves;
	DecideCollectNamedColumns(value, leaves);
	vector<string> names;
	vector<const Expression *> columns;
	for (auto leaf : leaves) {
		if (std::find(names.begin(), names.end(), leaf->GetAlias()) == names.end()) {
			names.push_back(leaf->GetAlias());
			columns.push_back(leaf);
		}
	}
	auto text = DirectTextOf(value);
	auto no_column = DecideNullValueMessage({}, text);
	if (names.empty()) {
		return DirectVarchar(no_column);
	}
	if (names.size() == 1) {
		return make_uniq<BoundCaseExpression>(DirectIsNull(columns[0]->Copy()),
		                                      DirectVarchar(DecideNullValueMessage(names, text)),
		                                      DirectVarchar(no_column));
	}
	auto piece = [&](idx_t i) {
		return unique_ptr<Expression>(make_uniq<BoundCaseExpression>(
		    DirectIsNull(columns[i]->Copy()), DirectVarchar(DecideNullNameQuote() + names[i] + DecideNullNameQuote()),
		    DirectNullVarchar()));
	};
	auto last = [&]() {
		auto coalesce = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_COALESCE, LogicalType::VARCHAR);
		for (idx_t i = names.size(); i > 0; i--) {
			coalesce->children.push_back(piece(i - 1));
		}
		return unique_ptr<Expression>(std::move(coalesce));
	};
	auto all = [&]() {
		vector<unique_ptr<Expression>> parts;
		parts.push_back(DirectVarchar(DecideNullNameSeparator()));
		for (idx_t i = 0; i < names.size(); i++) {
			parts.push_back(piece(i));
		}
		return optimizer.BindScalarFunction("concat_ws", std::move(parts));
	};
	auto length_of = [&](unique_ptr<Expression> text_value) {
		return optimizer.BindScalarFunction("length", std::move(text_value));
	};
	auto substr = [&](unique_ptr<Expression> text_value, unique_ptr<Expression> from, unique_ptr<Expression> count) {
		vector<unique_ptr<Expression>> args;
		args.push_back(std::move(text_value));
		args.push_back(std::move(from));
		args.push_back(std::move(count));
		return optimizer.BindScalarFunction("substr", std::move(args));
	};
	// One NULL column: its quoted name, without the quotes, goes in twice.
	auto one_column_name = substr(last(), DirectBigint(NumericCast<int64_t>(1 + DecideNullNameQuote().size())),
	                              DirectMinus(optimizer, length_of(last()), 2 * DecideNullNameQuote().size()));
	vector<unique_ptr<Expression>> one_column;
	one_column.push_back(DirectVarchar(DecideNullOneColumnBefore()));
	one_column.push_back(one_column_name->Copy());
	one_column.push_back(DirectVarchar(DecideNullOneColumnMiddle()));
	one_column.push_back(std::move(one_column_name));
	one_column.push_back(DirectVarchar(DecideNullOneColumnAfter()));
	// Several: the comma-joined names without the last one (and the comma before it), then the last.
	auto leading_names =
	    substr(all(), DirectBigint(1),
	           DirectMinus(optimizer, optimizer.BindScalarFunction("-", length_of(all()), length_of(last())),
	                       DecideNullNameSeparator().size()));
	vector<unique_ptr<Expression>> joined_names;
	joined_names.push_back(std::move(leading_names));
	joined_names.push_back(DirectVarchar(DecideNullLastNameSeparator()));
	joined_names.push_back(last());
	vector<unique_ptr<Expression>> several;
	several.push_back(DirectVarchar(DecideNullColumnsBefore()));
	several.push_back(DirectConcat(optimizer, std::move(joined_names)));
	several.push_back(DirectVarchar(DecideNullColumnsAfter()));
	auto exactly_one = make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, all(), last());
	auto some = make_uniq<BoundCaseExpression>(std::move(exactly_one), DirectConcat(optimizer, std::move(one_column)),
	                                           DirectConcat(optimizer, std::move(several)));
	return make_uniq<BoundCaseExpression>(DirectIsNull(last()), DirectVarchar(no_column), std::move(some));
}

unique_ptr<Expression> DirectValidNumberPredicate(Optimizer &optimizer, const LogicalType &number_type,
                                                  ColumnBinding number_binding, const Expression *source,
                                                  DirectNumberKind kind) {
	auto is_null = DirectIsNull(DirectColumn(number_type, number_binding));
	auto is_finite = optimizer.BindScalarFunction("isfinite", DirectColumn(number_type, number_binding));
	auto finite_or_error = make_uniq<BoundCaseExpression>(std::move(is_finite), DirectConstantBool(true),
	                                                      DirectInvalidNumberError(optimizer, kind));
	auto null_message =
	    source
	        ? DirectNullMessageOf(optimizer, *source)
	        : unique_ptr<Expression>(make_uniq<BoundConstantExpression>(Value(DecideNullValueMessage({}, string()))));
	return make_uniq<BoundCaseExpression>(std::move(is_null), DirectErrorPredicate(optimizer, std::move(null_message)),
	                                      std::move(finite_or_error));
}

unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const Expression &source_value) {
	auto is_nan = optimizer.BindScalarFunction("isnan", DirectColumn(LogicalType::DOUBLE, bound_binding));
	auto not_nan = make_uniq<BoundCaseExpression>(
	    std::move(is_nan), DirectErrorPredicate(optimizer, DirectBoundNaNMessage()), DirectConstantBool(true));
	return make_uniq<BoundCaseExpression>(DirectIsNull(DirectColumn(LogicalType::DOUBLE, bound_binding)),
	                                      DirectErrorPredicate(optimizer, DirectNullMessageOf(optimizer, source_value)),
	                                      std::move(not_nan));
}

string DirectInvalidBoundMessage() {
	return "DECIDE: the bound expression has an invalid value (NULL or NaN). Impute it with COALESCE(), or filter "
	       "those rows out with a WHERE clause.";
}

} // namespace duckdb
