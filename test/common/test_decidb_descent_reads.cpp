#include "catch.hpp"

#include "duckdb/execution/operator/decide/decide_descent.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

using namespace duckdb;

namespace {

//! A tree of one E node whose `when` is `when`, over rows of `width` columns.
duckdb::unique_ptr<DecideTree> TreeReading(duckdb::unique_ptr<Expression> when) {
	auto tree = make_uniq<DecideTree>();
	auto node = make_uniq<DecideNode>(DecideNodeKind::SCOPE);
	node->when.push_back(std::move(when));
	tree->nodes.push_back(std::move(node));
	tree->roots.push_back(0);
	return tree;
}

} // namespace

TEST_CASE("DECIDE descent reads only the row (plan L2)", "[decidb]") {
	SECTION("a column of the row is read") {
		auto tree = TreeReading(make_uniq<BoundReferenceExpression>(LogicalType::BOOLEAN, 2));
		REQUIRE_NOTHROW(DecideDescent::RequireRowReads(*tree, 3));
	}
	SECTION("a column beyond the row is refused") {
		auto tree = TreeReading(make_uniq<BoundReferenceExpression>(LogicalType::BOOLEAN, 3));
		REQUIRE_THROWS_WITH(DecideDescent::RequireRowReads(*tree, 3),
		                    Catch::Contains("reads column 3 of a row of 3 in the descent"));
	}
	SECTION("a value computed over rows is refused") {
		auto window =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
		auto tree = TreeReading(std::move(window));
		REQUIRE_THROWS_WITH(DecideDescent::RequireRowReads(*tree, 3),
		                    Catch::Contains("computes over rows in the descent"));
	}
}
