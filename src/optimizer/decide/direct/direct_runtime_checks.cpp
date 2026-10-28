#include "duckdb/optimizer/decide/direct/direct_runtime_checks.hpp"

#include <algorithm>

#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/decide/direct/direct_plan_fragments.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"

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

unique_ptr<Expression> DirectAllHold(const ExpressionBuilder &calc, vector<unique_ptr<Expression>> checks) {
	auto chain = calc.Bool(true);
	for (idx_t i = checks.size(); i > 0; i--) {
		chain = calc.If(calc.Wrap(std::move(checks[i - 1])), chain, calc.Bool(false));
	}
	return calc.Out(chain);
}

unique_ptr<Expression> DirectInfeasibleErrorPredicate(Optimizer &optimizer) {
	return DirectErrorPredicate(optimizer, "DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see "
	                                       "which clause to change.");
}

unique_ptr<Expression> DirectUnboundedErrorPredicate(Optimizer &optimizer) {
	return DirectErrorPredicate(optimizer, "DECIDE optimization is unbounded. Prefix the query with DIAGNOSE to see "
	                                       "which decision needs a bound.");
}

//! The error for a NaN or infinite number, naming the kind of coefficient the user wrote it in.
static unique_ptr<Expression> DirectInvalidNumberError(Optimizer &optimizer, DirectNumberKind kind) {
	return DirectErrorPredicate(optimizer, kind == DirectNumberKind::OBJECTIVE
	                                           ? DecideInvalidValueMessage("objective coefficient", "NaN or Infinity")
	                                           : DirectCoefficientInvalidMessage());
}

string DirectBoundNaNMessage() {
	return DecideInvalidValueMessage("constraint right-hand side", "NaN");
}

string DirectCoefficientInvalidMessage() {
	return DecideInvalidValueMessage("constraint coefficient", "NaN or Infinity");
}

//! The column part of the NULL message, as a VARCHAR expression over the row it is evaluated on: it names the columns
//! of the value that are NULL on that row, and quotes `text` when none is. The wording comes from the pieces in
//! `decide_value_messages.hpp`, the ones `DecideNullValueMessage` joins for the solver.
//!
//! The expression grows with the number of columns, not with the number of sets of NULL columns. Each column
//! contributes `piece_i`, its quoted name when it is NULL on the row and NULL otherwise. Then
//!   last  = COALESCE(piece_n, ..., piece_1)         the last NULL column, in the order the solver names them
//!   all   = concat_ws(', ', piece_1, ..., piece_n)   every NULL column, comma-joined (concat_ws skips NULL pieces)
//! and the text follows from them: no NULL column when `last` is NULL, one when `all = last`, otherwise the names
//! before the last one, ` and `, and the last one.
static ExprPtr NullColumnsMessage(const ExpressionBuilder &calc, const Expression &value, const string &text) {
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
	auto varchar = [&](const string &text) {
		return calc.Constant(Value(text));
	};
	auto bigint = [&](idx_t number) {
		return calc.Constant(Value::BIGINT(NumericCast<int64_t>(number)));
	};
	auto is_null = [&](idx_t i) {
		return calc.IsNull(calc.Wrap(columns[i]->Copy()));
	};
	auto no_column = DecideNullValueMessage({}, text);
	if (names.empty()) {
		return varchar(no_column);
	}
	if (names.size() == 1) {
		return calc.If(is_null(0), varchar(DecideNullValueMessage(names, text)), varchar(no_column));
	}
	auto piece = [&](idx_t i) {
		return calc.If(is_null(i), varchar(DecideNullNameQuote() + names[i] + DecideNullNameQuote()),
		               calc.Constant(Value(LogicalType::VARCHAR)));
	};
	auto last = [&]() {
		vector<ExprPtr> pieces;
		for (idx_t i = names.size(); i > 0; i--) {
			pieces.push_back(piece(i - 1));
		}
		return calc.Coalesce(pieces);
	};
	auto all = [&]() {
		vector<ExprPtr> parts;
		parts.push_back(varchar(DecideNullNameSeparator()));
		for (idx_t i = 0; i < names.size(); i++) {
			parts.push_back(piece(i));
		}
		return calc.Fn("concat_ws", parts);
	};
	auto length_of = [&](const ExprPtr &text_value) {
		return calc.Fn("length", text_value);
	};
	auto substr = [&](const ExprPtr &text_value, const ExprPtr &from, const ExprPtr &count) {
		return calc.Fn("substr", vector<ExprPtr> {text_value, from, count});
	};
	// One NULL column: its quoted name, without the quotes, goes in twice.
	auto one_column_name = substr(last(), bigint(1 + DecideNullNameQuote().size()),
	                              calc.Sub(length_of(last()), bigint(2 * DecideNullNameQuote().size())));
	vector<ExprPtr> one_column {varchar(DecideNullOneColumnBefore()), one_column_name,
	                            varchar(DecideNullOneColumnMiddle()), one_column_name,
	                            varchar(DecideNullOneColumnAfter())};
	// Several: the comma-joined names without the last one (and the comma before it), then the last.
	auto leading_names =
	    substr(all(), bigint(1),
	           calc.Sub(calc.Sub(length_of(all()), length_of(last())), bigint(DecideNullNameSeparator().size())));
	vector<ExprPtr> joined_names {leading_names, varchar(DecideNullLastNameSeparator()), last()};
	vector<ExprPtr> several {varchar(DecideNullColumnsBefore()), calc.Fn("concat", joined_names),
	                         varchar(DecideNullColumnsAfter())};
	auto exactly_one = calc.Cmp(ExpressionType::COMPARE_EQUAL, all(), last());
	auto some = calc.If(exactly_one, calc.Fn("concat", one_column), calc.Fn("concat", several));
	return calc.If(calc.IsNull(last()), varchar(no_column), some);
}

