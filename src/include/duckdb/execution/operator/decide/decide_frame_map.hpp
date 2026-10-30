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
// timeline (syntax_reference.md §5.6). Execution builds one DecideFrameMap per frame before
// any clause is evaluated, then appends the values the frames read to the input as columns
// (DecidePreparedModel::frame_columns), so every evaluator downstream reads them as data and
// none learns what a frame is. Defined in physical_decide_frames.cpp.
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
//! each of those rows, its anchors, the peer it selects.
struct DecideFrameMap {
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

	//! The row the frame reads at `row` -- its selected peer's first --, or INVALID_INDEX.
	idx_t SelectedRow(idx_t row) const {
		auto p = selected_peer[row];
		return p == DConstants::INVALID_INDEX ? DConstants::INVALID_INDEX : peer_rows[peer_begin[p]];
	}
	//! Whether `row` is an anchor whose position is missing: where its `else` stands in.
	bool Missing(idx_t row) const {
		return anchors[row] && selected_peer[row] == DConstants::INVALID_INDEX;
	}
	//! The rows `peer` holds.
	idx_t PeerSize(idx_t p) const {
		return peer_begin[p + 1] - peer_begin[p];
	}
};

//! `frame`'s timelines over `data`: its anchors split by its path's `by` keys and `within`,
//! each ordered by its order key -- descending when written, NULL where the connection's
//! `default_null_order` puts it (D68) -- into peers, and each anchor's selected peer.
DecideFrameMap BuildDecideFrameMap(ClientContext &context, const DecideFrameSpec &frame,
                                   const vector<DecideGroupKey> &group_keys, ColumnDataCollection &data,
                                   idx_t num_rows, ChunkExprCache &chunk_expr_cache);

//! Appends to `data`, after its own columns, the value of each of `prepared.frame_columns`
//! on every row (DecideFrameColumn), each computed on the rows its kind names only: a
//! payload on the first row of each selected peer, which CheckDecideFramePeers holds for the
//! peer's other rows, and an `else` on the anchors that reach it.
void AppendDecideFrameColumns(ClientContext &context, const vector<DecideFrameMap> &maps,
                              const DecidePreparedModel &prepared, ColumnDataCollection &data, idx_t num_rows,
                              ChunkExprCache &chunk_expr_cache);

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
