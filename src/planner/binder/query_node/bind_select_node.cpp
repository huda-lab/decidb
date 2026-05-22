#include "duckdb/common/limits.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_expanded_expression.hpp"
#include "duckdb/planner/expression_binder/column_alias_binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/planner/expression_binder/group_binder.hpp"
#include "duckdb/planner/expression_binder/having_binder.hpp"
#include "duckdb/planner/expression_binder/order_binder.hpp"
#include "duckdb/planner/expression_binder/qualify_binder.hpp"
#include "duckdb/planner/expression_binder/select_bind_state.hpp"
#include "duckdb/planner/expression_binder/select_binder.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "duckdb/planner/expression_binder/decide_binder.hpp"
#include "duckdb/planner/expression_binder/decide_constraints_binder.hpp"
#include "duckdb/planner/expression_binder/decide_objective_binder.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"

#include "duckdb/packdb/utility/debug.hpp"
#include "duckdb/packdb/symbolic/decide_symbolic.hpp"

namespace duckdb {

unique_ptr<Expression> Binder::BindOrderExpression(OrderBinder &order_binder, unique_ptr<ParsedExpression> expr) {
	// we treat the distinct list as an ORDER BY
	auto bound_expr = order_binder.Bind(std::move(expr));
	if (!bound_expr) {
		// DISTINCT ON non-integer constant
		// remove the expression from the DISTINCT ON list
		return nullptr;
	}
	D_ASSERT(bound_expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT);
	return bound_expr;
}

BoundLimitNode Binder::BindLimitValue(OrderBinder &order_binder, unique_ptr<ParsedExpression> limit_val,
                                      bool is_percentage, bool is_offset) {
	auto new_binder = Binder::CreateBinder(context, this);
	ExpressionBinder expr_binder(*new_binder, context);
	auto target_type = is_percentage ? LogicalType::DOUBLE : LogicalType::BIGINT;
	expr_binder.target_type = target_type;
	auto original_limit = limit_val->Copy();
	auto expr = expr_binder.Bind(limit_val);
	if (expr->HasSubquery()) {
		if (!order_binder.HasExtraList()) {
			throw BinderException("Subquery in LIMIT/OFFSET not supported in set operation");
		}
		auto bound_limit = order_binder.CreateExtraReference(std::move(original_limit));
		if (is_percentage) {
			return BoundLimitNode::ExpressionPercentage(std::move(bound_limit));
		} else {
			return BoundLimitNode::ExpressionValue(std::move(bound_limit));
		}
	}
	if (expr->IsFoldable()) {
		//! this is a constant
		auto val = ExpressionExecutor::EvaluateScalar(context, *expr).CastAs(context, target_type);
		if (is_percentage) {
			D_ASSERT(!is_offset);
			double percentage_val;
			if (val.IsNull()) {
				percentage_val = 100.0;
			} else {
				percentage_val = val.GetValue<double>();
			}
			if (Value::IsNan(percentage_val) || percentage_val < 0 || percentage_val > 100) {
				throw OutOfRangeException("Limit percent out of range, should be between 0% and 100%");
			}
			return BoundLimitNode::ConstantPercentage(percentage_val);
		} else {
			int64_t constant_val;
			if (val.IsNull()) {
				constant_val = is_offset ? 0 : NumericLimits<int64_t>::Maximum();
			} else {
				constant_val = val.GetValue<int64_t>();
			}
			if (constant_val < 0) {
				throw BinderException(expr->GetQueryLocation(), "LIMIT/OFFSET cannot be negative");
			}
			return BoundLimitNode::ConstantValue(constant_val);
		}
	}
	if (!new_binder->correlated_columns.empty()) {
		throw BinderException("Correlated columns not supported in LIMIT/OFFSET");
	}
	// move any correlated columns to this binder
	MoveCorrelatedExpressions(*new_binder);
	if (is_percentage) {
		return BoundLimitNode::ExpressionPercentage(std::move(expr));
	} else {
		return BoundLimitNode::ExpressionValue(std::move(expr));
	}
}

duckdb::unique_ptr<BoundResultModifier> Binder::BindLimit(OrderBinder &order_binder, LimitModifier &limit_mod) {
	auto result = make_uniq<BoundLimitModifier>();
	if (limit_mod.limit) {
		result->limit_val = BindLimitValue(order_binder, std::move(limit_mod.limit), false, false);
	}
	if (limit_mod.offset) {
		result->offset_val = BindLimitValue(order_binder, std::move(limit_mod.offset), false, true);
	}
	return std::move(result);
}

unique_ptr<BoundResultModifier> Binder::BindLimitPercent(OrderBinder &order_binder, LimitPercentModifier &limit_mod) {
	auto result = make_uniq<BoundLimitModifier>();
	if (limit_mod.limit) {
		result->limit_val = BindLimitValue(order_binder, std::move(limit_mod.limit), true, false);
	}
	if (limit_mod.offset) {
		result->offset_val = BindLimitValue(order_binder, std::move(limit_mod.offset), false, true);
	}
	return std::move(result);
}

void Binder::PrepareModifiers(OrderBinder &order_binder, QueryNode &statement, BoundQueryNode &result) {
	for (auto &mod : statement.modifiers) {
		unique_ptr<BoundResultModifier> bound_modifier;
		switch (mod->type) {
		case ResultModifierType::DISTINCT_MODIFIER: {
			auto &distinct = mod->Cast<DistinctModifier>();
			auto bound_distinct = make_uniq<BoundDistinctModifier>();
			bound_distinct->distinct_type =
			    distinct.distinct_on_targets.empty() ? DistinctType::DISTINCT : DistinctType::DISTINCT_ON;
			if (distinct.distinct_on_targets.empty()) {
				for (idx_t i = 0; i < result.names.size(); i++) {
					distinct.distinct_on_targets.push_back(
					    make_uniq<ConstantExpression>(Value::INTEGER(UnsafeNumericCast<int32_t>(1 + i))));
				}
			}
			order_binder.SetQueryComponent("DISTINCT ON");
			for (auto &distinct_on_target : distinct.distinct_on_targets) {
				auto expr = BindOrderExpression(order_binder, std::move(distinct_on_target));
				if (!expr) {
					continue;
				}
				bound_distinct->target_distincts.push_back(std::move(expr));
			}
			order_binder.SetQueryComponent();

			bound_modifier = std::move(bound_distinct);
			break;
		}
		case ResultModifierType::ORDER_MODIFIER: {

			auto &order = mod->Cast<OrderModifier>();
			auto bound_order = make_uniq<BoundOrderModifier>();
			auto &config = DBConfig::GetConfig(context);
			D_ASSERT(!order.orders.empty());
			auto &order_binders = order_binder.GetBinders();
			if (order.orders.size() == 1 && order.orders[0].expression->GetExpressionType() == ExpressionType::STAR) {
				auto &star = order.orders[0].expression->Cast<StarExpression>();
				if (star.exclude_list.empty() && star.replace_list.empty() && !star.expr) {
					// ORDER BY ALL
					// replace the order list with the all elements in the SELECT list
					auto order_type = config.ResolveOrder(order.orders[0].type);
					auto null_order = config.ResolveNullOrder(order_type, order.orders[0].null_order);
					auto constant_expr = make_uniq<BoundConstantExpression>(Value("ALL"));
					bound_order->orders.emplace_back(order_type, null_order, std::move(constant_expr));
					bound_modifier = std::move(bound_order);
					break;
				}
			}
#if 0
			// When this verification is enabled, replace ORDER BY x, y with ORDER BY create_sort_key(x, y)
			// note that we don't enable this during actual verification since it doesn't always work
			// e.g. it breaks EXPLAIN output on queries
			bool can_replace = true;
			for (auto &order_node : order.orders) {
				if (order_node.expression->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
					// we cannot replace the sort key when we order by literals (e.g. ORDER BY 1, 2`
					can_replace = false;
					break;
				}
			}
			if (!order_binder.HasExtraList()) {
				// we can only do the replacement when we can order by elements that are not in the selection list
				can_replace = false;
			}
			if (can_replace) {
				vector<unique_ptr<ParsedExpression>> sort_key_parameters;
				for (auto &order_node : order.orders) {
					sort_key_parameters.push_back(std::move(order_node.expression));
					auto type = config.ResolveOrder(order_node.type);
					auto null_order = config.ResolveNullOrder(type, order_node.null_order);
					string sort_param = EnumUtil::ToString(type) + " " + EnumUtil::ToString(null_order);
					sort_key_parameters.push_back(make_uniq<ConstantExpression>(Value(sort_param)));
				}
				order.orders.clear();
				auto create_sort_key = make_uniq<FunctionExpression>("create_sort_key", std::move(sort_key_parameters));
				order.orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, std::move(create_sort_key));
			}
#endif
			for (auto &order_node : order.orders) {
				vector<unique_ptr<ParsedExpression>> order_list;
				order_binders[0].get().ExpandStarExpression(std::move(order_node.expression), order_list);

				auto type = config.ResolveOrder(order_node.type);
				auto null_order = config.ResolveNullOrder(type, order_node.null_order);
				for (auto &order_expr : order_list) {
					auto bound_expr = BindOrderExpression(order_binder, std::move(order_expr));
					if (!bound_expr) {
						continue;
					}
					bound_order->orders.emplace_back(type, null_order, std::move(bound_expr));
				}
			}
			if (!bound_order->orders.empty()) {
				bound_modifier = std::move(bound_order);
			}
			break;
		}
		case ResultModifierType::LIMIT_MODIFIER:
			bound_modifier = BindLimit(order_binder, mod->Cast<LimitModifier>());
			break;
		case ResultModifierType::LIMIT_PERCENT_MODIFIER:
			bound_modifier = BindLimitPercent(order_binder, mod->Cast<LimitPercentModifier>());
			break;
		default:
			throw InternalException("Unsupported result modifier");
		}
		if (bound_modifier) {
			result.modifiers.push_back(std::move(bound_modifier));
		}
	}
}

