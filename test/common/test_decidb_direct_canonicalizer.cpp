#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/decide/direct/direct_canonicalizer.hpp"
#include "duckdb/optimizer/decide/direct/direct_expression.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

using namespace duckdb;

namespace {

//! The plans below are built by hand over one-row leaves, in the order a rule builds them: each projection reads the
//! columns of the operator under it. `Leaf` is a one-row projection with the constant column `c` (7) and the column `x`
//! of its leaf; `x` is a source column and not a constant.
struct Plans {
	explicit Plans(ClientContext &context)
	    : binder(Binder::CreateBinder(context)), optimizer(*binder, context), calc(context) {
	}

	duckdb::unique_ptr<LogicalOperator> Leaf(idx_t &constant_binding_table) {
		auto dummy = make_uniq<LogicalDummyScan>(binder->GenerateTableIndex());
		auto dummy_binding = dummy->GetColumnBindings()[0];
		constant_binding_table = binder->GenerateTableIndex();
		duckdb::vector<duckdb::unique_ptr<Expression>> expressions;
		expressions.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(7)));
		expressions.push_back(calc.Out(calc.Col(LogicalType::INTEGER, dummy_binding)));
		auto projection = make_uniq<LogicalProjection>(constant_binding_table, std::move(expressions));
		projection->children.push_back(std::move(dummy));
		return std::move(projection);
	}

	//! `column + 1` over `child`, a projection with one column.
	duckdb::unique_ptr<LogicalOperator> PlusOne(duckdb::unique_ptr<LogicalOperator> child, ColumnBinding column) {
		duckdb::vector<duckdb::unique_ptr<Expression>> expressions;
		expressions.push_back(
		    calc.Out(calc.Add(calc.Col(LogicalType::INTEGER, column), calc.Constant(Value::INTEGER(1)))));
		auto projection = make_uniq<LogicalProjection>(binder->GenerateTableIndex(), std::move(expressions));
		projection->children.push_back(std::move(child));
		return std::move(projection);
	}

	duckdb::unique_ptr<LogicalOperator> Join(JoinType type, duckdb::unique_ptr<LogicalOperator> left,
	                                         duckdb::unique_ptr<LogicalOperator> right, ColumnBinding on_left,
	                                         ColumnBinding on_right) {
		auto join = make_uniq<LogicalComparisonJoin>(type);
		JoinCondition condition;
		condition.comparison = ExpressionType::COMPARE_EQUAL;
		condition.left = calc.Out(calc.Col(LogicalType::INTEGER, on_left));
		condition.right = calc.Out(calc.Col(LogicalType::INTEGER, on_right));
		join->conditions.push_back(std::move(condition));
		join->children.push_back(std::move(left));
		join->children.push_back(std::move(right));
		return std::move(join);
	}

	//! Whether the expression of the one column of `plan` is the constant `value`.
	static bool IsConstant(const LogicalOperator &plan, int32_t value) {
		auto &expression = *plan.Cast<LogicalProjection>().expressions[0];
		return expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
		       expression.Cast<BoundConstantExpression>().value == Value::INTEGER(value);
	}

	duckdb::shared_ptr<Binder> binder;
	Optimizer optimizer;
	ExpressionBuilder calc;
};

//! A DirectCanonicalize that reads nothing from `source`: a projection in no plan.
void Canonicalize(Plans &plans, LogicalOperator &plan, const LogicalOperator &source) {
	DirectCanonicalize(plans.optimizer, plan, source);
}

} // namespace

// A reference to a projection column that holds a constant becomes the constant, and DuckDB's rules then fold what the
// constant makes foldable. It is carried through the operators that pass their child's columns on.
TEST_CASE("Canonicalization carries a constant column up through the operators that pass it on", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.context->RunFunctionInTransaction([&]() {
		Plans plans(*con.context);
		LogicalProjection stand_in(0, {});
		SECTION("a projection over the projection") {
			idx_t index;
			auto leaf = plans.Leaf(index);
			auto top = plans.PlusOne(std::move(leaf), ColumnBinding(index, 0));
			Canonicalize(plans, *top, stand_in);
			REQUIRE(Plans::IsConstant(*top, 8));
		}
		SECTION("a column that is not a constant stays a column") {
			idx_t index;
			auto leaf = plans.Leaf(index);
			auto top = plans.PlusOne(std::move(leaf), ColumnBinding(index, 1));
			Canonicalize(plans, *top, stand_in);
			REQUIRE(!Plans::IsConstant(*top, 8));
			REQUIRE(top->Cast<LogicalProjection>().expressions[0]->GetExpressionClass() ==
			        ExpressionClass::BOUND_FUNCTION);
		}
		SECTION("through a filter") {
			idx_t index;
			auto filter = make_uniq<LogicalFilter>(plans.calc.Out(plans.calc.Bool(true)));
			filter->children.push_back(plans.Leaf(index));
			auto top = plans.PlusOne(std::move(filter), ColumnBinding(index, 0));
			Canonicalize(plans, *top, stand_in);
			REQUIRE(Plans::IsConstant(*top, 8));
		}
		SECTION("through a window") {
			idx_t index;
			auto window = make_uniq<LogicalWindow>(plans.binder->GenerateTableIndex());
			window->children.push_back(plans.Leaf(index));
			auto top = plans.PlusOne(std::move(window), ColumnBinding(index, 0));
			Canonicalize(plans, *top, stand_in);
			REQUIRE(Plans::IsConstant(*top, 8));
		}
		SECTION("through a cross product") {
			for (bool read_left : {true, false}) {
				idx_t left_index, right_index;
				auto product = LogicalCrossProduct::Create(plans.Leaf(left_index), plans.Leaf(right_index));
				auto top = plans.PlusOne(std::move(product), ColumnBinding(read_left ? left_index : right_index, 0));
				Canonicalize(plans, *top, stand_in);
				REQUIRE(Plans::IsConstant(*top, 8));
			}
		}
	});
}

