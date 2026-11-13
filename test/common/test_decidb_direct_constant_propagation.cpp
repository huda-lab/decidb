#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/optimizer/decide/direct/direct_constant_propagation.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_positional_join.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

using namespace duckdb;

namespace {

using PlanPtr = duckdb::unique_ptr<LogicalOperator>;
using ExprPtr = duckdb::unique_ptr<Expression>;

ExprPtr PropagationCol(const ColumnBinding &binding) {
	return make_uniq<BoundColumnRefExpression>(LogicalType::DOUBLE, binding);
}

ExprPtr PropagationConstant(double value) {
	return make_uniq<BoundConstantExpression>(Value::DOUBLE(value));
}

//! A projection of table `index` over `child` with the given columns.
PlanPtr PropagationProject(idx_t index, duckdb::vector<ExprPtr> expressions, PlanPtr child) {
	auto projection = make_uniq<LogicalProjection>(index, std::move(expressions));
	projection->children.push_back(std::move(child));
	return std::move(projection);
}

//! The columns (index, 0) = 5.0 and (index, 1) = the one column x of a leaf of table `leaf`, over that leaf.
PlanPtr ConstantAndColumn(idx_t index, idx_t leaf, double constant) {
	duckdb::vector<ExprPtr> expressions;
	expressions.push_back(PropagationConstant(constant));
	expressions.push_back(PropagationCol(ColumnBinding(leaf, 0)));
	return PropagationProject(index, std::move(expressions), make_uniq<LogicalDummyScan>(leaf));
}

//! Whether `expr` reads the column `binding`.
bool PropagationReads(const Expression &expr, const ColumnBinding &binding) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    expr.Cast<BoundColumnRefExpression>().binding == binding) {
		return true;
	}
	bool found = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { found = found || PropagationReads(child, binding); });
	return found;
}

//! Runs the pass over `plan`, as the rule driver does.
void Propagate(Connection &con, PlanPtr &plan) {
	if (!con.HasActiveTransaction()) {
		con.BeginTransaction();
	}
	auto binder = Binder::CreateBinder(*con.context);
	Optimizer optimizer(*binder, *con.context);
	DirectConstantPropagation(optimizer.rewriter).VisitOperator(*plan);
}

//! `constant > column`, a computation that reads both, in a projection of table 9 over `child`.
PlanPtr CompareOver(PlanPtr child, const ColumnBinding &constant, const ColumnBinding &column) {
	duckdb::vector<ExprPtr> expressions;
	expressions.push_back(make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHAN,
	                                                           PropagationCol(constant), PropagationCol(column)));
	return PropagationProject(9, std::move(expressions), std::move(child));
}

const Expression &TopExpression(PlanPtr &plan, idx_t i) {
	return *plan->Cast<LogicalProjection>().expressions[i];
}

//! Which constants of a join's sides a projection above it still reads as columns.
struct JoinOutcome {
	bool left_constant_stays;
	bool right_constant_stays;
};

//! A projection of table 9 over a join of table 2 (constant c, x) and table 4 (constant d, y): its column 0 reads the
//! left constant and x, its column 1 the right constant and y.
PlanPtr JoinPlan(const std::function<PlanPtr(PlanPtr, PlanPtr)> &join) {
	duckdb::vector<ExprPtr> expressions;
	expressions.push_back(make_uniq<BoundComparisonExpression>(
	    ExpressionType::COMPARE_GREATERTHAN, PropagationCol(ColumnBinding(2, 0)), PropagationCol(ColumnBinding(2, 1))));
	expressions.push_back(make_uniq<BoundComparisonExpression>(
	    ExpressionType::COMPARE_GREATERTHAN, PropagationCol(ColumnBinding(4, 0)), PropagationCol(ColumnBinding(4, 1))));
	return PropagationProject(9, std::move(expressions),
	                          join(ConstantAndColumn(2, 1, 5.0), ConstantAndColumn(4, 3, 7.0)));
}

PlanPtr ComparisonJoin(JoinType type, PlanPtr left, PlanPtr right) {
	auto join = make_uniq<LogicalComparisonJoin>(type);
	JoinCondition condition;
	condition.left = PropagationCol(ColumnBinding(2, 1));
	condition.right = PropagationCol(ColumnBinding(4, 1));
	condition.comparison = ExpressionType::COMPARE_EQUAL;
	join->conditions.push_back(std::move(condition));
	join->children.push_back(std::move(left));
	join->children.push_back(std::move(right));
	return std::move(join);
}

JoinOutcome PropagateJoin(const std::function<PlanPtr(PlanPtr, PlanPtr)> &join) {
	DuckDB db(nullptr);
	Connection con(db);
	auto plan = JoinPlan(join);
	Propagate(con, plan);
	return {PropagationReads(TopExpression(plan, 0), ColumnBinding(2, 0)),
	        PropagationReads(TopExpression(plan, 1), ColumnBinding(4, 0))};
}

JoinOutcome PropagateComparisonJoin(JoinType type) {
	return PropagateJoin(
	    [&](PlanPtr left, PlanPtr right) { return ComparisonJoin(type, std::move(left), std::move(right)); });
}

} // namespace