unique_ptr<Expression> CreateOrderExpression(unique_ptr<Expression> expr, const vector<string> &names,
                                             const vector<LogicalType> &sql_types, idx_t table_index, idx_t index) {
	if (index >= sql_types.size()) {
		throw BinderException(*expr, "ORDER term out of range - should be between 1 and %lld", sql_types.size());
	}
	auto result =
	    make_uniq<BoundColumnRefExpression>(expr->GetAlias(), sql_types[index], ColumnBinding(table_index, index));
	if (result->GetAlias().empty() && index < names.size()) {
		result->SetAlias(names[index]);
	}
	return std::move(result);
}

unique_ptr<Expression> FinalizeBindOrderExpression(unique_ptr<Expression> expr, idx_t table_index,
                                                   const vector<string> &names, const vector<LogicalType> &sql_types,
                                                   const SelectBindState &bind_state) {
	auto &constant = expr->Cast<BoundConstantExpression>();
	switch (constant.value.type().id()) {
	case LogicalTypeId::UBIGINT: {
		// index
		auto index = UBigIntValue::Get(constant.value);
		return CreateOrderExpression(std::move(expr), names, sql_types, table_index, bind_state.GetFinalIndex(index));
	}
	case LogicalTypeId::VARCHAR: {
		// ORDER BY ALL
		return nullptr;
	}
	case LogicalTypeId::STRUCT: {
		// collation
		auto &struct_values = StructValue::GetChildren(constant.value);
		if (struct_values.size() > 2) {
			throw InternalException("Expected one or two children: index and optional collation");
		}
		auto index = UBigIntValue::Get(struct_values[0]);
		string collation;
		if (struct_values.size() == 2) {
			collation = StringValue::Get(struct_values[1]);
		}
		auto result = CreateOrderExpression(std::move(expr), names, sql_types, table_index, index);
		if (!collation.empty()) {
			if (sql_types[index].id() != LogicalTypeId::VARCHAR) {
				throw BinderException(*result, "COLLATE can only be applied to varchar columns");
			}
			result->return_type = LogicalType::VARCHAR_COLLATION(std::move(collation));
		}
		return result;
	}
	default:
		throw InternalException("Unknown type in FinalizeBindOrderExpression");
	}
}

static void AssignReturnType(unique_ptr<Expression> &expr, idx_t table_index, const vector<string> &names,
                             const vector<LogicalType> &sql_types, const SelectBindState &bind_state) {
	if (!expr) {
		return;
	}
	if (expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		expr = FinalizeBindOrderExpression(std::move(expr), table_index, names, sql_types, bind_state);
	}
	if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return;
	}
	auto &bound_colref = expr->Cast<BoundColumnRefExpression>();
	bound_colref.return_type = sql_types[bound_colref.binding.column_index];
}

