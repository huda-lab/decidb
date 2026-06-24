#include "catch.hpp"

#include "duckdb/decidb/decide_diagnostic.hpp"

#include <set>

using namespace duckdb;

// Pure core of the unbounded affected-rows/entities characterization. Given a
// variable's escaping-instance set and categorical column groupings, CharacterizeEscape
// returns every (column, value) group whose within-group escape rate (escaping/total)
// clears the threshold — the "sufficient-direction" rule: when column = value, the
// variable escapes (a of b of the time). No DuckDB execution types, so it unit-tests
// directly like BuildColumnProvenance.

namespace {

// channel: rows 0,1,2 = export ; rows 3,4,5 = domestic
ColumnGrouping ChannelGrouping() {
	ColumnGrouping cg;
	cg.column = "channel";
	cg.instance_to_group = {0, 0, 0, 1, 1, 1};
	cg.group_value = {"export", "domestic"};
	return cg;
}

// region: rows 0,3 = APAC ; rows 1,4 = US ; rows 2,5 = EU
ColumnGrouping RegionGrouping() {
	ColumnGrouping cg;
	cg.column = "region";
	cg.instance_to_group = {0, 1, 2, 0, 1, 2};
	cg.group_value = {"APAC", "US", "EU"};
	return cg;
}

} // namespace

TEST_CASE("DeciDB unbounded escape characterization", "[decidb][query_diagnostics][unbounded]") {
	SECTION("one categorical value fully escapes -> single sufficient rule") {
		std::set<idx_t> escaping = {0, 1, 2}; // all export rows
		auto rules = CharacterizeEscape(escaping, 6, {ChannelGrouping()}, 0.8);
		REQUIRE(rules.size() == 1);
		REQUIRE(rules[0].column == "channel");
		REQUIRE(rules[0].value == "export");
		REQUIRE(rules[0].escaping == 3);
		REQUIRE(rules[0].total == 3);
	}

	SECTION("threshold gates a partially-escaping group") {
		std::set<idx_t> escaping = {0, 1, 3}; // export 2/3, domestic 1/3
		// 0.8 threshold: neither group clears it.
		REQUIRE(CharacterizeEscape(escaping, 6, {ChannelGrouping()}, 0.8).empty());
		// 0.6 threshold: export (0.667) clears, domestic (0.333) does not.
		auto rules = CharacterizeEscape(escaping, 6, {ChannelGrouping()}, 0.6);
		REQUIRE(rules.size() == 1);
		REQUIRE(rules[0].column == "channel");
		REQUIRE(rules[0].value == "export");
		REQUIRE(rules[0].escaping == 2);
		REQUIRE(rules[0].total == 3);
	}

	SECTION("union across multiple columns, strongest rule first") {
		// export rows all escape (3/3); within region, APAC = rows {0,3}: row 0 escapes
		// (export), row 3 does not -> APAC 1/2. US = {1,4}: row 1 escapes -> 1/2.
		// EU = {2,5}: row 2 escapes -> 1/2. Only channel=export clears 0.8.
		std::set<idx_t> escaping = {0, 1, 2};
		auto rules = CharacterizeEscape(escaping, 6, {ChannelGrouping(), RegionGrouping()}, 0.8);
		REQUIRE(rules.size() == 1);
		REQUIRE(rules[0].column == "channel");
		REQUIRE(rules[0].value == "export");

		// Lower the bar: every region half-escapes (0.5), export fully (1.0). export
		// must sort first (highest rate).
		auto all = CharacterizeEscape(escaping, 6, {ChannelGrouping(), RegionGrouping()}, 0.5);
		REQUIRE(all.size() == 4); // export + APAC + US + EU
		REQUIRE(all[0].column == "channel");
		REQUIRE(all[0].value == "export");
	}

	SECTION("scattered escape clears nothing") {
		std::set<idx_t> escaping = {0, 4}; // export 1/3, domestic 1/3
		REQUIRE(CharacterizeEscape(escaping, 6, {ChannelGrouping()}, 0.8).empty());
	}

	SECTION("excluded instances (INVALID group) are ignored") {
		ColumnGrouping cg = ChannelGrouping();
		cg.instance_to_group[2] = DConstants::INVALID_INDEX; // row 2 has a NULL key, say
		std::set<idx_t> escaping = {0, 1, 2};                // row 2 still "escapes" but uncharacterized
		auto rules = CharacterizeEscape(escaping, 6, {cg}, 0.8);
		// export group now has only rows {0,1}, both escaping -> 2/2.
		REQUIRE(rules.size() == 1);
		REQUIRE(rules[0].escaping == 2);
		REQUIRE(rules[0].total == 2);
	}
}
