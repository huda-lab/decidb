#include "catch.hpp"

#include "duckdb/common/decide_numeric_policy.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

using namespace duckdb;

TEST_CASE("DECIDE numeric policy constants", "[decidb][numeric_policy]") {
	// The sentinel is far beyond the point where a bound reads as infinite, which keeps a bound that arithmetic
	// pushed near it recognisable.
	REQUIRE(DECIDE_SOLVER_INFINITY == 1e30);
	REQUIRE(DECIDE_EFFECTIVE_INFINITY == 1e20);
	REQUIRE(DECIDE_EFFECTIVE_INFINITY < DECIDE_SOLVER_INFINITY);
	REQUIRE(DECIDE_MAX_EXACT_INTEGER == static_cast<double>(int64_t(1) << 53));
	REQUIRE(DECIDE_MAX_FRACTIONAL_MAGNITUDE == static_cast<double>(int64_t(1) << 52));
	// 2^63 is the first double above the largest BIGINT, which is 2^63 - 1.
	REQUIRE(DECIDE_FIRST_DOUBLE_BEYOND_BIGINT == std::ldexp(1.0, 63));
	REQUIRE(static_cast<double>(std::numeric_limits<int64_t>::max()) == DECIDE_FIRST_DOUBLE_BEYOND_BIGINT);
}

TEST_CASE("DECIDE open-bound tests read the solver sentinel and its margin", "[decidb][numeric_policy]") {
	const double infinity = std::numeric_limits<double>::infinity();
	REQUIRE(DecideIsOpenAbove(DECIDE_SOLVER_INFINITY));
	REQUIRE(DecideIsOpenAbove(DECIDE_EFFECTIVE_INFINITY));
	REQUIRE(DecideIsOpenAbove(infinity));
	REQUIRE_FALSE(DecideIsOpenAbove(1e19));
	REQUIRE_FALSE(DecideIsOpenAbove(-infinity));
	REQUIRE_FALSE(DecideIsOpenAbove(std::numeric_limits<double>::quiet_NaN()));

	REQUIRE(DecideIsOpenBelow(-DECIDE_SOLVER_INFINITY));
	REQUIRE(DecideIsOpenBelow(-DECIDE_EFFECTIVE_INFINITY));
	REQUIRE(DecideIsOpenBelow(-infinity));
	REQUIRE_FALSE(DecideIsOpenBelow(-1e19));
	REQUIRE_FALSE(DecideIsOpenBelow(infinity));
	REQUIRE_FALSE(DecideIsOpenBelow(std::numeric_limits<double>::quiet_NaN()));
}

TEST_CASE("DECIDE exact range ends at 2^53 and excludes NaN and infinity", "[decidb][numeric_policy]") {
	REQUIRE(DecideIsWithinExactRange(0.0));
	REQUIRE(DecideIsWithinExactRange(-0.0));
	REQUIRE(DecideIsWithinExactRange(DECIDE_MAX_EXACT_INTEGER));
	REQUIRE(DecideIsWithinExactRange(-DECIDE_MAX_EXACT_INTEGER));
	REQUIRE_FALSE(DecideIsWithinExactRange(DECIDE_MAX_EXACT_INTEGER * 2.0));
	REQUIRE_FALSE(DecideIsWithinExactRange(-DECIDE_MAX_EXACT_INTEGER * 2.0));
	REQUIRE_FALSE(DecideIsWithinExactRange(std::numeric_limits<double>::infinity()));
	REQUIRE_FALSE(DecideIsWithinExactRange(-std::numeric_limits<double>::infinity()));
	REQUIRE_FALSE(DecideIsWithinExactRange(std::numeric_limits<double>::quiet_NaN()));
}

TEST_CASE("DECIDE exact finite double needs a finite double that converts back", "[decidb][numeric_policy]") {
	auto to_int64 = [](double converted) {
		return static_cast<int64_t>(std::llround(converted));
	};
	// 2^53 + 1 does not survive the trip through a double: it lands on 2^53.
	const int64_t beyond = (int64_t(1) << 53) + 1;
	REQUIRE(DecideIsExactFiniteDouble(7.0, int64_t(7), to_int64));
	REQUIRE(DecideIsExactFiniteDouble(DECIDE_MAX_EXACT_INTEGER, int64_t(1) << 53, to_int64));
	REQUIRE_FALSE(DecideIsExactFiniteDouble(static_cast<double>(beyond), beyond, to_int64));
	// A fraction that the original type would round is not exact.
	REQUIRE_FALSE(DecideIsExactFiniteDouble(7.5, int64_t(7), to_int64));
	// A non-finite double is refused before the conversion back is tried.
	bool converted_back = false;
	auto never = [&](double) {
		converted_back = true;
		return int64_t(0);
	};
	REQUIRE_FALSE(DecideIsExactFiniteDouble(std::numeric_limits<double>::infinity(), int64_t(0), never));
	REQUIRE_FALSE(DecideIsExactFiniteDouble(std::numeric_limits<double>::quiet_NaN(), int64_t(0), never));
	REQUIRE_FALSE(converted_back);
}