void Binder::BindModifiers(BoundQueryNode &result, idx_t table_index, const vector<string> &names,
                           const vector<LogicalType> &sql_types, const SelectBindState &bind_state) {
	for (auto &bound_mod : result.modifiers) {
		switch (bound_mod->type) {
		case ResultModifierType::DISTINCT_MODIFIER: {
			auto &distinct = bound_mod->Cast<BoundDistinctModifier>();
			// set types of distinct targets
			for (auto &expr : distinct.target_distincts) {
				expr = FinalizeBindOrderExpression(std::move(expr), table_index, names, sql_types, bind_state);
				if (!expr) {
					throw InternalException("DISTINCT ON ORDER BY ALL not supported");
				}
			}
			for (auto &expr : distinct.target_distincts) {
				ExpressionBinder::PushCollation(context, expr, expr->return_type);
			}
			break;
		}
		case ResultModifierType::LIMIT_MODIFIER: {
			auto &limit = bound_mod->Cast<BoundLimitModifier>();
			AssignReturnType(limit.limit_val.GetExpression(), table_index, names, sql_types, bind_state);
			AssignReturnType(limit.offset_val.GetExpression(), table_index, names, sql_types, bind_state);
			break;
		}
		case ResultModifierType::ORDER_MODIFIER: {
			auto &order = bound_mod->Cast<BoundOrderModifier>();
			bool order_by_all = false;
			for (auto &order_node : order.orders) {
				auto &expr = order_node.expression;
				expr = FinalizeBindOrderExpression(std::move(expr), table_index, names, sql_types, bind_state);
				if (!expr) {
					order_by_all = true;
				}
			}
			if (order_by_all) {
				D_ASSERT(order.orders.size() == 1);
				auto order_type = order.orders[0].type;
				auto null_order = order.orders[0].null_order;
				order.orders.clear();
				for (idx_t i = 0; i < sql_types.size(); i++) {
					auto expr = make_uniq<BoundColumnRefExpression>(sql_types[i], ColumnBinding(table_index, i));
					if (i < names.size()) {
						expr->SetAlias(names[i]);
					}
					order.orders.emplace_back(order_type, null_order, std::move(expr));
				}
			}
			for (auto &order_node : order.orders) {
				auto &expr = order_node.expression;
				ExpressionBinder::PushCollation(context, order_node.expression, expr->return_type);
			}
			break;
		}
		default:
			break;
		}
	}
}

unique_ptr<BoundQueryNode> Binder::BindNode(SelectNode &statement) {
	D_ASSERT(statement.from_table);

	// first bind the FROM table statement
	auto from = std::move(statement.from_table);
	auto from_table = Bind(*from);
	return BindSelectNode(statement, std::move(from_table));
}

void Binder::BindWhereStarExpression(unique_ptr<ParsedExpression> &expr) {
	// expand any expressions in the upper AND recursively
	if (expr->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		auto &conj = expr->Cast<ConjunctionExpression>();
		for (auto &child : conj.children) {
			BindWhereStarExpression(child);
		}
		return;
	}
	if (expr->GetExpressionType() == ExpressionType::STAR) {
		auto &star = expr->Cast<StarExpression>();
		if (!star.columns) {
			throw ParserException("STAR expression is not allowed in the WHERE clause. Use COLUMNS(*) instead.");
		}
	}
	// expand the stars for this expression
	vector<unique_ptr<ParsedExpression>> new_conditions;
	ExpandStarExpression(std::move(expr), new_conditions);
	if (new_conditions.empty()) {
		throw ParserException("COLUMNS expansion resulted in empty set of columns");
	}

	// set up an AND conjunction between the expanded conditions
	expr = std::move(new_conditions[0]);
	for (idx_t i = 1; i < new_conditions.size(); i++) {
		auto and_conj = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(expr),
		                                                 std::move(new_conditions[i]));
		expr = std::move(and_conj);
	}
}

// Check if a parsed expression references any DECIDE variable.
static bool ReferencesDecideVariable(ParsedExpression &expr,
                                     const case_insensitive_map_t<idx_t> &variables) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		return variables.count(colref.GetColumnName()) > 0;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<ParsedExpression> &child) {
		if (!found && child) {
			found = ReferencesDecideVariable(*child, variables);
		}
	});
	return found;
}

// ABS linearization is now fully handled by DecideOptimizer::RewriteAbs (post-binding).
// The binder binds ABS as a normal BoundFunctionExpression; the optimizer detects it,
// creates auxiliary REAL variables, and generates linearization constraints.

// MIN/MAX constraint and objective rewriting is now fully handled by
// DecideOptimizer::RewriteMinMax (post-binding). The binder binds MIN/MAX as normal
// BoundAggregateExpression nodes; the optimizer classifies easy/hard, creates
// indicator variables, and rewrites to SUM.

// Rewrite qualified `Table.var` ColumnRefs into bare `var` ColumnRefs when
// `Table.var` matches a registered table-scoped DECIDE variable. After this
// pass, every reference to a scoped decision variable is unqualified, so the
// regular DuckDB binder (used for SELECT/ORDER/etc.) and the per-row branch
// of DecideConstraintsBinder both resolve it through the generic
// `decide_variables` binding instead of routing `Table` to the real table
// binding (which has no such column). The aggregate-SUM path already strips
// qualifiers via SymEngine round-trip in NormalizeDecideConstraints; this
// pre-pass extends that behavior uniformly to per-row constraints, the
// SELECT list, and the objective.
static void RewriteScopedVarRefs(unique_ptr<ParsedExpression> &expr,
                                  const case_insensitive_map_t<idx_t> &variables) {
	if (!expr) {
		return;
	}
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (colref.IsQualified()) {
			string qualified = colref.GetTableName() + "." + colref.GetColumnName();
			if (variables.count(qualified)) {
				auto alias = colref.GetAlias();
				expr = make_uniq<ColumnRefExpression>(colref.GetColumnName());
				if (!alias.empty()) {
					expr->alias = alias;
				}
				return;
			}
		}
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		RewriteScopedVarRefs(child, variables);
	});
}

