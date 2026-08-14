#include "catch.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

using namespace duckdb;

namespace {

static constexpr idx_t DECIDE_INDEX = 42;

duckdb::unique_ptr<Expression> DecideRef(idx_t column_index, const string &name = "x") {
	return make_uniq<BoundColumnRefExpression>(
	    name, LogicalType::DOUBLE, ColumnBinding(DECIDE_INDEX, column_index));
}

duckdb::unique_ptr<Expression> DataRef(idx_t column_index = 0) {
	return make_uniq<BoundColumnRefExpression>(
	    "v", LogicalType::DOUBLE, ColumnBinding(7, column_index));
}

duckdb::unique_ptr<Expression> DoubleConstant(double value) {
	return make_uniq<BoundConstantExpression>(Value::DOUBLE(value));
}

duckdb::unique_ptr<Expression> Comparison(ExpressionType type, duckdb::unique_ptr<Expression> left,
	                                      duckdb::unique_ptr<Expression> right) {
	return make_uniq<BoundComparisonExpression>(type, std::move(left), std::move(right));
}

} // namespace

TEST_CASE("DeciDB canonical invariant verifier", "[decidb][canonicalize][verifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	DecideCanonicalizer canonicalizer(
	    *con.context, DECIDE_INDEX,
	    {DecideVarScopeInfo::Row(), DecideVarScopeInfo::Row()});

	SECTION("canonical reversed comparison is a fixed point") {
		auto written = Comparison(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		                          DoubleConstant(5), DecideRef(0));
		auto canonical = canonicalizer.CanonicalizeTree(*written);
		REQUIRE_NOTHROW(canonicalizer.VerifyCanonical(*canonical));
	}

	SECTION("decision reference on the RHS violates C2") {
		auto invalid = Comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                          DecideRef(0), DecideRef(1, "y"));
		REQUIRE_THROWS_AS(canonicalizer.VerifyCanonical(*invalid), InternalException);
	}

	SECTION("decision-free comparison violates C1") {
		auto invalid = Comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                          DoubleConstant(1), DoubleConstant(2));
		REQUIRE_THROWS_AS(canonicalizer.VerifyCanonical(*invalid), InternalException);
	}

	SECTION("WHEN wrapper arity is structural") {
		auto wrapped = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		wrapped->SetAlias(WHEN_CONSTRAINT_TAG);
		wrapped->children.push_back(Comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                                       DecideRef(0), DoubleConstant(5)));
		REQUIRE_THROWS_AS(canonicalizer.VerifyCanonical(*wrapped), InternalException);
	}

	SECTION("WHEN wrapper remains structural with composable metadata") {
		auto wrapped = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		string alias = WHEN_CONSTRAINT_TAG;
		AddDecideTag(alias, MakeSourceClauseTag(0));
		wrapped->SetAlias(std::move(alias));
		wrapped->children.push_back(Comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                                       DecideRef(0), DoubleConstant(5)));
		wrapped->children.push_back(make_uniq<BoundConstantExpression>(Value::BOOLEAN(true)));
		auto canonical = canonicalizer.CanonicalizeTree(*wrapped);
		REQUIRE_NOTHROW(canonicalizer.VerifyCanonical(*canonical));
	}

	SECTION("PER requires an aggregate constraint child") {
		auto wrapped = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		wrapped->SetAlias(PER_CONSTRAINT_TAG);
		wrapped->children.push_back(Comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                                       DecideRef(0), DoubleConstant(5)));
		wrapped->children.push_back(DataRef());
		REQUIRE_THROWS_AS(canonicalizer.VerifyCanonical(*wrapped), InternalException);
	}

	SECTION("missing query-wide bound provenance breaks the fixed point") {
		auto written = Comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                          DecideRef(0), DoubleConstant(5));
		auto canonical = canonicalizer.CanonicalizeTree(*written);
		auto &comparison = canonical->Cast<BoundComparisonExpression>();
		auto alias = comparison.right->GetAlias();
		RemoveDecideTag(alias, QUERY_WIDE_BOUND_TAG);
		comparison.right->SetAlias(std::move(alias));
		REQUIRE_THROWS_AS(canonicalizer.VerifyCanonical(*canonical), InternalException);
	}

	SECTION("stale query-wide provenance on row data breaks the fixed point") {
		auto invalid = Comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                          DecideRef(0), DataRef());
		auto &comparison = invalid->Cast<BoundComparisonExpression>();
		auto alias = comparison.right->GetAlias();
		AddDecideTag(alias, QUERY_WIDE_BOUND_TAG);
		comparison.right->SetAlias(std::move(alias));
		REQUIRE_THROWS_AS(canonicalizer.VerifyCanonical(*invalid), InternalException);
	}

	SECTION("optimizer Boolean placeholder is an allowed non-comparison leaf") {
		auto placeholder = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
		REQUIRE_NOTHROW(canonicalizer.VerifyCanonical(*placeholder));
	}
}