// Canonicalization stops at the source, which the user wrote and DuckDB has simplified already.
TEST_CASE("Canonicalization leaves the source as it is", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.context->RunFunctionInTransaction([&]() {
		Plans plans(*con.context);
		idx_t index;
		auto source = plans.PlusOne(plans.Leaf(index), ColumnBinding(index, 0));
		auto &source_ref = *source;
		auto top = plans.PlusOne(std::move(source), ColumnBinding(source_ref.Cast<LogicalProjection>().table_index, 0));
		Canonicalize(plans, *top, source_ref);
		// The source's own `7 + 1` is not folded, and what reads it is not either.
		REQUIRE(!Plans::IsConstant(source_ref, 8));
		REQUIRE(!Plans::IsConstant(*top, 9));
	});
}

// The columns of the side of an outer join that is NULL-padded are not constants above the join: a row with no partner
// reads NULL there.
TEST_CASE("Canonicalization does not carry a constant across the padded side of an outer join", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.context->RunFunctionInTransaction([&]() {
		Plans plans(*con.context);
		LogicalProjection stand_in(0, {});
		struct Case {
			JoinType type;
			//! Whether the left side's constant column is a constant above the join, and the right side's.
			bool left;
			bool right;
		};
		duckdb::vector<Case> cases {{JoinType::INNER, true, true},
		                            {JoinType::LEFT, true, false},
		                            {JoinType::SINGLE, true, false},
		                            {JoinType::RIGHT, false, false},
		                            {JoinType::OUTER, false, false}};
		for (auto &join_case : cases) {
			for (bool read_left : {true, false}) {
				idx_t left_index, right_index;
				auto left = plans.Leaf(left_index);
				auto right = plans.Leaf(right_index);
				auto join = plans.Join(join_case.type, std::move(left), std::move(right), ColumnBinding(left_index, 1),
				                       ColumnBinding(right_index, 1));
				auto top = plans.PlusOne(std::move(join), ColumnBinding(read_left ? left_index : right_index, 0));
				Canonicalize(plans, *top, stand_in);
				INFO("join type " << static_cast<int>(join_case.type) << (read_left ? " left" : " right"));
				REQUIRE(Plans::IsConstant(*top, 8) == (read_left ? join_case.left : join_case.right));
			}
		}
	});
}

// A hash join is built on columns and a hash aggregate groups on them, so their keys keep the column; the values an
// aggregate reads are expressions, which take the constant.
TEST_CASE("Canonicalization keeps the keys of a join and of an aggregate as columns", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.context->RunFunctionInTransaction([&]() {
		Plans plans(*con.context);
		LogicalProjection stand_in(0, {});
		SECTION("a join condition") {
			idx_t left_index, right_index;
			auto join = plans.Join(JoinType::INNER, plans.Leaf(left_index), plans.Leaf(right_index),
			                       ColumnBinding(left_index, 0), ColumnBinding(right_index, 1));
			auto &condition = join->Cast<LogicalComparisonJoin>().conditions[0];
			Canonicalize(plans, *join, stand_in);
			REQUIRE(condition.left->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF);
		}
		SECTION("an aggregate's group and the column it reads") {
			idx_t index;
			auto leaf = plans.Leaf(index);
			duckdb::vector<duckdb::unique_ptr<Expression>> arguments;
			arguments.push_back(plans.calc.Out(plans.calc.Col(LogicalType::INTEGER, ColumnBinding(index, 0))));
			duckdb::vector<duckdb::unique_ptr<Expression>> aggregates;
			aggregates.push_back(plans.optimizer.BindAggregateFunction("sum", std::move(arguments)));
			auto aggregate = make_uniq<LogicalAggregate>(plans.binder->GenerateTableIndex(),
			                                             plans.binder->GenerateTableIndex(), std::move(aggregates));
			aggregate->groups.push_back(plans.calc.Out(plans.calc.Col(LogicalType::INTEGER, ColumnBinding(index, 0))));
			aggregate->children.push_back(std::move(leaf));
			auto &group = *aggregate->groups[0];
			Canonicalize(plans, *aggregate, stand_in);
			REQUIRE(group.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF);
			auto &argument = *aggregate->expressions[0]->Cast<BoundAggregateExpression>().children[0];
			REQUIRE(argument.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT);
		}
	});
}