TEST_CASE("Constant propagation: a projection records its constant column", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto plan = CompareOver(ConstantAndColumn(2, 1, 5.0), ColumnBinding(2, 0), ColumnBinding(2, 1));
	Propagate(con, plan);
	REQUIRE(!PropagationReads(TopExpression(plan, 0), ColumnBinding(2, 0)));
	REQUIRE(PropagationReads(TopExpression(plan, 0), ColumnBinding(2, 1)));
}

TEST_CASE("Constant propagation: a constant passes through a filter", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto filter = make_uniq<LogicalFilter>(make_uniq<BoundComparisonExpression>(
	    ExpressionType::COMPARE_GREATERTHAN, PropagationCol(ColumnBinding(2, 1)), PropagationCol(ColumnBinding(2, 0))));
	filter->children.push_back(ConstantAndColumn(2, 1, 5.0));
	auto plan = CompareOver(std::move(filter), ColumnBinding(2, 0), ColumnBinding(2, 1));
	Propagate(con, plan);
	REQUIRE(!PropagationReads(TopExpression(plan, 0), ColumnBinding(2, 0)));
	// the filter's own predicate reads the constant too
	auto &filter_op = plan->children[0]->Cast<LogicalFilter>();
	REQUIRE(!PropagationReads(*filter_op.expressions[0], ColumnBinding(2, 0)));
}

TEST_CASE("Constant propagation: a constant passes through a window", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto window = make_uniq<LogicalWindow>(5);
	window->expressions.push_back(
	    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr));
	window->children.push_back(ConstantAndColumn(2, 1, 5.0));
	auto plan = CompareOver(std::move(window), ColumnBinding(2, 0), ColumnBinding(2, 1));
	Propagate(con, plan);
	REQUIRE(!PropagationReads(TopExpression(plan, 0), ColumnBinding(2, 0)));
}

TEST_CASE("Constant propagation: constants pass through an inner join and a cross product", "[decidb]") {
	auto inner = PropagateComparisonJoin(JoinType::INNER);
	REQUIRE(!inner.left_constant_stays);
	REQUIRE(!inner.right_constant_stays);
	auto product = PropagateJoin(
	    [](PlanPtr left, PlanPtr right) { return make_uniq<LogicalCrossProduct>(std::move(left), std::move(right)); });
	REQUIRE(!product.left_constant_stays);
	REQUIRE(!product.right_constant_stays);
}

TEST_CASE("Constant propagation: a LEFT join forgets its right side", "[decidb]") {
	auto outcome = PropagateComparisonJoin(JoinType::LEFT);
	REQUIRE(!outcome.left_constant_stays);
	REQUIRE(outcome.right_constant_stays);
}

TEST_CASE("Constant propagation: a SINGLE join forgets its right side", "[decidb]") {
	auto outcome = PropagateComparisonJoin(JoinType::SINGLE);
	REQUIRE(!outcome.left_constant_stays);
	REQUIRE(outcome.right_constant_stays);
}

TEST_CASE("Constant propagation: a RIGHT join forgets its left side", "[decidb]") {
	auto outcome = PropagateComparisonJoin(JoinType::RIGHT);
	REQUIRE(outcome.left_constant_stays);
	REQUIRE(!outcome.right_constant_stays);
}

TEST_CASE("Constant propagation: a positional join forgets both sides", "[decidb]") {
	auto outcome = PropagateJoin([](PlanPtr left, PlanPtr right) {
		return make_uniq<LogicalPositionalJoin>(std::move(left), std::move(right));
	});
	REQUIRE(outcome.left_constant_stays);
	REQUIRE(outcome.right_constant_stays);
}

TEST_CASE("Constant propagation: a whole-expression reference stays a column", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	duckdb::vector<ExprPtr> expressions;
	expressions.push_back(PropagationCol(ColumnBinding(2, 0)));
	expressions.push_back(PropagationCol(ColumnBinding(2, 1)));
	auto plan = CompareOver(PropagationProject(3, std::move(expressions), ConstantAndColumn(2, 1, 5.0)),
	                        ColumnBinding(3, 0), ColumnBinding(3, 1));
	Propagate(con, plan);
	auto &middle = plan->children[0]->Cast<LogicalProjection>();
	REQUIRE(middle.expressions[0]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF);
	REQUIRE(PropagationReads(*middle.expressions[0], ColumnBinding(2, 0)));
}

TEST_CASE("Constant propagation: a reference to a constant column is a constant column", "[decidb]") {
	DuckDB db(nullptr);
	Connection con(db);
	duckdb::vector<ExprPtr> expressions;
	expressions.push_back(PropagationCol(ColumnBinding(2, 0)));
	expressions.push_back(PropagationCol(ColumnBinding(2, 1)));
	auto plan = CompareOver(PropagationProject(3, std::move(expressions), ConstantAndColumn(2, 1, 5.0)),
	                        ColumnBinding(3, 0), ColumnBinding(3, 1));
	Propagate(con, plan);
	// the projection above reads the copy, which carries the constant on
	REQUIRE(!PropagationReads(TopExpression(plan, 0), ColumnBinding(3, 0)));
	REQUIRE(PropagationReads(TopExpression(plan, 0), ColumnBinding(3, 1)));
}