// Rewrite IN domain constraints on DECIDE variables into auxiliary binary indicator
// variables + cardinality/linking constraints.
// x IN (v1, v2, ..., vK) becomes:
//   z_1 + z_2 + ... + z_K = 1           (exactly one indicator active)
//   x - v1*z_1 - v2*z_2 - ... - vK*z_K = 0  (x takes the selected value)
// Each z_i is a BOOLEAN auxiliary variable.
static void RewriteInDomain(unique_ptr<ParsedExpression> &expr,
                             case_insensitive_map_t<idx_t> &variables,
                             vector<bool> &is_boolean_var,
                             vector<string> &var_names,
                             vector<LogicalType> &var_types,
                             idx_t &in_counter) {
	if (expr->GetExpressionClass() == ExpressionClass::OPERATOR) {
		auto &op = expr->Cast<OperatorExpression>();
		if (op.type == ExpressionType::COMPARE_IN && !op.children.empty()) {
			auto &target = *op.children[0];
			if (target.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
				auto &colref = target.Cast<ColumnRefExpression>();
				auto it = variables.find(colref.GetColumnName());
				if (it != variables.end()) {
					string var_name = colref.GetColumnName();
					idx_t K = op.children.size() - 1; // number of domain values

					if (K == 0) {
						throw BinderException(*expr, "IN domain constraint requires at least one value.");
					}

					// Validate: IN values must not reference DECIDE variables
					// (would create non-linear x * z terms)
					for (idx_t i = 1; i <= K; i++) {
						if (ReferencesDecideVariable(*op.children[i], variables)) {
							throw BinderException(*expr,
							    "IN domain constraints on DECIDE variables are not yet supported. "
							    "The values in the IN list must be constants or table columns, "
							    "not DECIDE variables.");
						}
					}

					// Optimization: x IN (0, 1) on BOOLEAN var → trivially satisfied, skip
					if (is_boolean_var[it->second] && K == 2) {
						auto &v0 = *op.children[1];
						auto &v1 = *op.children[2];
						if (v0.GetExpressionClass() == ExpressionClass::CONSTANT &&
						    v1.GetExpressionClass() == ExpressionClass::CONSTANT) {
							double d0 = v0.Cast<ConstantExpression>().value.GetValue<double>();
							double d1 = v1.Cast<ConstantExpression>().value.GetValue<double>();
							if ((d0 == 0.0 && d1 == 1.0) || (d0 == 1.0 && d1 == 0.0)) {
								// Trivially satisfied — replace with x >= 0 (always true for BOOLEAN)
								expr = make_uniq<ComparisonExpression>(
								    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
								    make_uniq<ColumnRefExpression>(var_name),
								    make_uniq<ConstantExpression>(Value::INTEGER(0)));
								return;
							}
						}
					}

					// Single value: x IN (v) → x = v
					if (K == 1) {
						expr = make_uniq<ComparisonExpression>(
						    ExpressionType::COMPARE_EQUAL,
						    make_uniq<ColumnRefExpression>(var_name),
						    op.children[1]->Copy());
						return;
					}

					// General case: create K auxiliary indicator variables
					// Note: these use INTEGER (not BOOLEAN) because the binder generates
					// arithmetic expressions (z_1 + z_2 + ... = 1) that require integer arithmetic.
					// Bounds (0 <= z <= 1) are generated explicitly below.
					vector<string> ind_names;
					for (idx_t i = 0; i < K; i++) {
						string ind_name = "__in_ind_" + var_name + "_" + to_string(in_counter) + "_" + to_string(i) + "__";
						idx_t ind_idx = var_names.size();
						var_names.push_back(ind_name);
						var_types.push_back(LogicalType::INTEGER);
						is_boolean_var.push_back(true);
						variables.emplace(ind_name, ind_idx);
						ind_names.push_back(ind_name);
					}
					in_counter++;

					// Cardinality constraint: z_1 + z_2 + ... + z_K = 1
					unique_ptr<ParsedExpression> cardinality_lhs = make_uniq<ColumnRefExpression>(ind_names[0]);
					for (idx_t i = 1; i < K; i++) {
						vector<unique_ptr<ParsedExpression>> add_args;
						add_args.push_back(std::move(cardinality_lhs));
						add_args.push_back(make_uniq<ColumnRefExpression>(ind_names[i]));
						cardinality_lhs = make_uniq<FunctionExpression>("+", std::move(add_args));
					}
					auto cardinality_constraint = make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL,
					    std::move(cardinality_lhs),
					    make_uniq<ConstantExpression>(Value::INTEGER(1)));

					// Linking constraint: x + (-v1)*z_1 + (-v2)*z_2 + ... + (-vK)*z_K = 0
					// Uses only + and * for clarity (though ExtractTerms does handle subtraction)
					unique_ptr<ParsedExpression> linking_lhs = make_uniq<ColumnRefExpression>(var_name);
					for (idx_t i = 0; i < K; i++) {
						// Build (-vi) * zi term: negate value inside coefficient
						vector<unique_ptr<ParsedExpression>> neg_args;
						neg_args.push_back(make_uniq<ConstantExpression>(Value::INTEGER(0)));
						neg_args.push_back(op.children[i + 1]->Copy());
						auto neg_value = make_uniq<FunctionExpression>("-", std::move(neg_args));

						vector<unique_ptr<ParsedExpression>> mul_args;
						mul_args.push_back(std::move(neg_value));
						mul_args.push_back(make_uniq<ColumnRefExpression>(ind_names[i]));
						auto term = make_uniq<FunctionExpression>("*", std::move(mul_args));

						// linking_lhs = linking_lhs + term
						vector<unique_ptr<ParsedExpression>> add_args;
						add_args.push_back(std::move(linking_lhs));
						add_args.push_back(std::move(term));
						linking_lhs = make_uniq<FunctionExpression>("+", std::move(add_args));
					}
					auto linking_constraint = make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_EQUAL,
					    std::move(linking_lhs),
					    make_uniq<ConstantExpression>(Value::INTEGER(0)));

					// Replace the IN expression with AND of both constraints
					expr = make_uniq<ConjunctionExpression>(
					    ExpressionType::CONJUNCTION_AND,
					    std::move(cardinality_constraint),
					    std::move(linking_constraint));
					return;
				}
			}
		}
	}
	// Recurse into children (handles WHEN/PER wrappers and conjunctions)
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		RewriteInDomain(child, variables, is_boolean_var, var_names, var_types, in_counter);
	});
}

// NOTE: RewriteNotEqual has been moved to DecideOptimizer (src/optimizer/decide/decide_optimizer.cpp).
// It now operates on BoundExpressions post-binding instead of ParsedExpressions pre-binding.

