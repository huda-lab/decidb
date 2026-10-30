#include "catch.hpp"

#include "duckdb/execution/operator/decide/physical_decide.hpp"

using namespace duckdb;

// The time-limit and Ctrl-C report prints how close the best-so-far is beside "not proven best". Only a proven
// optimum has a gap of 0, so no gap the report prints may read as 0, and none may switch to an exponent.
TEST_CASE("DeciDB time-limit report gap", "[decidb][slow_solve]") {
	SECTION("a gap under 1% keeps two significant digits") {
		// Fixed decimals (%.2f) printed this gap as 0.00%.
		CHECK(FormatDecideGapPercent(2e-6) == "0.0002");
		CHECK(FormatDecideGapPercent(0.00123) == "0.12");
	}
	SECTION("from 1% up, the whole number and one decimal, never an exponent") {
		// Two significant digits alone print 100% as 1e+02.
		CHECK(FormatDecideGapPercent(0.0123) == "1.2");
		CHECK(FormatDecideGapPercent(0.999) == "99.9");
		CHECK(FormatDecideGapPercent(1.0) == "100");
		CHECK(FormatDecideGapPercent(2.5) == "250");
	}
}
