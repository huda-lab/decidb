//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_frame_map.hpp
//
// The frame selectors' timelines over the materialized input (stage 08).
//===----------------------------------------------------------------------===//
//
// A point frame `at(F [else v]: e) over (S [desc] [cyclic] [within P])` reads, at each row of
// its scoped expression, the payload `e` at the row its position F selects on that row's
// timeline; a range `agg(from F1 to F2 [every d] [else v | all]: e)` reads it at every row
// of every position from F1 to F2 (language/guide.md §5.6). Execution builds one
// DecideFrameMap per frame before any clause is evaluated, then adds the values the
// frames read as columns beside the one pass's (DecidePreparedModel::frame_columns), so
// every evaluator downstream reads them as data and none learns what a frame is; a range
// over decisions is a helper decision instead, defined per window from the map. Defined in
// physical_decide_frames.cpp.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/planner/decide/decide_prepared_model.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

#include <functional>
#include <map>

namespace duckdb {

//! One frame selector's reading of the input: its timelines over the rows its scoped
//! expression reads, each ordered into peers -- the rows of one order-key value -- and for
//! each of those rows, its anchors, the peer a point selects, or the window a range reads.
struct DecideFrameMap {
	//! A range frame's map; else a point's.
	bool range = false;
	//! Whether the frame has an `else`: at a missing position a point reads it, and a range
	//! reads it once per missing position.
	bool fallback = false;
	//! The rows its timelines read (DecideFrameSpec::rows): its anchors.
	vector<bool> anchors;
	//! Per anchor, its timeline; INVALID_INDEX on every other row.
	vector<idx_t> timeline;
	//! Per anchor, its peer, numbered over every timeline, so two anchors share one
	//! exactly when they share a timeline and an order-key value; INVALID_INDEX elsewhere.
	vector<idx_t> peer;
	//! Per anchor, the peer its position selects; INVALID_INDEX where it is missing, and
	//! on every row that is no anchor.
	vector<idx_t> selected_peer;
	//! Each peer's rows, ascending: peer p holds peer_rows[peer_begin[p], peer_begin[p + 1]).
	vector<idx_t> peer_rows;
	vector<idx_t> peer_begin;
	//! Per row: an anchor where the frame reads nothing -- one in no timeline (D75); a point's
	//! missing position with no `else`; a range that requests no position, meets `all`, or
	//! finds no value -- which skips the instance or drops the member (D84).
	vector<bool> absent;

	//! A range's windows: the ranges its anchors read, one per timeline when neither end
	//! depends on the anchor, else one per position (peer) -- and one per anchor where it
	//! reaches an `else` that reads the anchor's own row. Window w is read by the anchors
	//! `window` maps to it, its `else` at `window_anchor[w]`, and reads list
	//! `window_list[w]`; windows of one position share their list. List l reads the first
	//! row of each position it visits -- a position holds one payload (D73), as a point reads
	//! it -- list_rows[list_begin[l], list_begin[l + 1]), each `list_visits` times (a cyclic
	//! range may visit a position again, D76), in the order its positions are requested, and
	//! finds `list_missing[l]` of its requested positions missing.
	vector<idx_t> window;
	vector<idx_t> window_anchor;
	vector<idx_t> window_list;
	vector<idx_t> list_begin {0};
	vector<idx_t> list_rows;
	vector<idx_t> list_visits;
	vector<idx_t> list_missing;
	//! A range over decisions: the rows whose payload reads a NULL and the anchors whose
	//! `else` does, which its windows skip as SQL's aggregate skips a NULL (D84); empty when
	//! none does (SkipNullReads).
	vector<bool> null_payload;
	vector<bool> null_fallback;
	//! The windows SkipNullReads left with nothing to read: they still visit their positions, each read as no value,
	//! which D73 holds to one payload as any position read.
	vector<bool> emptied;

