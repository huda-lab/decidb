#include "duckdb/verification/parsed_statement_verifier.hpp"

#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/cte_node.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/list.hpp"

namespace duckdb {

static bool QueryContainsDecide(const QueryNode &node);

static bool ExpressionContainsDecide(const ParsedExpression &expression) {
	if (expression.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery = expression.Cast<SubqueryExpression>();
		if (QueryContainsDecide(*subquery.subquery->node)) {
			return true;
		}
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expression, [&](const ParsedExpression &child) { found = found || ExpressionContainsDecide(child); });
	return found;
}

static bool ExpressionContainsDecide(const unique_ptr<ParsedExpression> &expression) {
	return expression && ExpressionContainsDecide(*expression);
}

static bool TableRefContainsDecide(const TableRef &ref) {
	switch (ref.type) {
	case TableReferenceType::SUBQUERY:
		return QueryContainsDecide(*ref.Cast<SubqueryRef>().subquery->node);
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		if (TableRefContainsDecide(*join.left) || TableRefContainsDecide(*join.right) ||
		    ExpressionContainsDecide(join.condition)) {
			return true;
		}
		for (auto &column : join.duplicate_eliminated_columns) {
			if (ExpressionContainsDecide(column)) {
				return true;
			}
		}
		return false;
	}
	case TableReferenceType::TABLE_FUNCTION: {
		auto &function = ref.Cast<TableFunctionRef>();
		return ExpressionContainsDecide(function.function) ||
		       (function.subquery && QueryContainsDecide(*function.subquery->node));
	}
	case TableReferenceType::EXPRESSION_LIST: {
		auto &values = ref.Cast<ExpressionListRef>();
		for (auto &row : values.values) {
			for (auto &expression : row) {
				if (ExpressionContainsDecide(expression)) {
					return true;
				}
			}
		}
		return false;
	}
	case TableReferenceType::PIVOT: {
		auto &pivot = ref.Cast<PivotRef>();
		if (TableRefContainsDecide(*pivot.source)) {
			return true;
		}
		for (auto &aggregate : pivot.aggregates) {
			if (ExpressionContainsDecide(aggregate)) {
				return true;
			}
		}
		for (auto &column : pivot.pivots) {
			for (auto &expression : column.pivot_expressions) {
				if (ExpressionContainsDecide(expression)) {
					return true;
				}
			}
			for (auto &entry : column.entries) {
				if (ExpressionContainsDecide(entry.expr)) {
					return true;
				}
			}
			if (column.subquery && QueryContainsDecide(*column.subquery)) {
				return true;
			}
		}
		return false;
	}
	case TableReferenceType::SHOW_REF: {
		auto &show = ref.Cast<ShowRef>();
		return show.query && QueryContainsDecide(*show.query);
	}
	case TableReferenceType::BASE_TABLE:
	case TableReferenceType::EMPTY_FROM:
	case TableReferenceType::COLUMN_DATA:
	case TableReferenceType::DELIM_GET:
	case TableReferenceType::CTE:
	case TableReferenceType::INVALID:
		return false;
	}
	return false;
}

static bool ModifierContainsDecide(const ResultModifier &modifier) {
	switch (modifier.type) {
	case ResultModifierType::LIMIT_MODIFIER: {
		auto &limit = modifier.Cast<LimitModifier>();
		return ExpressionContainsDecide(limit.limit) || ExpressionContainsDecide(limit.offset);
	}
	case ResultModifierType::LIMIT_PERCENT_MODIFIER: {
		auto &limit = modifier.Cast<LimitPercentModifier>();
		return ExpressionContainsDecide(limit.limit) || ExpressionContainsDecide(limit.offset);
	}
	case ResultModifierType::ORDER_MODIFIER:
		for (auto &order : modifier.Cast<OrderModifier>().orders) {
			if (ExpressionContainsDecide(order.expression)) {
				return true;
			}
		}
		return false;
	case ResultModifierType::DISTINCT_MODIFIER:
		for (auto &target : modifier.Cast<DistinctModifier>().distinct_on_targets) {
			if (ExpressionContainsDecide(target)) {
				return true;
			}
		}
		return false;
	}
	return false;
}

static bool QueryContainsDecide(const QueryNode &node) {
	for (auto &entry : node.cte_map.map) {
		if (QueryContainsDecide(*entry.second->query->node)) {
			return true;
		}
	}
	for (auto &modifier : node.modifiers) {
		if (ModifierContainsDecide(*modifier)) {
			return true;
		}
	}

	switch (node.type) {
	case QueryNodeType::SELECT_NODE: {
		auto &select = node.Cast<SelectNode>();
		if (select.HasDecideClause()) {
			return true;
		}
		for (auto &expression : select.select_list) {
			if (ExpressionContainsDecide(expression)) {
				return true;
			}
		}
		for (auto &expression : select.groups.group_expressions) {
			if (ExpressionContainsDecide(expression)) {
				return true;
			}
		}
		return ExpressionContainsDecide(select.where_clause) || ExpressionContainsDecide(select.having) ||
		       ExpressionContainsDecide(select.qualify) ||
		       (select.from_table && TableRefContainsDecide(*select.from_table));
	}
	case QueryNodeType::SET_OPERATION_NODE: {
		auto &setop = node.Cast<SetOperationNode>();
		return QueryContainsDecide(*setop.left) || QueryContainsDecide(*setop.right);
	}
	case QueryNodeType::RECURSIVE_CTE_NODE: {
		auto &cte = node.Cast<RecursiveCTENode>();
		return QueryContainsDecide(*cte.left) || QueryContainsDecide(*cte.right);
	}
	case QueryNodeType::CTE_NODE: {
		auto &cte = node.Cast<CTENode>();
		return QueryContainsDecide(*cte.query) || QueryContainsDecide(*cte.child);
	}
	case QueryNodeType::BOUND_SUBQUERY_NODE:
		return false;
	}
	return false;
}

ParsedStatementVerifier::ParsedStatementVerifier(unique_ptr<SQLStatement> statement_p,
                                                 optional_ptr<case_insensitive_map_t<BoundParameterData>> parameters,
                                                 bool contains_decide_p)
    : StatementVerifier(VerificationType::PARSED, "Parsed", std::move(statement_p), parameters),
      contains_decide(contains_decide_p) {
}

unique_ptr<StatementVerifier>
ParsedStatementVerifier::Create(const SQLStatement &statement,
                                optional_ptr<case_insensitive_map_t<BoundParameterData>> parameters) {
	auto &select = statement.Cast<SelectStatement>();
	bool contains_decide = QueryContainsDecide(*select.node);
	auto query_str = statement.ToString();
	Parser parser;
	try {
		parser.ParseQuery(query_str);
	} catch (std::exception &ex) {
		throw InternalException("Parsed statement verification failed. Query:\n%s\n\nError: %s", query_str, ex.what());
	}
	D_ASSERT(parser.statements.size() == 1);
	D_ASSERT(parser.statements[0]->type == StatementType::SELECT_STATEMENT);
	return make_uniq<ParsedStatementVerifier>(std::move(parser.statements[0]), parameters, contains_decide);
}

} // namespace duckdb
