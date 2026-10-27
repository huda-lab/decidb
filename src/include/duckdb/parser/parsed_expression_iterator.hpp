//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_expression_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/tokens.hpp"

#include <functional>

namespace duckdb {

class ParsedExpressionIterator {
public:
	static void EnumerateChildren(const ParsedExpression &expression,
	                              const std::function<void(const ParsedExpression &child)> &callback);
	static void EnumerateChildren(ParsedExpression &expr, const std::function<void(ParsedExpression &child)> &callback);
	static void EnumerateChildren(ParsedExpression &expr,
	                              const std::function<void(unique_ptr<ParsedExpression> &child)> &callback);

	//! `decide_clauses` false leaves out each DECIDE's SUCH THAT and objective, for a walk that reads a query as
	//! ordinary SQL: a nested DECIDE's clauses are its own binder's.
	static void EnumerateTableRefChildren(TableRef &ref,
	                                      const std::function<void(unique_ptr<ParsedExpression> &child)> &expr_callback,
	                                      const std::function<void(TableRef &ref)> &ref_callback = DefaultRefCallback,
	                                      bool decide_clauses = true);
	static void EnumerateQueryNodeChildren(
	    QueryNode &node, const std::function<void(unique_ptr<ParsedExpression> &child)> &expr_callback,
	    const std::function<void(TableRef &ref)> &ref_callback = DefaultRefCallback, bool decide_clauses = true);

	static void
	EnumerateQueryNodeModifiers(QueryNode &node,
	                            const std::function<void(unique_ptr<ParsedExpression> &child)> &expr_callback);

private:
	static void DefaultRefCallback(TableRef &ref) {}; // NOP
};

} // namespace duckdb
