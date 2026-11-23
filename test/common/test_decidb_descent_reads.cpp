#include "catch.hpp"

#include "duckdb/execution/operator/decide/decide_descent.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"
#include "duckdb/planner/decide/decide_tree.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

using namespace duckdb;

namespace {

//! A tree of one E node whose `when` is `when`, over rows of `width` columns.
duckdb::unique_ptr<DecideTree> TreeReading(duckdb::unique_ptr<Expression> when) {
	auto tree = make_uniq<DecideTree>();
	auto node = make_uniq<DecideScopeNode>();
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

// A helper's class at a row (pipeline/08_execution/done.md, "A helper's class at a row"): listed by row while few of
// the input's rows read it, a class per row from there on. Either form answers alike.
TEST_CASE("A helper's rows are listed while few read it", "[decidb]") {
	constexpr idx_t ROWS = 40;

	SECTION("few rows read it: they are listed, in any order and once for each reader") {
		DecideScopeRows rows(ROWS, 2, 4);
		rows.List(31, 1);
		rows.List(7, 0);
		rows.List(31, 1);
		rows.List(7, 0);
		// A row of a group with no member reads none.
		rows.List(12, DConstants::INVALID_INDEX);
		const auto mapping = rows.Close(ROWS);
		CHECK(rows.class_of_row.empty());
		CHECK(rows.listed.size() == 2);
		CHECK(mapping.row_to_class.empty());
		CHECK(mapping.num_classes == 3);
		CHECK(mapping.absent_class == 2);
		CHECK(rows.At(7) == 0);
		CHECK(rows.At(31) == 1);
		CHECK(rows.At(12) == 2);
		CHECK(rows.At(39) == 2);
		CHECK(rows.Absent(0));
		CHECK_FALSE(rows.Absent(31));
	}

	SECTION("one row in eight reads it: a class per row") {
		DecideScopeRows rows(ROWS, 2, ROWS / DecideScopeRows::LISTED_SHARE);
		rows.List(7, 0);
		rows.List(31, 1);
		const auto mapping = rows.Close(ROWS);
		CHECK(rows.class_of_row.size() == ROWS);
		CHECK(rows.listed.empty());
		CHECK(mapping.num_classes == 3);
		CHECK(rows.At(7) == 0);
		CHECK(rows.At(31) == 1);
		CHECK(rows.Absent(12));
	}

	SECTION("every row reads a class: no spare class") {
		DecideScopeRows rows(2, 1, 2);
		rows.List(0, 0);
		rows.List(1, 0);
		const auto mapping = rows.Close(2);
		CHECK(mapping.num_classes == 1);
		CHECK(mapping.absent_class == DConstants::INVALID_INDEX);
		CHECK_FALSE(rows.Absent(1));
	}

	SECTION("a declared decision's classes are read where they are held") {
		KeyedMapping declared;
		declared.num_classes = 2;
		declared.row_to_class = {1, 0, 1};
		const DecideScopeRows rows(declared);
		CHECK(rows.At(0) == 1);
		CHECK(rows.At(1) == 0);
		CHECK_FALSE(rows.Absent(2));
	}
}