//! The NULL message of one value, as the solver gives it (DescribeNullSource): the first flattened subquery that is
//! NULL on the row, else the columns that are, else the value as written.
unique_ptr<Expression> DirectNullMessageOf(Optimizer &optimizer, const Expression &value,
                                           const vector<string> &fragments) {
	ExpressionBuilder calc(optimizer.context);
	// A coefficient or bound holds no reducer by now (the bound-side window rewrite made each a column), so no entity
	// scope is needed to render it.
	static const vector<EntityScopeInfo> no_scopes;
	auto message = NullColumnsMessage(calc, value, RenderDecideSource(value, fragments, no_scopes));
	vector<const Expression *> subqueries;
	DecideCollectSubqueries(value, subqueries);
	for (idx_t i = subqueries.size(); i > 0; i--) {
		auto &leaf = *subqueries[i - 1];
		idx_t fragment_id;
		TryParseSourceFragmentTag(leaf.GetAlias(), fragment_id);
		D_ASSERT(fragment_id < fragments.size());
		message = calc.If(calc.IsNull(calc.Of(leaf)),
		                  calc.Constant(Value(DecideNullSubqueryMessage(fragments[fragment_id]))), message);
	}
	return calc.Out(message);
}

unique_ptr<Expression> DirectCheckOnKeptRows(const ExpressionBuilder &calc, unique_ptr<Expression> kept,
                                             unique_ptr<Expression> check) {
	return calc.Out(calc.If(calc.Wrap(std::move(kept)), calc.Wrap(std::move(check)), calc.Bool(true)));
}

unique_ptr<Expression> DirectValidNumberPredicate(Optimizer &optimizer, const LogicalType &number_type,
                                                  ColumnBinding number_binding, const Expression *source,
                                                  DirectNumberKind kind, const vector<string> &fragments) {
	ExpressionBuilder calc(optimizer.context);
	auto number = calc.Col(number_type, number_binding);
	auto null_message = source ? DirectNullMessageOf(optimizer, *source, fragments)
	                           : calc.Out(calc.Constant(Value(DecideNullValueMessage({}, string()))));
	vector<unique_ptr<Expression>> checks;
	checks.push_back(
	    DirectRaiseIf(calc, calc.Out(calc.IsNull(number)), DirectErrorPredicate(optimizer, std::move(null_message))));
	checks.push_back(
	    DirectRaiseUnless(calc, calc.Out(calc.Fn("isfinite", number)), DirectInvalidNumberError(optimizer, kind)));
	return DirectAllHold(calc, std::move(checks));
}

unique_ptr<Expression> DirectValidBoundPredicate(Optimizer &optimizer, ColumnBinding bound_binding,
                                                 const Expression &source_value, const vector<string> &fragments) {
	ExpressionBuilder calc(optimizer.context);
	auto bound = calc.Col(LogicalType::DOUBLE, bound_binding);
	vector<unique_ptr<Expression>> checks;
	checks.push_back(
	    DirectRaiseIf(calc, calc.Out(calc.IsNull(bound)),
	                  DirectErrorPredicate(optimizer, DirectNullMessageOf(optimizer, source_value, fragments))));
	checks.push_back(DirectRaiseIf(calc, calc.Out(calc.Fn("isnan", bound)),
	                               DirectErrorPredicate(optimizer, DirectBoundNaNMessage())));
	return DirectAllHold(calc, std::move(checks));
}

} // namespace duckdb
