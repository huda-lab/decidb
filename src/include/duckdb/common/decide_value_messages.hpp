//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/common/decide_value_messages.hpp
//
// What DECIDE tells the user when a number it reads from the source cannot be used: a NULL value, and a NaN or
// infinite one. The solver path and the direct-solve plans both raise these errors, and both build their text here, so
// one sentence cannot read two ways.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//! The columns an expression names, in the order a NULL message reports them. A leaf is a column reference that still
//! carries the name the user wrote: before lowering it is a BoundColumnRefExpression, after it a positional
//! BoundReferenceExpression with the alias retained. A leaf with no name is skipped, so the caller quotes the
//! expression instead of printing `#3`.
inline void DecideCollectNamedColumns(const Expression &expr, vector<const Expression *> &out) {
	auto expression_class = expr.GetExpressionClass();
	if (expression_class == ExpressionClass::BOUND_COLUMN_REF || expression_class == ExpressionClass::BOUND_REF) {
		if (!expr.GetAlias().empty()) {
			out.push_back(&expr);
		}
		return;
	}
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](const Expression &child) { DecideCollectNamedColumns(child, out); });
}

//! The pieces of the NULL message. `DecideNullValueMessage` joins them for the solver path, and the direct-solve plans
//! join the same pieces with SQL string functions, one column at a time, so a value of any width stays a small plan.
//! With no NULL column to name, the text quotes the expression (`NoColumn`), or is `Generic` when there is none.
inline string DecideNullGenericMessage() {
	return "DECIDE: a value used in the optimization is NULL. Impute it with COALESCE(), "
	       "or filter those rows out with a WHERE clause.";
}
inline string DecideNullNoColumnMessage(const string &expression_text) {
	return "DECIDE: " + expression_text +
	       " is NULL. Impute it with COALESCE(), or filter those rows out with a WHERE clause.";
}
//! One NULL column: `Before` + name + `Middle` + name + `After`.
inline string DecideNullOneColumnBefore() {
	return "DECIDE: column \"";
}
inline string DecideNullOneColumnMiddle() {
	return "\" is NULL. Impute it with COALESCE(";
}
inline string DecideNullOneColumnAfter() {
	return ", 0) or filter those rows out with a WHERE clause.";
}
//! Several NULL columns: `Before` + names + `After`. The names are quoted and joined by `Separator`, the last one by
//! `LastSeparator`.
inline string DecideNullColumnsBefore() {
	return "DECIDE: columns ";
}
inline string DecideNullColumnsAfter() {
	return " are NULL. Impute them with COALESCE(), or filter those rows out with a WHERE clause.";
}
inline string DecideNullNameQuote() {
	return "\"";
}
inline string DecideNullNameSeparator() {
	return ", ";
}
inline string DecideNullLastNameSeparator() {
	return " and ";
}

//! The error for a NULL value, in the terms the user wrote the query in: name the column that is NULL and give the two
//! ways out, impute a value or drop the row. `null_columns` are the NULL columns inside the value, without
//! duplicates. A NULL that no column explains is possible (`NULLIF`, a TRY_CAST that failed): then there is no column
//! to name and `expression_text` is quoted instead. The advice is the same either way.
inline string DecideNullValueMessage(const vector<string> &null_columns, const string &expression_text) {
	if (null_columns.empty()) {
		return expression_text.empty() ? DecideNullGenericMessage() : DecideNullNoColumnMessage(expression_text);
	}
	if (null_columns.size() == 1) {
		return DecideNullOneColumnBefore() + null_columns[0] + DecideNullOneColumnMiddle() + null_columns[0] +
		       DecideNullOneColumnAfter();
	}
	string names;
	for (idx_t i = 0; i < null_columns.size(); i++) {
		if (i > 0) {
			names += (i + 1 == null_columns.size()) ? DecideNullLastNameSeparator() : DecideNullNameSeparator();
		}
		names += DecideNullNameQuote() + null_columns[i] + DecideNullNameQuote();
	}
	return DecideNullColumnsBefore() + names + DecideNullColumnsAfter();
}

//! The error for a NaN or infinite value. `context` names where the value sits (`objective coefficient`), `kind` what
//! is not allowed (`NaN` or `NaN or Infinity`), and `location` is empty or the position the solver adds (` at row 3`).
inline string DecideInvalidValueMessage(const string &context, const string &kind, const string &location) {
	return "DECIDE " + context + " contains invalid value (" + kind + ")" + location +
	       ". Common causes:\n"
	       "  • Division by zero in the expression\n"
	       "  • Arithmetic overflow in calculations\n"
	       "  • NULL values that propagated through math operations\n"
	       "Check your expressions and input data.";
}

} // namespace duckdb
