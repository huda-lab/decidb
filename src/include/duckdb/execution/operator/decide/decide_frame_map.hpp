//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/execution/operator/decide/decide_frame_map.hpp
//
// The frame selectors' timelines over the materialized input (stage 08).
//===----------------------------------------------------------------------===//
//
// A point frame `at(F [else v]: e) over (S [desc] [cyclic] [within P])` reads, at each row of
// its scoped expression, the payload `e` at the row its position F selects on that row's
// timeline; a range `agg(from F1 to F2 [every d] [else v | all]: e)` reads it at every row
// of every position from F1 to F2 (syntax_reference.md §5.6). Execution builds one
// DecideFrameMap per frame before any clause is evaluated, then appends the values the
// frames read to the input as columns (DecidePreparedModel::frame_columns), so every
// evaluator downstream reads them as data and none learns what a frame is; a range over
// decisions is a helper decision instead, defined per window from the map. Defined in
// physical_decide_frames.cpp.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/decidb/formulation/solver_input.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"
#include "duckdb/planner/decide/decide_prepared_model.hpp"
#include "duckdb/planner/operator/decide/logical_decide.hpp"

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
	//! Per row: an anchor where the frame reads nothing -- a point's missing position with
	//! no `else`; a range that requests no position, meets `all`, or finds no value -- which
	//! skips the instance or drops the member (syntax_reference.md §5.6).
	vector<bool> absent;

	//! A range's windows: the ranges its anchors read, one per timeline when neither end
	//! depends on the anchor, else one per position (peer) -- and one per anchor where it
	//! reaches an `else` that reads the anchor's own row. Window w is read by the anchors
	//! `window` maps to it, its `else` at `window_anchor[w]`, and reads list
	//! `window_list[w]`; windows of one position share their list. List l reads the rows
	//! list_rows[list_begin[l], list_begin[l + 1]), each `list_visits` times (a cyclic range
	//! may visit a position again, D70), in the order its positions are requested, and finds
	//! `list_missing[l]` of its requested positions missing.
	vector<idx_t> window;
	vector<idx_t> window_anchor;
	vector<idx_t> window_list;
	vector<idx_t> list_begin {0};
	vector<idx_t> list_rows;
	vector<idx_t> list_visits;
	vector<idx_t> list_missing;

	//! The row the frame reads at `row` -- its selected peer's first --, or INVALID_INDEX.
	idx_t SelectedRow(idx_t row) const {
		auto p = selected_peer[row];
		return p == DConstants::INVALID_INDEX ? DConstants::INVALID_INDEX : peer_rows[peer_begin[p]];
	}
	//! Whether `row` is an anchor some requested position of which is missing: where its
	//! `else` stands in.
	bool Missing(idx_t row) const {
		if (!anchors[row]) {
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
	//! Whether window `w` reads nothing: all its anchors are absent.
	bool WindowAbsent(idx_t w) const {
		return absent[window_anchor[w]];
	}
	//! The `else` values window `w` reads: one per missing position, with an `else`.
	idx_t WindowFallbacks(idx_t w) const {
		return fallback ? list_missing[window_list[w]] : 0;
	}
	//! The values window `w` reads, each row once per visit and each `else` counted.
	idx_t ValueCount(idx_t w) const {
		idx_t values = WindowFallbacks(w);
		const auto list = window_list[w];
		for (idx_t i = list_begin[list]; i < list_begin[list + 1]; i++) {
			values += list_visits[i];
		}
		return values;
	}
	//! Each row window `w` reads, with how often: `visit(row, times)`.
	template <class VISIT>
	void ForEachRead(idx_t w, VISIT &&visit) const {
		const auto list = window_list[w];
		for (idx_t i = list_begin[list]; i < list_begin[list + 1]; i++) {
			visit(list_rows[i], list_visits[i]);
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
};

//! `frame`'s timelines over `data`: its anchors split by its path's `by` keys and `within`,
//! each ordered by its order key -- descending when written, NULL where the connection's
//! `default_null_order` puts it (D68) -- into peers, and each anchor's selected peer or
//! window. A range listing more values than `decide_frame_limit` is refused as it is
//! listed, and an interrupt is polled per timeline and per window.
DecideFrameMap BuildDecideFrameMap(ClientContext &context, const DecideFrameSpec &frame,
                                   const vector<DecideGroupKey> &group_keys, ColumnDataCollection &data,
                                   idx_t num_rows, ChunkExprCache &chunk_expr_cache);

//! Appends to `data`, after its own columns, the value of each of `prepared.frame_columns`
//! on every row (DecideFrameColumn), each computed on the rows its kind names only: a
//! payload on the first row of each selected peer, which CheckDecideFramePeers holds for the
//! peer's other rows, or on the rows a range's windows read; an `else` on the anchors that
//! reach it.
void AppendDecideFrameColumns(ClientContext &context, const vector<DecideFrameSpec> &frames,
                              const vector<DecideFrameMap> &maps, const DecidePreparedModel &prepared,
                              ColumnDataCollection &data, idx_t num_rows, ChunkExprCache &chunk_expr_cache);

//! D67, once the scope checks have passed: the rows of a selected peer read one payload. A
//! frame over data holds one value of its payload; a frame over decisions one linear
//! form of its payload (`prepared.frame_payload_terms`) -- one constant and one coefficient
//! on each decision instance, the instances through the classes of `entity_mappings` -- so
//! `0 * x` reads no instance of `x`.
void CheckDecideFramePeers(ClientContext &context, const vector<DecideFrameSpec> &frames,
                           const vector<DecideFrameMap> &maps, const DecidePreparedModel &prepared,
                           const vector<DecideVarScopeInfo> &variable_scopes,
                           const vector<EntityMapping> &entity_mappings, const vector<DecideGroupKey> &group_keys,
                           ColumnDataCollection &data, idx_t num_rows, ChunkExprCache &chunk_expr_cache);

} // namespace duckdb
