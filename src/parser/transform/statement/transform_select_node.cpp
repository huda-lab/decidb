#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/transformer.hpp"
#include "duckdb/parser/query_node/cte_node.hpp"

namespace duckdb {

void Transformer::TransformModifiers(duckdb_libpgquery::PGSelectStmt &stmt, QueryNode &node) {
	// transform the common properties
	// both the set operations and the regular select can have an ORDER BY/LIMIT attached to them
	vector<OrderByNode> orders;
	TransformOrderBy(stmt.sortClause, orders);
	if (!orders.empty()) {
		auto order_modifier = make_uniq<OrderModifier>();
		order_modifier->orders = std::move(orders);
		node.modifiers.push_back(std::move(order_modifier));
	}

	if (stmt.limitCount || stmt.limitOffset) {
		if (stmt.limitCount && stmt.limitCount->type == duckdb_libpgquery::T_PGLimitPercent) {
			auto limit_percent_modifier = make_uniq<LimitPercentModifier>();
			auto expr_node = PGPointerCast<duckdb_libpgquery::PGLimitPercent>(stmt.limitCount)->limit_percent;
			limit_percent_modifier->limit = TransformExpression(expr_node);
			if (stmt.limitOffset) {
				limit_percent_modifier->offset = TransformExpression(stmt.limitOffset);
			}
			node.modifiers.push_back(std::move(limit_percent_modifier));
		} else {
			auto limit_modifier = make_uniq<LimitModifier>();
			if (stmt.limitCount) {
				limit_modifier->limit = TransformExpression(stmt.limitCount);
			}
			if (stmt.limitOffset) {
				limit_modifier->offset = TransformExpression(stmt.limitOffset);
			}
			node.modifiers.push_back(std::move(limit_modifier));
		}
	}
}

unique_ptr<QueryNode> Transformer::TransformSelectInternal(duckdb_libpgquery::PGSelectStmt &stmt) {
	D_ASSERT(stmt.type == duckdb_libpgquery::T_PGSelectStmt);
	auto stack_checker = StackCheck();

	unique_ptr<QueryNode> node;

	switch (stmt.op) {
	case duckdb_libpgquery::PG_SETOP_NONE: {
		node = make_uniq<SelectNode>();
		auto &result = node->Cast<SelectNode>();
		if (stmt.withClause) {
			TransformCTE(*PGPointerCast<duckdb_libpgquery::PGWithClause>(stmt.withClause), node->cte_map);
		}
		if (stmt.windowClause) {
			for (auto window_ele = stmt.windowClause->head; window_ele != nullptr; window_ele = window_ele->next) {
				auto window_def = PGPointerCast<duckdb_libpgquery::PGWindowDef>(window_ele->data.ptr_value);
				D_ASSERT(window_def);
				D_ASSERT(window_def->name);
				string window_name(window_def->name);
				auto it = window_clauses.find(window_name);
				if (it != window_clauses.end()) {
					throw ParserException("window \"%s\" is already defined", window_name);
				}
				window_clauses[window_name] = window_def.get();
			}
		}

		// checks distinct clause
		if (stmt.distinctClause != nullptr) {
			auto modifier = make_uniq<DistinctModifier>();
			// checks distinct on clause
			auto target = PGPointerCast<duckdb_libpgquery::PGNode>(stmt.distinctClause->head->data.ptr_value);
			if (target) {
				//  add the columns defined in the ON clause to the select list
				TransformExpressionList(*stmt.distinctClause, modifier->distinct_on_targets);
			}
			result.modifiers.push_back(std::move(modifier));
		}

		// do this early so the value lists also have a `FROM`
		if (stmt.valuesLists) {
			// VALUES list, create an ExpressionList
			D_ASSERT(!stmt.fromClause);
			result.from_table = TransformValuesList(stmt.valuesLists);
			result.select_list.push_back(make_uniq<StarExpression>());
		} else {
			if (!stmt.targetList) {
				throw ParserException("SELECT clause without selection list");
			}
			// transform in the specified order to ensure positional parameters are correctly set
			if (stmt.from_first) {
				result.from_table = TransformFrom(stmt.fromClause);
				TransformExpressionList(*stmt.targetList, result.select_list);
			} else {
				TransformExpressionList(*stmt.targetList, result.select_list);
				result.from_table = TransformFrom(stmt.fromClause);
			}
		}
		// DeciDB's decide
		if (stmt.decideClause) {
			auto &decide_clause = PGCast<duckdb_libpgquery::PGDecideClause>(*stmt.decideClause);
			// A declaration with no type is refused only now, after the whole query has parsed, so that a syntax error
			// after the name is the one reported.
			for (auto cell = decide_clause.variables ? decide_clause.variables->head : nullptr; cell;
			     cell = cell->next) {
				auto &declaration = PGCast<duckdb_libpgquery::PGDecideDeclaration>(
				    *PGPointerCast<duckdb_libpgquery::PGNode>(cell->data.ptr_value));
				if (declaration.var_type == duckdb_libpgquery::PG_DECIDE_TYPE_NONE) {
					throw ParserException::SyntaxError(
					    *RootTransformer().parsed_query,
					    StringUtil::Format("decision '%s' has no type", declaration.name),
					    NumericCast<idx_t>(declaration.location));
				}
			}
			if (!decide_clause.constraints) {
				throw ParserException::SyntaxError(*RootTransformer().parsed_query, DECIDE_NO_CONSTRAINTS_MESSAGE,
				                                   NumericCast<idx_t>(decide_clause.location));
			}
			if (decide_clause.spans) {
				RootTransformer().IndexDecideExpressions(*decide_clause.spans);
			}
			result.decide = make_uniq<ParsedDecide>();
			auto &decide = *result.decide;
			for (auto cell = decide_clause.variables->head; cell; cell = cell->next) {
				auto &declaration = PGCast<duckdb_libpgquery::PGDecideDeclaration>(
				    *PGPointerCast<duckdb_libpgquery::PGNode>(cell->data.ptr_value));
				DecideDeclaration variable;
				variable.name = declaration.name;
				variable.type =
				    declaration.var_type == duckdb_libpgquery::PG_DECIDE_TYPE_REAL   ? DecideVariableType::REAL
				    : declaration.var_type == duckdb_libpgquery::PG_DECIDE_TYPE_BOOL ? DecideVariableType::BOOL
				                                                                     : DecideVariableType::INT;
				variable.scope = TransformDecideScope(declaration.scope);
				variable.location = NumericCast<idx_t>(declaration.location);
				decide.variables.push_back(std::move(variable));
			}
			for (auto cell = decide_clause.constraints->head; cell; cell = cell->next) {
				auto clause = TransformDecideItem(*PGPointerCast<duckdb_libpgquery::PGNode>(cell->data.ptr_value));
				// `(a AND b)` with no scope in front is its constraints, each one a clause.
				if (clause.scope.IsEmpty() && clause.body->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
					for (auto &child : clause.body->Cast<ConjunctionExpression>().children) {
						decide.constraints.emplace_back();
						decide.constraints.back().body = std::move(child);
					}
					continue;
				}
				decide.constraints.push_back(std::move(clause));
			}
			if (decide_clause.sense == duckdb_libpgquery::PG_OBJ_MAXIMIZE) {
				decide.sense = DecideSense::MAXIMIZE;
			} else if (decide_clause.sense == duckdb_libpgquery::PG_OBJ_MINIMIZE) {
				decide.sense = DecideSense::MINIMIZE;
			}
			if (decide_clause.objective) {
				decide.objective = TransformDecideItem(*decide_clause.objective);
			}
			// The text the user wrote, from DECIDE through the objective, with where the parser found each construct.
			auto &query = RootTransformer().parsed_query;
			if (query && decide_clause.decl_start >= 0 && decide_clause.decl_start <= decide_clause.tail_start &&
			    decide_clause.tail_end <= NumericCast<int>(query->size())) {
				auto &text = decide.text;
				text.start = NumericCast<idx_t>(decide_clause.decl_start);
				text.tail_start = NumericCast<idx_t>(decide_clause.tail_start);
				text.text = query->substr(text.start, NumericCast<idx_t>(decide_clause.tail_end) - text.start);
				for (auto cell = decide_clause.spans ? decide_clause.spans->head : nullptr; cell; cell = cell->next) {
					auto &span = *reinterpret_cast<duckdb_libpgquery::PGDecideSpan *>(cell->data.ptr_value);
					if (span.anchor < decide_clause.decl_start || span.end > decide_clause.tail_end) {
						continue;
					}
					for (auto value : {span.anchor, static_cast<int>(span.role), span.start, span.end, 0}) {
						text.spans.push_back(NumericCast<idx_t>(value));
					}
					for (idx_t i : {0, 2, 3}) {
						text.spans[text.spans.size() - 5 + i] -= text.start;
					}
				}
				// Every expression of the clause, anchored at the location its parsed expression took.
				for (auto &entry : RootTransformer().decide_expr_spans) {
					auto &span = entry.second;
					if (span.start < decide_clause.decl_start || span.end > decide_clause.tail_end) {
						continue;
					}
					for (auto value : {NumericCast<idx_t>(entry.first), static_cast<idx_t>(DecideSpanRole::EXPR),
					                   NumericCast<idx_t>(span.start), NumericCast<idx_t>(span.end),
					                   NumericCast<idx_t>(span.binding)}) {
						text.spans.push_back(value);
					}
					for (idx_t i : {0, 2, 3}) {
						text.spans[text.spans.size() - 5 + i] -= text.start;
					}
				}
			}
		}
		// where
		result.where_clause = TransformExpression(stmt.whereClause);
		// group by
		TransformGroupBy(stmt.groupClause, result);
		// having
		result.having = TransformExpression(stmt.havingClause);
		// qualify
		result.qualify = TransformExpression(stmt.qualifyClause);
		// sample
		result.sample = TransformSampleOptions(stmt.sampleOptions);
		break;
	}
	case duckdb_libpgquery::PG_SETOP_UNION:
	case duckdb_libpgquery::PG_SETOP_EXCEPT:
	case duckdb_libpgquery::PG_SETOP_INTERSECT:
	case duckdb_libpgquery::PG_SETOP_UNION_BY_NAME: {
		node = make_uniq<SetOperationNode>();
		auto &result = node->Cast<SetOperationNode>();
		if (stmt.withClause) {
			TransformCTE(*PGPointerCast<duckdb_libpgquery::PGWithClause>(stmt.withClause), node->cte_map);
		}
		result.left = TransformSelectNode(*stmt.larg);
		result.right = TransformSelectNode(*stmt.rarg);
		if (!result.left || !result.right) {
			throw InternalException("Failed to transform setop children.");
		}

		result.setop_all = stmt.all;
		switch (stmt.op) {
		case duckdb_libpgquery::PG_SETOP_UNION:
			result.setop_type = SetOperationType::UNION;
			break;
		case duckdb_libpgquery::PG_SETOP_EXCEPT:
			result.setop_type = SetOperationType::EXCEPT;
			break;
		case duckdb_libpgquery::PG_SETOP_INTERSECT:
			result.setop_type = SetOperationType::INTERSECT;
			break;
		case duckdb_libpgquery::PG_SETOP_UNION_BY_NAME:
			result.setop_type = SetOperationType::UNION_BY_NAME;
			break;
		default:
			throw InternalException("Unexpected setop type");
		}
		if (stmt.sampleOptions) {
			throw ParserException("SAMPLE clause is only allowed in regular SELECT statements");
		}
		break;
	}
	default:
		throw NotImplementedException("Statement type %d not implemented!", stmt.op);
	}

	TransformModifiers(stmt, *node);

	return node;
}

} // namespace duckdb
