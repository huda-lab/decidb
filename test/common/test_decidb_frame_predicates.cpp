#include "catch.hpp"

#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

using namespace duckdb;

namespace {

//! One end of a frame: its kind and its k.
struct End {
	DecideFrameAnchor anchor;
	int64_t k;
};

int64_t Lift(const End &end, int64_t at, int64_t peers) {
	switch (end.anchor) {
	case DecideFrameAnchor::FIRST:
		return 0;
	case DecideFrameAnchor::LAST:
		return peers - 1;
	case DecideFrameAnchor::PREVIOUS:
		return at - end.k;
	case DecideFrameAnchor::NEXT:
		return at + end.k;
	}
	return 0;
}

//! What some anchor of some timeline of up to `max_peers` positions finds, enumerated
//! position by position from syntax_reference.md §5.6 rather than reasoned about.
struct Found {
	bool empty = false;         //!< some range requests no position
	bool misses = false;        //!< some requested position lies off a timeline
	bool every_missing = false; //!< some range requests positions, all of them off it
	bool absent = false;        //!< some anchor reads nothing
};

Found Enumerate(const DecideFrameSpec &frame, int64_t max_peers) {
	Found found;
	for (int64_t peers = 1; peers <= max_peers; peers++) {
		for (int64_t at = 0; at < peers; at++) {
			duckdb::vector<int64_t> requested;
			if (frame.range) {
				const auto lo = Lift({frame.from, frame.from_k}, at, peers);
				const auto hi = Lift({frame.to, frame.to_k}, at, peers);
				for (auto q = lo; q <= hi; q += frame.every) {
					requested.push_back(q);
				}
			} else {
				requested.push_back(Lift({frame.from, frame.from_k}, at, peers));
			}
			idx_t missing = 0;
			for (auto q : requested) {
				missing += !frame.cyclic && (q < 0 || q >= peers);
			}
			const idx_t present = requested.size() - missing;
			found.empty = found.empty || requested.empty();
			found.misses = found.misses || missing > 0;
			found.every_missing = found.every_missing || (!requested.empty() && present == 0);
			bool absent;
			if (!frame.range) {
				absent = missing > 0 && !frame.fallback;
			} else if (frame.all && missing > 0) {
				absent = true;
			} else if (frame.reducer == "count") {
				absent = false; // no value is 0
			} else {
				absent = present == 0 && (missing == 0 || !frame.fallback);
			}
			found.absent = found.absent || absent;
		}
	}
	return found;
}

} // namespace

TEST_CASE("A frame's static absence predicates match every position they summarize", "[decidb]") {
	// Every end with k up to 4, every step up to 4, open and cyclic, each policy, a point and
	// a range of SUM and COUNT, on timelines of up to 10 positions: enough for every
	// predicate's witness when one exists.
	duckdb::vector<End> ends {{DecideFrameAnchor::FIRST, 1}, {DecideFrameAnchor::LAST, 1}};
	for (int64_t k = 1; k <= 4; k++) {
		ends.push_back({DecideFrameAnchor::PREVIOUS, k});
		ends.push_back({DecideFrameAnchor::NEXT, k});
	}
	idx_t checked = 0;
	for (auto &from : ends) {
		for (auto &to : ends) {
			for (int64_t every = 1; every <= 4; every++) {
				for (int policy = 0; policy < 3; policy++) {
					for (bool cyclic : {false, true}) {
						for (int shape = 0; shape < 3; shape++) {
							DecideFrameSpec frame;
							frame.range = shape > 0;
							if (!frame.range && (to.anchor != from.anchor || to.k != from.k || every > 1 ||
							                     policy == 2)) {
								continue; // a point has one position, no step and no `all`
							}
							frame.reducer = shape == 2 ? "count" : (frame.range ? "sum" : "");
							frame.from = from.anchor;
							frame.from_k = from.k;
							frame.to = to.anchor;
							frame.to_k = to.k;
							frame.every = every;
							frame.cyclic = cyclic;
							frame.all = policy == 2;
							if (policy == 1) {
								frame.fallback = make_uniq<BoundConstantExpression>(Value::INTEGER(0));
							}
							auto found = Enumerate(frame, 10);
							INFO("from " << int(from.anchor) << "/" << from.k << " to " << int(to.anchor) << "/"
							             << to.k << " every " << every << " policy " << policy << " cyclic "
							             << cyclic << " shape " << shape);
							// Exact: an over-approximation refuses a frame that could not miss,
							// an under-approximation reads one as present where it is absent.
							REQUIRE(frame.MaySelectNothing() == found.absent);
							REQUIRE(frame.MayMissAPosition() == found.misses);
							if (frame.range) {
								REQUIRE(frame.MayBeEmpty() == found.empty);
								// Used only beside MayBeEmpty, so only its soundness matters.
								if (!cyclic && found.every_missing) {
									REQUIRE(frame.MayMissEveryPosition());
								}
							}
							checked++;
						}
					}
				}
			}
		}
	}
	REQUIRE(checked > 4000);
}