unique_ptr<BoundQueryNode> Binder::BindSelectNode(SelectNode &statement, unique_ptr<BoundTableRef> from_table) {
	D_ASSERT(from_table);
	D_ASSERT(!statement.from_table);
	auto result = make_uniq<BoundSelectNode>();
	result->projection_index = GenerateTableIndex();
	result->group_index = GenerateTableIndex();
	result->aggregate_index = GenerateTableIndex();
	result->groupings_index = GenerateTableIndex();
	result->window_index = GenerateTableIndex();
	result->prune_index = GenerateTableIndex();
    result->decide_index = GenerateTableIndex();

	result->from_table = std::move(from_table);
	// bind the sample clause
	if (statement.sample) {
		result->sample_options = std::move(statement.sample);
	}

    // Bind DECIDE clause before ExpandStarExpression
    if (statement.HasDecideClause()) {

		

        case_insensitive_map_t<idx_t> decide_variable_names;
        vector<string> var_names;
        vector<LogicalType> var_types;
        vector<bool> is_boolean_var;  // Track which variables are BOOLEAN for generating bounds

        // Table-scoped variable tracking
        vector<EntityScopeInfo> entity_scopes;
        vector<idx_t> variable_entity_scope;  // per variable: INVALID_INDEX or index into entity_scopes
        // Map table alias → entity_scopes index (to share scope info for multiple vars on same table)
        case_insensitive_map_t<idx_t> table_scope_map;

        for (const auto& expr_ptr : statement.decide_variables) {
            string name;
            string table_name;  // empty for row-scoped variables
            string type_marker = "integer_variable";  // Default type

            // Handle typed variable declarations (ComparisonExpression from "x IS INTEGER")
            if (expr_ptr->GetExpressionClass() == ExpressionClass::COMPARISON) {
                const auto& comp = expr_ptr->Cast<duckdb::ComparisonExpression>();

                // LHS should be the variable name (ColumnRefExpression)
                if (comp.left->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
                    throw BinderException(*expr_ptr, "Invalid DECIDE variable declaration: expected variable name on left side.");
                }
                const auto& colref = comp.left->Cast<duckdb::ColumnRefExpression>();
                name = colref.GetColumnName();
                if (colref.IsQualified()) {
                    table_name = colref.GetTableName();
                }

                // RHS should be the type marker (ConstantExpression with string value)
                if (comp.right->GetExpressionClass() == ExpressionClass::CONSTANT) {
                    const auto& const_expr = comp.right->Cast<duckdb::ConstantExpression>();
                    if (const_expr.value.type() == LogicalType::VARCHAR) {
                        type_marker = const_expr.value.ToString();
                    }
                }
            } else if (expr_ptr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
                // Plain variable name without type (backward compatibility)
                const auto& colref = expr_ptr->Cast<duckdb::ColumnRefExpression>();
                name = colref.GetColumnName();
                if (colref.IsQualified()) {
                    table_name = colref.GetTableName();
                }
            } else {
                throw BinderException(*expr_ptr, "Invalid DECIDE variable declaration.");
            }

            if (bind_context.GetMatchingBinding(name)) {
                throw BinderException(*expr_ptr, "DECIDE variable '%s' conflicts with an existing column name.", name);
            }
            if (decide_variable_names.count(name)) {
                throw BinderException(*expr_ptr, "Duplicate DECIDE variable name '%s'.", name);
            }

            idx_t var_idx = var_names.size();

            // Register under unqualified name (always)
            decide_variable_names.emplace(name, var_idx);

            // Handle table-scoped variable
            idx_t scope_idx = DConstants::INVALID_INDEX;
            if (!table_name.empty()) {
                // Resolve table in the bind context
                ErrorData error;
                auto binding = bind_context.GetBinding(table_name, error);
                if (!binding) {
                    throw BinderException(*expr_ptr,
                        "DECIDE variable '%s.%s': table '%s' not found in FROM clause.",
                        table_name, name, table_name);
                }

                // Also register under qualified name so constraints can use T.x syntax
                string qualified_name = table_name + "." + name;
                decide_variable_names.emplace(qualified_name, var_idx);

                // Reuse existing entity scope if another variable already scoped to this table
                auto scope_it = table_scope_map.find(table_name);
                if (scope_it != table_scope_map.end()) {
                    scope_idx = scope_it->second;
                    entity_scopes[scope_idx].scoped_variable_indices.push_back(var_idx);
                } else {
                    // Create new EntityScopeInfo for this table.
                    // Register every source-table column in the TableBinding's
                    // bound_column_ids via GetColumnBinding. This forces the
                    // columns into the scan's column_ids and returns the correct
                    // ColumnBinding (table_index, position-in-col_ids). Refinement
                    // below may drop some; whatever survives gets its columns
                    // kept in the scan via entity_key_expressions.
                    EntityScopeInfo scope_info;
                    scope_info.table_alias = table_name;
                    scope_info.source_table_index = binding->index;
                    if (binding->binding_type == BindingType::TABLE) {
                        auto &tbl_binding = binding->Cast<TableBinding>();
                        for (idx_t col_idx = 0; col_idx < binding->names.size(); col_idx++) {
                            scope_info.entity_key_column_types.push_back(binding->types[col_idx]);
                            scope_info.entity_key_bindings.push_back(
                                tbl_binding.GetColumnBinding(col_idx));
                        }
                    } else {
                        // Non-base table (e.g., subquery): fall back to raw bindings.
                        for (idx_t col_idx = 0; col_idx < binding->names.size(); col_idx++) {
                            scope_info.entity_key_column_types.push_back(binding->types[col_idx]);
                            scope_info.entity_key_bindings.push_back(
                                ColumnBinding(binding->index, col_idx));
                        }
                    }
                    scope_info.scoped_variable_indices.push_back(var_idx);
                    scope_idx = entity_scopes.size();
                    table_scope_map.emplace(table_name, scope_idx);
                    entity_scopes.push_back(std::move(scope_info));
                }
            }

            variable_entity_scope.push_back(scope_idx);
            var_names.push_back(name);
            var_types.push_back(type_marker == "real_variable" ? LogicalType::DOUBLE : LogicalType::INTEGER);
            is_boolean_var.push_back(type_marker == "bool_variable");
        }
        
        // Generate implicit bounds constraints for BOOLEAN variables: 0 <= x <= 1
        // Prepend these to the existing constraints
        for (idx_t i = 0; i < var_names.size(); i++) {
            if (is_boolean_var[i]) {
                auto x_ref = make_uniq<ColumnRefExpression>(var_names[i]);
                auto zero = make_uniq<ConstantExpression>(Value::INTEGER(0));
                auto one = make_uniq<ConstantExpression>(Value::INTEGER(1));
                
                auto lower_bound = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO, x_ref->Copy(), std::move(zero));
                auto upper_bound = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(x_ref), std::move(one));
                
                auto bounds = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower_bound), std::move(upper_bound));
                
                // Prepend to constraints
                if (statement.decide_constraints) {
                    statement.decide_constraints = make_uniq<ConjunctionExpression>(
                        ExpressionType::CONJUNCTION_AND, 
                        std::move(bounds), 
                        std::move(statement.decide_constraints)
                    );
                } else {
                    statement.decide_constraints = std::move(bounds);
                }
            }
        }


        // Capture user var count BEFORE any rewrites that add auxiliary variables
        idx_t num_user_vars = var_names.size();

        // Strip table qualifiers from `Table.var` references that match a
        // registered scoped DECIDE variable. Applied to the SELECT list, the
        // SUCH THAT tree, and the objective tree so that downstream binders
        // (regular DuckDB binder for SELECT, per-row branch of
        // DecideConstraintsBinder) see bare `var` instead of `Table.var`.
        // No-op when no scoped variables are declared.
        RewriteScopedVarRefs(statement.decide_constraints, decide_variable_names);
        RewriteScopedVarRefs(statement.decide_objective, decide_variable_names);
        for (auto &sel_expr : statement.select_list) {
            RewriteScopedVarRefs(sel_expr, decide_variable_names);
        }

        // MIN/MAX rewrite is handled by DecideOptimizer::RewriteMinMax (post-binding).

        // Rewrite IN domain constraints: x IN (v1, ..., vK) → indicator variables + constraints
        if (statement.decide_constraints) {
            idx_t in_counter = 0;
            RewriteInDomain(statement.decide_constraints, decide_variable_names,
                            is_boolean_var, var_names, var_types, in_counter);
        }

        // NOT-EQUAL (<>) indicator variables are now created by DecideOptimizer
        // (runs after binding, creates BOOLEAN indicators directly on LogicalDecide)

        // MIN/MAX indicator bounds are handled automatically by BOOLEAN type
        // in physical_decide.cpp. NE indicator bounds are handled by DecideOptimizer.
        // IN indicators use INTEGER (needed for arithmetic in parsed constraints),
        // so they need explicit bounds (0 <= z <= 1).
        for (idx_t i = num_user_vars; i < var_names.size(); i++) {
            if (!is_boolean_var[i]) continue;
            if (var_types[i] == LogicalType::BOOLEAN) continue; // auto-bounds via physical_decide
            auto z_ref = make_uniq<ColumnRefExpression>(var_names[i]);
            auto zero = make_uniq<ConstantExpression>(Value::INTEGER(0));
            auto one = make_uniq<ConstantExpression>(Value::INTEGER(1));
            auto lower = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO, z_ref->Copy(), std::move(zero));
            auto upper = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(z_ref), std::move(one));
            auto bounds = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower), std::move(upper));
            if (statement.decide_constraints) {
                statement.decide_constraints = make_uniq<ConjunctionExpression>(
                    ExpressionType::CONJUNCTION_AND, std::move(bounds), std::move(statement.decide_constraints));
            } else {
                statement.decide_constraints = std::move(bounds);
            }
        }

        // MIN/MAX objective rewrite is handled by DecideOptimizer::RewriteMinMaxObjective (post-binding).
        // MIN/MAX aggregates pass through normalization and binding as normal functions.

        // ABS linearization is now fully handled by DecideOptimizer::RewriteAbs (post-binding).
        // ABS(expr) passes through normalization and binding as a normal function.
        idx_t num_auxiliary_vars = var_names.size() - num_user_vars;

        if (statement.decide_constraints) {
            // deb("-- Parsed SUCH THAT (DOT) --\n", ExpressionToDot(*statement.decide_constraints));
            // Reject scalar functions like sqrt(x), exp(x), floor(x) wrapping a
            // DECIDE variable. Must run before NormalizeDecideConstraints — the
            // symbolic layer throws InternalException on unknown functions, and
            // the per-row path would silently strip them.
            ValidateDecideNoNonLinearScalar(context, *statement.decide_constraints, decide_variable_names);
            statement.decide_constraints = NormalizeDecideConstraints(*statement.decide_constraints, decide_variable_names);
            // deb("-- Normalized SUCH THAT (DOT) --\n", ExpressionToDot(*statement.decide_constraints));
        }
        if (statement.decide_objective) {
            // deb("-- Parsed OBJECTIVE (DOT) --\n", ExpressionToDot(*statement.decide_objective));
            ValidateDecideNoNonLinearScalar(context, *statement.decide_objective, decide_variable_names);
            double peeled_offset = 0.0;
            statement.decide_objective = NormalizeDecideObjective(*statement.decide_objective,
                                                                   decide_variable_names,
                                                                   peeled_offset);
            result->objective_constant_offset = peeled_offset;
            // deb("-- Normalized OBJECTIVE (DOT) --\n", ExpressionToDot(*statement.decide_objective));
        }

        bind_context.AddGenericBinding(result->decide_index, "decide_variables", var_names, var_types);
        // Isolate with brackets to avoid multiple active binders.
        {
            DecideConstraintsBinder decide_constraints_binder (*this, context, decide_variable_names);
            unique_ptr<ParsedExpression> constraints = std::move(statement.decide_constraints);
            result->decide_constraints = decide_constraints_binder.Bind(constraints);
            // Types are now determined from the DECIDE clause, not from constraint binding
        }
        if (statement.decide_objective) {
            DecideObjectiveBinder decide_objective_binder (*this, context, decide_variable_names);
            decide_objective_binder.decide_sense = statement.decide_sense;
            unique_ptr<ParsedExpression> objective = std::move(statement.decide_objective);
            result->decide_objective = decide_objective_binder.Bind(objective);
        }
        result->decide_sense = statement.decide_sense;
        // Update types in bind context to reflect the determined types from DECIDE clause
        bind_context.GetBindingsList().back()->types = var_types;
        for (idx_t i = 0; i < var_names.size(); i++) {
            auto bound_col_ref = make_uniq<BoundColumnRefExpression>(
                var_names[i],
                var_types[i],
                ColumnBinding(result->decide_index, i)
            );
            result->decide_variables.push_back(std::move(bound_col_ref));
        }
        result->num_auxiliary_vars = num_auxiliary_vars;
        result->is_boolean_var = is_boolean_var;

        // Refine entity keys: exclude scoped-table columns that appear as coefficient
        // data in constraints/objectives. These are dependent data columns, not identity columns.
        // Without this refinement, CTEs with multiple columns would use ALL columns as
        // entity key, making every row its own entity and defeating table-scoping.
        if (!entity_scopes.empty()) {
            // Collect column bindings from scoped tables that appear in bound expressions
            unordered_set<uint64_t> data_columns;  // encode as (table_index << 32) | col_index
            auto collect_data_cols = [&](Expression &expr) {
                if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
                    auto &col_ref = expr.Cast<BoundColumnRefExpression>();
                    auto &binding = col_ref.binding;
                    // Check if this column belongs to any scoped table
                    for (auto &scope : entity_scopes) {
                        if (binding.table_index == scope.source_table_index) {
                            data_columns.insert((static_cast<uint64_t>(binding.table_index) << 32) |
                                                static_cast<uint64_t>(binding.column_index));
                        }
                    }
                }
            };
            if (result->decide_constraints) {
                ExpressionIterator::EnumerateExpression(result->decide_constraints, collect_data_cols);
            }
            if (result->decide_objective) {
                ExpressionIterator::EnumerateExpression(result->decide_objective, collect_data_cols);
            }

            // Remove data columns from entity keys (keep at least one key column)
            for (auto &scope : entity_scopes) {
                vector<ColumnBinding> refined_bindings;
                vector<LogicalType> refined_types;
                for (idx_t k = 0; k < scope.entity_key_bindings.size(); k++) {
                    auto &b = scope.entity_key_bindings[k];
                    uint64_t key = (static_cast<uint64_t>(b.table_index) << 32) |
                                   static_cast<uint64_t>(b.column_index);
                    if (data_columns.find(key) == data_columns.end()) {
                        refined_bindings.push_back(b);
                        refined_types.push_back(scope.entity_key_column_types[k]);
                    }
                }
                // Only apply refinement if at least one column survives
                if (!refined_bindings.empty()) {
                    scope.entity_key_bindings = std::move(refined_bindings);
                    scope.entity_key_column_types = std::move(refined_types);
                }
            }
        }

        // Create BoundColumnRefExpressions for entity-key columns.
        // These are stored on LogicalDecide so that DuckDB's binder column-reference
        // tracking (used by the initial Get column_id selection AND by the column
        // pruner's rebinding pass) treats entity-key columns as live. Without this,
        // entity-key columns that don't also appear in SELECT/WHERE/constraints/
        // objective get pruned by the Binder, leaving the VarIndexer with a
        // degenerate key set that silently collapses distinct entities.
        for (auto &scope : entity_scopes) {
            for (idx_t k = 0; k < scope.entity_key_bindings.size(); k++) {
                result->entity_key_expressions.push_back(
                    make_uniq<BoundColumnRefExpression>(
                        "entity_key_" + scope.table_alias,
                        scope.entity_key_column_types[k],
                        scope.entity_key_bindings[k]));
            }
        }

        // Store table-scoped variable metadata
        result->entity_scopes = std::move(entity_scopes);
        result->variable_entity_scope = variable_entity_scope;

        // ne_indicator_indices is now populated by DecideOptimizer (post-binding)
        // minmax_indicator_links, flat_objective_agg/is_easy, per_inner/outer_agg/is_easy
        // are now populated by DecideOptimizer::RewriteMinMax (post-binding)

        // Hide auxiliary vars from SELECT * by truncating the bind context binding.
        // The decide_variables vector still has ALL vars (user + aux) for the execution layer,
        // but the bind context only exposes user vars for star expansion.
        if (num_auxiliary_vars > 0) {
            auto &binding = *bind_context.GetBindingsList().back();
            for (idx_t i = num_user_vars; i < var_names.size(); i++) {
                binding.name_map.erase(var_names[i]);
            }
            binding.names.resize(num_user_vars);
            binding.types.resize(num_user_vars);
        }
    }

	// visit the select list and expand any "*" statements
	vector<unique_ptr<ParsedExpression>> new_select_list;
	ExpandStarExpressions(statement.select_list, new_select_list);

	if (new_select_list.empty()) {
		throw BinderException("SELECT list is empty after resolving * expressions!");
	}
	statement.select_list = std::move(new_select_list);

	auto &bind_state = result->bind_state;
	for (idx_t i = 0; i < statement.select_list.size(); i++) {
		auto &expr = statement.select_list[i];
		result->names.push_back(expr->GetName());
		ExpressionBinder::QualifyColumnNames(*this, expr);
		if (!expr->GetAlias().empty()) {
			bind_state.alias_map[expr->GetAlias()] = i;
			result->names[i] = expr->GetAlias();
		}
		bind_state.projection_map[*expr] = i;
		bind_state.original_expressions.push_back(expr->Copy());
	}
	result->column_count = statement.select_list.size();

	// first visit the WHERE clause
	// the WHERE clause happens before the GROUP BY, PROJECTION or HAVING clauses
	if (statement.where_clause) {
		// bind any star expressions in the WHERE clause
		BindWhereStarExpression(statement.where_clause);

		ColumnAliasBinder alias_binder(bind_state);
		WhereBinder where_binder(*this, context, &alias_binder);
		unique_ptr<ParsedExpression> condition = std::move(statement.where_clause);
		result->where_clause = where_binder.Bind(condition);
	}

	// now bind all the result modifiers; including DISTINCT and ORDER BY targets
	OrderBinder order_binder({*this}, statement, bind_state);
	PrepareModifiers(order_binder, statement, *result);

	vector<unique_ptr<ParsedExpression>> unbound_groups;
	BoundGroupInformation info;
	auto &group_expressions = statement.groups.group_expressions;
	if (!group_expressions.empty()) {
		// the statement has a GROUP BY clause, bind it
		unbound_groups.resize(group_expressions.size());
		GroupBinder group_binder(*this, context, statement, result->group_index, bind_state, info.alias_map);
		for (idx_t i = 0; i < group_expressions.size(); i++) {

			// we keep a copy of the unbound expression;
			// we keep the unbound copy around to check for group references in the SELECT and HAVING clause
			// the reason we want the unbound copy is because we want to figure out whether an expression
			// is a group reference BEFORE binding in the SELECT/HAVING binder
			group_binder.unbound_expression = group_expressions[i]->Copy();
			group_binder.bind_index = i;

			// bind the groups
			LogicalType group_type;
			auto bound_expr = group_binder.Bind(group_expressions[i], &group_type);
			D_ASSERT(bound_expr->return_type.id() != LogicalTypeId::INVALID);

			// find out whether the expression contains a subquery, it can't be copied if so
			auto &bound_expr_ref = *bound_expr;
			bool contains_subquery = bound_expr_ref.HasSubquery();

			// push a potential collation, if necessary
			bool requires_collation = ExpressionBinder::PushCollation(context, bound_expr, group_type);
			if (!contains_subquery && requires_collation) {
				// if there is a collation on a group x, we should group by the collated expr,
				// but also push a first(x) aggregate in case x is selected (uncollated)
				info.collated_groups[i] = result->aggregates.size();

				auto first_fun = FirstFunctionGetter::GetFunction(bound_expr_ref.return_type);
				vector<unique_ptr<Expression>> first_children;
				// FIXME: would be better to just refer to this expression, but for now we copy
				first_children.push_back(bound_expr_ref.Copy());

				FunctionBinder function_binder(*this);
				auto function = function_binder.BindAggregateFunction(first_fun, std::move(first_children));
				function->SetAlias("__collated_group");
				result->aggregates.push_back(std::move(function));
			}
			result->groups.group_expressions.push_back(std::move(bound_expr));

			// in the unbound expression we DO bind the table names of any ColumnRefs
			// we do this to make sure that "table.a" and "a" are treated the same
			// if we wouldn't do this then (SELECT test.a FROM test GROUP BY a) would not work because "test.a" <> "a"
			// hence we convert "a" -> "test.a" in the unbound expression
			unbound_groups[i] = std::move(group_binder.unbound_expression);
			ExpressionBinder::QualifyColumnNames(*this, unbound_groups[i]);
			info.map[*unbound_groups[i]] = i;
		}
	}
	result->groups.grouping_sets = std::move(statement.groups.grouping_sets);

	// bind the HAVING clause, if any
	if (statement.having) {
		HavingBinder having_binder(*this, context, *result, info, statement.aggregate_handling);
		ExpressionBinder::QualifyColumnNames(having_binder, statement.having);
		result->having = having_binder.Bind(statement.having);
	}

	// bind the QUALIFY clause, if any
	vector<BoundColumnReferenceInfo> bound_qualify_columns;
	if (statement.qualify) {
		if (statement.aggregate_handling == AggregateHandling::FORCE_AGGREGATES) {
			throw BinderException("Combining QUALIFY with GROUP BY ALL is not supported yet");
		}
		QualifyBinder qualify_binder(*this, context, *result, info);
		ExpressionBinder::QualifyColumnNames(*this, statement.qualify);
		result->qualify = qualify_binder.Bind(statement.qualify);
		if (qualify_binder.HasBoundColumns()) {
			if (qualify_binder.BoundAggregates()) {
				throw BinderException("Cannot mix aggregates with non-aggregated columns!");
			}
			bound_qualify_columns = qualify_binder.GetBoundColumns();
		}
	}

	// after that, we bind to the SELECT list
	SelectBinder select_binder(*this, context, *result, info);

	// if we expand select-list expressions, e.g., via UNNEST, then we need to possibly
	// adjust the column index of the already bound ORDER BY modifiers, and not only set their types
	vector<idx_t> group_by_all_indexes;
	vector<string> new_names;
	vector<LogicalType> internal_sql_types;

	for (idx_t i = 0; i < statement.select_list.size(); i++) {
		bool is_window = statement.select_list[i]->IsWindow();
		idx_t unnest_count = result->unnests.size();
		LogicalType result_type;
		auto expr = select_binder.Bind(statement.select_list[i], &result_type, true);
		bool is_original_column = i < result->column_count;
		bool can_group_by_all =
		    statement.aggregate_handling == AggregateHandling::FORCE_AGGREGATES && is_original_column;
		result->bound_column_count++;

		if (expr->GetExpressionType() == ExpressionType::BOUND_EXPANDED) {
			if (!is_original_column) {
				throw BinderException("UNNEST of struct cannot be used in ORDER BY/DISTINCT ON clause");
			}
			if (statement.aggregate_handling == AggregateHandling::FORCE_AGGREGATES) {
				throw BinderException("UNNEST of struct cannot be combined with GROUP BY ALL");
			}

			auto &expanded = expr->Cast<BoundExpandedExpression>();
			auto &struct_expressions = expanded.expanded_expressions;
			D_ASSERT(!struct_expressions.empty());

			for (auto &struct_expr : struct_expressions) {
				new_names.push_back(struct_expr->GetName());
				result->types.push_back(struct_expr->return_type);
				internal_sql_types.push_back(struct_expr->return_type);
				result->select_list.push_back(std::move(struct_expr));
			}
			bind_state.AddExpandedColumn(struct_expressions.size());
			continue;
		}

		if (expr->IsVolatile()) {
			bind_state.SetExpressionIsVolatile(i);
		}
		if (expr->HasSubquery()) {
			bind_state.SetExpressionHasSubquery(i);
		}
		bind_state.AddRegularColumn();

		if (can_group_by_all && select_binder.HasBoundColumns()) {
			if (select_binder.BoundAggregates()) {
				throw BinderException("Cannot mix aggregates with non-aggregated columns!");
			}
			if (is_window) {
				throw BinderException("Cannot group on a window clause");
			}
			if (result->unnests.size() > unnest_count) {
				throw BinderException("Cannot group on an UNNEST or UNLIST clause");
			}
			// we are forcing aggregates, and the node has columns bound
			// this entry becomes a group
			group_by_all_indexes.push_back(i);
		}

		result->select_list.push_back(std::move(expr));
		if (is_original_column) {
			new_names.push_back(std::move(result->names[i]));
			result->types.push_back(result_type);
		}
		internal_sql_types.push_back(result_type);

		if (can_group_by_all) {
			select_binder.ResetBindings();
		}
	}

	// push the GROUP BY ALL expressions into the group set

	for (auto &group_by_all_index : group_by_all_indexes) {
		auto &expr = result->select_list[group_by_all_index];
		auto group_ref = make_uniq<BoundColumnRefExpression>(
		    expr->return_type, ColumnBinding(result->group_index, result->groups.group_expressions.size()));
		result->groups.group_expressions.push_back(std::move(expr));
		expr = std::move(group_ref);
	}
	set<idx_t> group_by_all_indexes_set;
	if (!group_by_all_indexes.empty()) {
		idx_t num_set_indexes = result->groups.group_expressions.size();
		for (idx_t i = 0; i < num_set_indexes; i++) {
			group_by_all_indexes_set.insert(i);
		}
		D_ASSERT(result->groups.grouping_sets.empty());
		result->groups.grouping_sets.push_back(group_by_all_indexes_set);
	}
	result->column_count = new_names.size();
	result->names = std::move(new_names);
	result->need_prune = result->select_list.size() > result->column_count;

	// in the normal select binder, we bind columns as if there is no aggregation
	// i.e. in the query [SELECT i, SUM(i) FROM integers;] the "i" will be bound as a normal column
	// since we have an aggregation, we need to either (1) throw an error, or (2) wrap the column in a FIRST() aggregate
	// we choose the former one [CONTROVERSIAL: this is the PostgreSQL behavior]
	if (!result->groups.group_expressions.empty() || !result->aggregates.empty() || statement.having ||
	    !result->groups.grouping_sets.empty()) {
		if (statement.aggregate_handling == AggregateHandling::NO_AGGREGATES_ALLOWED) {
			throw BinderException("Aggregates cannot be present in a Project relation!");
		} else {
			vector<BoundColumnReferenceInfo> bound_columns;
			if (select_binder.HasBoundColumns()) {
				bound_columns = select_binder.GetBoundColumns();
			}
			for (auto &bound_qualify_col : bound_qualify_columns) {
				bound_columns.push_back(bound_qualify_col);
			}
			if (!bound_columns.empty()) {
				string error;
				error = "column \"%s\" must appear in the GROUP BY clause or must be part of an aggregate function.";
				if (statement.aggregate_handling == AggregateHandling::FORCE_AGGREGATES) {
					error += "\nGROUP BY ALL will only group entries in the SELECT list. Add it to the SELECT list or "
					         "GROUP BY this entry explicitly.";
					throw BinderException(bound_columns[0].query_location, error, bound_columns[0].name);
				} else {
					error +=
					    "\nEither add it to the GROUP BY list, or use \"ANY_VALUE(%s)\" if the exact value of \"%s\" "
					    "is not important.";
					throw BinderException(bound_columns[0].query_location, error, bound_columns[0].name,
					                      bound_columns[0].name, bound_columns[0].name);
				}
			}
		}
	}

	// QUALIFY clause requires at least one window function to be specified in at least one of the SELECT column list or
	// the filter predicate of the QUALIFY clause
	if (statement.qualify && result->windows.empty()) {
		throw BinderException("at least one window function must appear in the SELECT column or QUALIFY clause");
	}

	// now that the SELECT list is bound, we set the types of DISTINCT/ORDER BY expressions
	BindModifiers(*result, result->projection_index, result->names, internal_sql_types, bind_state);
	return std::move(result);
}

} // namespace duckdb