	//! The row the frame reads at `row` -- its selected peer's first --, or INVALID_INDEX.
	idx_t SelectedRow(idx_t row) const {
		auto p = selected_peer[row];
		return p == DConstants::INVALID_INDEX ? DConstants::INVALID_INDEX : peer_rows[peer_begin[p]];
	}
	//! Whether `row` is an anchor in no timeline (D75): it has no position to select from.
	bool Untimed(idx_t row) const {
		return anchors[row] && timeline[row] == DConstants::INVALID_INDEX;
	}
	//! Whether `row` is an anchor some requested position of which is missing: where its
	//! `else` stands in. An anchor in no timeline misses none: it has no value at all.
	bool Missing(idx_t row) const {
		if (!anchors[row] || Untimed(row)) {
			return false;
		}
		return range ? list_missing[window_list[window[row]]] > 0 : selected_peer[row] == DConstants::INVALID_INDEX;
	}
	//! Whether `row` is an anchor where the frame reads nothing.
	bool Absent(idx_t row) const {
		return absent[row];
	}
	//! The rows `peer` holds.
	idx_t PeerSize(idx_t p) const {
		return peer_begin[p + 1] - peer_begin[p];
	}
	idx_t WindowCount() const {
		return window_anchor.size();
	}
	//! Whether window `w` reads nothing: its anchors are absent.
	bool WindowAbsent(idx_t w) const {
		return absent[window_anchor[w]];
	}
	//! Whether window `w` visits its positions: it reads something, or reads only NULLs (`emptied`).
	bool WindowVisits(idx_t w) const {
		return !WindowAbsent(w) || (!emptied.empty() && emptied[w]);
	}
	//! The `else` values window `w` reads: one per missing position, with an `else` that holds a value.
	idx_t WindowFallbacks(idx_t w) const {
		if (!fallback || (!null_fallback.empty() && null_fallback[window_anchor[w]])) {
			return 0;
		}
		return list_missing[window_list[w]];
	}
	//! The values window `w` reads, each row once per visit and each `else` counted.
	idx_t ValueCount(idx_t w) const {
		idx_t values = WindowFallbacks(w);
		ForEachRead(w, [&](idx_t, idx_t times) { values += times; });
		return values;
	}
	//! Each row window `w` reads, with how often: `visit(row, times)`; a row whose payload
	//! reads a NULL is skipped.
	template <class VISIT>
	void ForEachRead(idx_t w, VISIT &&visit) const {
		const auto list = window_list[w];
		for (idx_t i = list_begin[list]; i < list_begin[list + 1]; i++) {
			if (null_payload.empty() || !null_payload[list_rows[i]]) {
				visit(list_rows[i], list_visits[i]);
			}
		}
	}
	//! The rows some window that reads something reads, where a range's payload is
	//! evaluated; and the anchors such a window reads its `else` at, where its `else` is.
	//! Nothing is evaluated on any other row, so a value no window reads raises nothing.
	vector<bool> ReadRows() const {
		vector<bool> read(anchors.size(), false);
		for (idx_t w = 0; w < WindowCount(); w++) {
			if (!WindowAbsent(w)) {
				ForEachRead(w, [&](idx_t row, idx_t) { read[row] = true; });
			}
		}
		return read;
	}
	vector<bool> FallbackAnchors() const {
		vector<bool> reached(anchors.size(), false);
		for (idx_t w = 0; w < WindowCount(); w++) {
			if (!WindowAbsent(w) && WindowFallbacks(w) > 0) {
				reached[window_anchor[w]] = true;
			}
		}
		return reached;
	}
	//! A range over decisions skips the values it reads as NULL (`null_payload`,
	//! `null_fallback`): a window left with none reads nothing, and its anchors are absent.
	void SkipNullReads(vector<bool> payload, vector<bool> else_values) {
		null_payload = std::move(payload);
		null_fallback = std::move(else_values);
		emptied.resize(WindowCount(), false);
		for (idx_t w = 0; w < WindowCount(); w++) {
			if (!WindowAbsent(w) && ValueCount(w) == 0) {
				emptied[w] = true;
			}
		}
		for (idx_t row = 0; row < anchors.size(); row++) {
			if (anchors[row] && window[row] != DConstants::INVALID_INDEX && emptied[window[row]]) {
				absent[row] = true;
			}
		}
	}
};

//! `frame`'s timelines over the input's rows: its anchors grouped by its timeline key (its path's `by` keys, then
//! `within`; a row whose written `within` column is NULL is in no timeline, D75), each ordered by its order key --
//! descending when written, NULL where the connection's `default_null_order` puts it (D74) -- into peers, and each
//! anchor's selected peer or window. A range listing more values than `decide_frame_limit` is refused as it is listed,
//! and an interrupt is polled per timeline and per window.
DecideFrameMap BuildDecideFrameMap(ClientContext &context, const DecideFrameSpec &frame, const vector<DecideKey> &keys,
                                   PerGroupCache &per_group_cache, idx_t num_rows,
                                   ChunkExprCache &chunk_expr_cache);

//! What each anchor of `map` reads, once its anchors, timelines and peers are known (`first_peer[t]` the first peer of
//! timeline `t`, then one past the last): a point's selected peer, a range's window (language/guide.md §5.6), and the
//! anchors where the frame reads nothing. BuildDecideFrameMap's second half, which the tree's frames share: their
//! timelines are its descent's (DecideDescentTimeline).
void SelectDecideFramePeers(ClientContext &context, const DecideFrameSpec &frame, const vector<idx_t> &first_peer,
                            DecideFrameMap &map);

//! An expression's values on the rows a frame reads it on, one slot per row and one after them for the caller: NULL on
//! every other row, and each row's error kept with its value until a reader reaches the row, as the one pass keeps a
//! row's error (DecideRowStore).
struct DecideFrameValues {
	DecideFrameValues(const LogicalType &type, idx_t count) : values(type, count) {
	}
	Vector values;
	std::map<idx_t, ErrorData> errors;
};

//! Where a frame's columns read an expression on the rows `rows` marks: flat's one pass, or the tree's programs on the
//! frame's anchors (DecideTreeColumns).
using DecideFrameRead = std::function<DecideFrameValues(const Expression &expression, const vector<bool> &rows)>;

//! Flat's read (DecideFrameRead): `expr` on the rows `rows` marks, as the one pass computed it. A computed value can
//! fail on a row that no surviving instance reads, so its failure stays with the value until a consumer reaches it, as
//! the pass keeps it for its expressions (DecideRowStore::Slot).
DecideFrameValues ReadDecideFrameRows(ClientContext &context, const Expression &expr, idx_t num_rows,
                                      const vector<bool> &rows, ChunkExprCache &chunk_expr_cache);

//! For each class of `row_to_class` (INVALID_INDEX: a row in none), the row holding the least value of `expr` -- a
//! column's spelling -- as ORDER BY places it.
vector<idx_t> LeastSpellingRows(const Expression &expr, const vector<idx_t> &row_to_class, idx_t num_classes,
                                ChunkExprCache &chunk_expr_cache, ClientContext &context);

//! The row that names each position of `map`: the one with its least spelling of the order key, which a collation lets
//! its rows differ in, so a name does not depend on row order (D69).
vector<idx_t> DecidePositionRows(ClientContext &context, const DecideFrameSpec &frame, const DecideFrameMap &map,
                                 ChunkExprCache &chunk_expr_cache);

//! D75: each class of `frame.partition_key` among the anchors lies in one timeline; an anchor in none is no witness.
void CheckDecideFramePartition(ClientContext &context, const DecideFrameSpec &frame, const DecideFrameMap &map,
                               const vector<DecideKey> &keys, PerGroupCache &per_group_cache,
                               idx_t num_rows, ChunkExprCache &chunk_expr_cache);

//! D75's message: the class `class_values` of the partition key (its elements `class_names`) lies in the two
//! timelines `left` and `right`.
string DecideFramePartitionMessage(const DecideFrameSpec &frame, const vector<string> &class_names,
                                   const vector<Value> &class_values, const vector<Value> &left,
                                   const vector<Value> &right);

//! D73's message: the position `position` names (the timeline's values, then the order key's) holds `rows` rows that
//! read different payloads.
string DecideAmbiguousPositionMessage(const DecideFrameSpec &frame, idx_t rows, const vector<Value> &position);

//! Whether the frame reads each peer of `map`: a point where some anchor selects it, a range where some window visits
//! it -- one that reads only NULLs included, else which row of a tied position comes first would decide whether its
//! rows are compared (D73).
vector<bool> DecideReadPeers(const DecideFrameMap &map);

//! A range over decisions skips the values it reads as NULL, as SQL's aggregate skips them (D84): `payload` are the
//! coefficients of its payload's terms, read at the rows its windows read, `fallback` those of its `else`, read at the
//! anchors that reach it, each through `read`; a NULL among a row's makes that row no member
//! (DecideFrameMap::SkipNullReads).
void SkipDecideRangeNullReads(const vector<const Expression *> &payload, const vector<const Expression *> &fallback,
                              DecideFrameMap &map, idx_t num_rows, const DecideFrameRead &read);

//! The values the frames' windows skip as NULL, read through `read` before any window is numbered or listed
//! (SkipDecideRangeNullReads): a range over decisions' payload coefficients and `else`s (its helper's, `links`), and a
//! range over data's `else`, which reads no value at an anchor where it is NULL (D86).
void SkipDecideFrameNullReads(const DecidePreparedModel &prepared, const vector<LogicalDecide::ReducerValueLink> &links,
                              vector<DecideFrameMap> &maps, idx_t num_rows, const DecideFrameRead &read);

//! Adds to the one pass's columns (DecideRowStore::AppendColumn) the value of each of `prepared.frame_columns`
//! on every row (DecideFrameColumn), each computed on the rows its kind names only: a
//! payload on the first row of each selected peer, which CheckDecideFramePeers holds for the
//! peer's other rows, or on the rows a range's windows read; an `else` on the anchors that
//! reach it. An anchor where the frame reads nothing is NULL in every column (D84). A payload, fallback or reduction
//! failure travels with the computed column at the anchors that read it, and raises only when a consumer reads one
//! of those anchors: an anchor without a surviving instance must not raise another instance's error.
void AppendDecideFrameColumns(ClientContext &context, const vector<DecideFrameSpec> &frames,
                              const vector<DecideFrameMap> &maps, const DecidePreparedModel &prepared,
                              idx_t num_rows, ChunkExprCache &chunk_expr_cache);

//! The frame columns AppendDecideFrameColumns appends, each with its rows' errors: every one before the first reducer
//! over data reading frames (AGGREGATE), each expression read through `read`.
struct DecideFrameColumnValues {
	vector<unique_ptr<Vector>> values;
	vector<std::map<idx_t, ErrorData>> errors;
};
DecideFrameColumnValues ComputeDecideFrameColumns(ClientContext &context, const vector<DecideFrameSpec> &frames,
                                                  const vector<DecideFrameMap> &maps,
                                                  const DecidePreparedModel &prepared, idx_t num_rows,
                                                  const DecideFrameRead &read);

//! A range frame's helper's classes (DecideOptimizer::RewriteFrameRanges): one per window of its frame, read by the
//! window's anchors; an anchor whose window reads nothing is in none.
KeyedMapping DecideFrameWindowMapping(const DecideFrameMap &map);

//! A range frame's helper's members (language/guide.md §5.6), one group per window that reads something, read at its
//! anchor: a member per row the window reads, and one for its `else`, the payload's coefficients `payload` of the
//! decisions `payload_vars` at those rows and the `else`'s `fallback` of `fallback_vars` at the anchor. A SUM or AVG
//! counts each value -- a row once per visit, the `else` once per missing position -- and an AVG divides by their
//! number; a MIN or MAX reads each once.
void DecideListFrameMembers(const DecideFrameMap &map, const LogicalDecide::ReducerValueLink &link,
                            const string &frame_text, const vector<CoefficientColumn> &payload,
                            const vector<idx_t> &payload_vars, const vector<CoefficientColumn> &fallback,
                            const vector<idx_t> &fallback_vars, ReducerValueMembers &out);

//! Whether a term reading its decision through `map` (DecideTerm::frame_read) reads, at `row`, an instance with no
//! value (D84): where the frame reads nothing, or at the row it selects where `absent` says the decision has none. A
//! missing position whose `else` stands in reads the `else`, not the term.
template <class ABSENT>
bool DecideFrameReadsAbsent(const DecideFrameMap &map, idx_t row, ABSENT &&absent) {
	if (map.Absent(row)) {
		return true;
	}
	const auto at = map.SelectedRow(row);
	return at != DConstants::INVALID_INDEX && absent(at);
}

//! The profile's counts of the frames' work (DECIDB_PROFILE): the positions they request, one per anchor of a point and
//! each value a range's window lists, once per window however many anchors share it.
void DecideCountFrames(const vector<DecideFrameMap> &maps);

//! The aggregate of `column`'s reducer (function and arguments; not its filter) over the rows `members` keeps of each
//! group into `out`, on each row its group's value: `group` numbers the rows' groups below `groups`, INVALID_INDEX for
//! a row in none, which reads NULL. The aggregate of a group with no member is SQL's over no row: NULL, and 0 for
//! `count`. A NaN among what it reads is an error (AggregateFold).
void ReduceDecideGroups(ClientContext &context, const DecideFrameColumn &column, const vector<bool> &members,
                        const vector<idx_t> &group, idx_t groups, ChunkExprCache &chunk_expr_cache, Vector &out);

//! The one pass's store with the columns `values_of` (one value per row each) after its own
//! (DecideRowStore::AppendColumn).
void AppendDecideColumns(ChunkExprCache &chunk_expr_cache, const vector<unique_ptr<Vector>> &values_of);

//! What a frame reads at each row of a position it reads, as D73 compares the rows (deck p. 110): each column `values`
//! read, by value, and the instance of each decision of `terms` -- term `i` the coefficient `values[terms[i].column]`
//! on decision `terms[i].decision` --, whatever its coefficient.
struct DecideFramePayload {
	vector<const Expression *> values;
	vector<DecideFramePayloadTerm> terms;
};

//! Each frame's payload: a point's VALUE and READ columns, with its payload terms
//! (DecidePreparedModel::frame_payload_terms); a range over data its reducer's argument; a range over decisions its
//! helper's member terms (`links`).
vector<DecideFramePayload> DecideFramePayloads(const vector<DecideFrameSpec> &frames,
                                               const DecidePreparedModel &prepared,
                                               const vector<LogicalDecide::ReducerValueLink> &links);

//! D73, once the keyed mappings are built: the rows of a position a frame reads -- a point's selected peer, each
//! position a range's window visits -- hold one signature of its payload, as the deck's frame container holds one per
//! position (p. 110): each column the payload reads, by value (`a + b` reads `a` and `b`), and each decision it reads,
//! by instance through the classes of `keyed_mappings` (`0 * x` reads `x`).
void CheckDecideFramePeers(ClientContext &context, const vector<DecideFrameSpec> &frames,
                           const vector<DecideFrameMap> &maps, const vector<DecideFramePayload> &payloads,
                           const vector<DecideVarScopeInfo> &variable_scopes,
                           const vector<KeyedMapping> &keyed_mappings, const vector<DecideKey> &keys,
                           idx_t num_rows, ChunkExprCache &chunk_expr_cache);

} // namespace duckdb
