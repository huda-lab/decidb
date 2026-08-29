#include "catch.hpp"

#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"

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

// tag: partitions the rows exactly as ChannelGrouping does, so its rules describe the
// same escaping instances by a different name.
ColumnGrouping TagGrouping() {
	ColumnGrouping cg;
	cg.column = "tag";
	cg.instance_to_group = {0, 0, 0, 1, 1, 1};
	cg.group_value = {"a", "b"};
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

	SECTION("columns describing the same escaping rows collapse to one rule") {
		// Both columns split the rows 3/3 the same way, so `channel = 'export'` and
		// `tag = 'a'` state one fact twice. Neither is clause-referenced, so the
		// leftmost candidate is the representative.
		std::set<idx_t> escaping = {0, 1, 2};
		auto rules = CharacterizeEscape(escaping, 6, {ChannelGrouping(), TagGrouping()}, 0.8);
		REQUIRE(rules.size() == 1);
		CHECK(rules[0].column == "channel");

		// Order decides it only among equals: reverse the candidates and `tag` wins.
		auto reversed = CharacterizeEscape(escaping, 6, {TagGrouping(), ChannelGrouping()}, 0.8);
		REQUIRE(reversed.size() == 1);
		CHECK(reversed[0].column == "tag");
	}

	SECTION("a DECIDE-clause column outranks a coincidental one") {
		// Same identical split, but now the query itself names `tag` — the column that
		// explains the escape beats the one that merely correlates with it, even though
		// `channel` is further left.
		std::set<idx_t> escaping = {0, 1, 2};
		ColumnGrouping tag = TagGrouping();
		tag.clause_referenced = true;
		auto rules = CharacterizeEscape(escaping, 6, {ChannelGrouping(), tag}, 0.8);
		REQUIRE(rules.size() == 1);
		CHECK(rules[0].column == "tag");
		CHECK(rules[0].value == "a");
	}

	SECTION("distinct slices are never collapsed") {
		// Rules covering different escaping rows are different facts; dedupe must leave
		// all of them queryable.
		std::set<idx_t> escaping = {0, 1, 2};
		auto rules = CharacterizeEscape(escaping, 6, {ChannelGrouping(), RegionGrouping()}, 0.5);
		REQUIRE(rules.size() == 4); // export {0,1,2} + APAC {0} + US {1} + EU {2}
	}

	SECTION("full coverage marks the rules that scope the cap") {
		// The single rule accounts for every escaping row and its whole group escapes,
		// so the caller may scope the prescribed cap to it.
		std::set<idx_t> escaping = {0, 1, 2};
		auto rules = CharacterizeEscape(escaping, 6, {ChannelGrouping()}, 0.8);
		REQUIRE(rules.size() == 1);
		CHECK(rules[0].covers_scope);
	}

	SECTION("an escaper outside every rule blocks scoping") {
		// Row 3 escapes but its group (domestic, 1/3) never clears the threshold, so the
		// rules explain 3 of 4 escapers. A cap scoped to them would leave row 3 free.
		std::set<idx_t> escaping = {0, 1, 2, 3};
		auto rules = CharacterizeEscape(escaping, 6, {ChannelGrouping()}, 0.8);
		REQUIRE(rules.size() == 1);
		CHECK(rules[0].column == "channel");
		CHECK_FALSE(rules[0].covers_scope);
	}

	SECTION("a partially-escaping rule cannot scope the cap") {
		// export escapes 2 of 3 at a 0.6 threshold, and those 2 are every escaper. The
		// rule still cannot scope the cap: row 2 shares the group but never ran away.
		std::set<idx_t> escaping = {0, 1};
		auto rules = CharacterizeEscape(escaping, 6, {ChannelGrouping()}, 0.6);
		REQUIRE(rules.size() == 1);
		CHECK(rules[0].escaping == 2);
		CHECK(rules[0].total == 3);
		CHECK_FALSE(rules[0].covers_scope);
	}

	SECTION("the narrowest set of rules scopes the cap") {
		// Every region half-escapes and export fully escapes; all four rules are
		// reported, but only `channel = 'export'` is needed to cover the escapers, and
		// the region rules are not whole-group anyway.
		std::set<idx_t> escaping = {0, 1, 2};
		auto rules = CharacterizeEscape(escaping, 6, {ChannelGrouping(), RegionGrouping()}, 0.5);
		REQUIRE(rules.size() == 4);
		for (const auto &r : rules) {
			CHECK(r.covers_scope == (r.column == "channel"));
		}
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
