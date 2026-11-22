#include "catch.hpp"
#include "duckdb/planner/decide/decide_tree_nodes.hpp"

#include "duckdb/common/enums/decide.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/decide/decide_canonicalizer.hpp"
#include "duckdb/planner/decide/decide_term_split.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

using namespace duckdb;

// VerifyCanonical/VerifyCanonicalObjective are DEBUG-only (see decide_canonicalizer.hpp):
// in a release build the body compiles away, so a broken tree legitimately does not
// throw there. Exercise the real invariant check under debug/relassert, and just pin
// "no crash" everywhere else.
#ifdef DEBUG
#define REQUIRE_VERIFY_THROWS(expr) REQUIRE_THROWS_AS(expr, InternalException)
#else
#define REQUIRE_VERIFY_THROWS(expr) REQUIRE_NOTHROW(expr)
#endif

namespace {

static constexpr idx_t DECIDE_INDEX = 42;

duckdb::unique_ptr<Expression> DecideRef(idx_t column_index, const string &name = "x") {
	return make_uniq<BoundColumnRefExpression>(name, LogicalType::DOUBLE, ColumnBinding(DECIDE_INDEX, column_index));
}

duckdb::unique_ptr<Expression> DataRef(idx_t column_index = 0) {
	return make_uniq<BoundColumnRefExpression>("v", LogicalType::DOUBLE, ColumnBinding(7, column_index));
}

duckdb::unique_ptr<Expression> DoubleConstant(double value) {
	return make_uniq<BoundConstantExpression>(Value::DOUBLE(value));
}

duckdb::unique_ptr<Expression> Comparison(ExpressionType type, duckdb::unique_ptr<Expression> left,
                                          duckdb::unique_ptr<Expression> right) {
	return make_uniq<BoundComparisonExpression>(type, std::move(left), std::move(right));
}

//! The clause whose body is `body`, with no scope.
BoundDecideClause Clause(duckdb::unique_ptr<Expression> body) {
	BoundDecideClause clause;
	clause.body = std::move(body);
	return clause;
}

} // namespace

TEST_CASE("DeciDB canonical invariant verifier", "[decidb][canonicalize][verifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	// The canonicalizer reads the keyed scopes and keys where they are, so they outlive it.
	duckdb::vector<KeyedScopeInfo> keyed_scopes;
	duckdb::vector<DecideKey> keys;
	DecideCanonicalizer canonicalizer(*con.context, DECIDE_INDEX,
	                                  {DecideVarScopeInfo::Row(), DecideVarScopeInfo::Row()}, keyed_scopes, keys,
	                                  duckdb::vector<bool> {false, false});
	auto canonical_of = [&](BoundDecideClause clause) {
		duckdb::vector<BoundDecideClause> clauses;
		clauses.push_back(std::move(clause));
		canonicalizer.CanonicalizeClauses(clauses);
		return std::move(clauses[0]);
	};
	auto bound_on_x = [&]() {
		return Clause(Comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO, DecideRef(0), DoubleConstant(5)));
	};

	SECTION("canonical reversed comparison is a fixed point") {
		auto canonical = canonical_of(
		    Clause(Comparison(ExpressionType::COMPARE_GREATERTHANOREQUALTO, DoubleConstant(5), DecideRef(0))));
		REQUIRE_NOTHROW(canonicalizer.VerifyCanonical(canonical));
	}

	SECTION("decision reference on the RHS violates C2") {
		auto invalid = Clause(Comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO, DecideRef(0), DecideRef(1, "y")));
		REQUIRE_VERIFY_THROWS(canonicalizer.VerifyCanonical(invalid));
	}

	SECTION("decision-free comparison violates C1") {
		auto invalid =
		    Clause(Comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO, DoubleConstant(1), DoubleConstant(2)));
		REQUIRE_VERIFY_THROWS(canonicalizer.VerifyCanonical(invalid));
	}

	SECTION("a clause under a WHEN is a fixed point") {
		auto clause = bound_on_x();
		clause.scope.when = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
		clause.origin = DecideRowOrigin::OfClause(0);
		auto canonical = canonical_of(std::move(clause));
		REQUIRE_NOTHROW(canonicalizer.VerifyCanonical(canonical));
	}

	SECTION("a clause under a WHEN and a per is a fixed point") {
		auto clause = bound_on_x();
		clause.scope = DecideClauseScope::Per(DConstants::INVALID_INDEX);
		clause.scope.when = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
		auto canonical = canonical_of(std::move(clause));
		REQUIRE_NOTHROW(canonicalizer.VerifyCanonical(canonical));
	}

	SECTION("missing query-wide bound provenance breaks the fixed point") {
		auto canonical = canonical_of(bound_on_x());
		auto &comparison = canonical.body->Cast<BoundComparisonExpression>();
		SetDecideQueryWideBound(*comparison.right, false);
		REQUIRE_VERIFY_THROWS(canonicalizer.VerifyCanonical(canonical));
	}

	SECTION("stale query-wide provenance on row data breaks the fixed point") {
		auto invalid = Clause(Comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO, DecideRef(0), DataRef()));
		auto &comparison = invalid.body->Cast<BoundComparisonExpression>();
		SetDecideQueryWideBound(*comparison.right, true);
		REQUIRE_VERIFY_THROWS(canonicalizer.VerifyCanonical(invalid));
	}

	SECTION("optimizer Boolean placeholder is an allowed non-comparison leaf") {
		auto placeholder = Clause(make_uniq<BoundConstantExpression>(Value::BOOLEAN(true)));
		REQUIRE_NOTHROW(canonicalizer.VerifyCanonical(placeholder));
	}

	auto sum_of = [&](duckdb::unique_ptr<Expression> left, duckdb::unique_ptr<Expression> right) {
		duckdb::vector<duckdb::unique_ptr<Expression>> children;
		children.push_back(std::move(left));
		children.push_back(std::move(right));
		return DecideRebindOperator(*con.context, "+", std::move(children));
	};

	SECTION("a data term beside the decisions stays in a canonical objective (D72)") {
		// Binding `+` reads the catalog.
		con.BeginTransaction();
		double offset = 0;
		auto canonical = canonicalizer.CanonicalizeObjective(Clause(sum_of(DecideRef(0), DataRef())), offset);
		REQUIRE_NOTHROW(canonicalizer.VerifyCanonicalObjective(canonical));
	}

	SECTION("a constant left beside the decisions violates O2") {
		con.BeginTransaction();
		REQUIRE_VERIFY_THROWS(canonicalizer.VerifyCanonicalObjective(Clause(sum_of(DecideRef(0), DoubleConstant(5)))));
	}
}
