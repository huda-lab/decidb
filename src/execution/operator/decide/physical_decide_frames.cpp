//===----------------------------------------------------------------------===//
//                         DecidB
//
// src/execution/operator/decide/physical_decide_frames.cpp
//
// The frame selectors' timelines and the values they read (decide_frame_map.hpp).
//
//===----------------------------------------------------------------------===//
#include "duckdb/execution/operator/decide/decide_frame_map.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/operator/decide/decide_row_evaluation.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

#include <algorithm>
#include <iterator>

namespace duckdb {

//! `expr` on the rows `rows` marks, one slot per row and one more after them: NULL on the
//! rows not read, the extra slot for the caller to fill.
static Vector EvaluateOnRows(ClientContext &context, const Expression &expr, ColumnDataCollection &data,
                             idx_t num_rows, const vector<bool> &rows, ChunkExprCache &chunk_expr_cache) {
	auto &lowered = CachedTransformToChunkExpression(chunk_expr_cache, expr, context);
	Vector values(lowered.return_type, num_rows + 1);
	ExpressionExecutor executor(context);
	executor.AddExpression(lowered);
	ColumnDataScanState scan;
	data.InitializeScan(scan);
	DataChunk chunk;
	chunk.Initialize(context, data.Types());
	DataChunk result;
	result.Initialize(context, {lowered.return_type});
	idx_t offset = 0;
	while (data.Scan(scan, chunk)) {
		result.Reset();
		ExecuteOnReadRows(executor, chunk, offset, &rows, result);
		VectorOperations::Copy(result.data[0], values, chunk.size(), 0, offset);
		offset += chunk.size();
	}
	return values;
}

//! The key expressions of `frame`'s timelines: its path's `by` keys, then `within`.
static vector<const Expression *> TimelineKey(ClientContext &context, const DecideFrameSpec &frame,
                                              const vector<DecideGroupKey> &group_keys,
                                              ChunkExprCache &chunk_expr_cache) {
	vector<const Expression *> keys;
	if (frame.scope_key != DConstants::INVALID_INDEX) {
		for (auto &expr : group_keys[frame.scope_key].expressions) {
			keys.push_back(&CachedTransformToChunkExpression(chunk_expr_cache, *expr, context));
		}
	}
	for (auto &expr : frame.within) {
		keys.push_back(&CachedTransformToChunkExpression(chunk_expr_cache, *expr, context));
	}
	return keys;
}

DecideFrameMap BuildDecideFrameMap(ClientContext &context, const DecideFrameSpec &frame,
                                   const vector<DecideGroupKey> &group_keys, ColumnDataCollection &data,
                                   idx_t num_rows, ChunkExprCache &chunk_expr_cache) {
	if (frame.range) {
		throw InternalException("DECIDE: a range frame reached execution: %s", frame.text);
	}
	DecideFrameMap map;
	map.anchors = frame.rows ? KeptRows(*frame.rows, chunk_expr_cache, context, data, num_rows)
	                         : vector<bool>(num_rows, true);
	idx_t timelines = 0;
	BuildGroupIds(TimelineKey(context, frame, group_keys, chunk_expr_cache), context, data, num_rows,
	              std::function<bool(idx_t)>(), map.timeline, timelines, nullptr, &map.anchors);

	// Each anchor's order key as a sort key: bytes that compare as ORDER BY orders the
	// values, in the frame's direction and with NULL where `default_null_order` puts it
	// (D68), so equal keys are one peer and the peers sort by plain comparison.
	auto order_type = frame.descending ? OrderType::DESCENDING : OrderType::ASCENDING;
	OrderModifiers modifiers(order_type,
	                         DBConfig::GetConfig(context).ResolveNullOrder(order_type, OrderByNullType::ORDER_DEFAULT));
	vector<string> sort_keys(num_rows);
	{
		auto &order = CachedTransformToChunkExpression(chunk_expr_cache, *frame.order, context);
		ExpressionExecutor executor(context);
		executor.AddExpression(order);
		ColumnDataScanState scan;
		data.InitializeScan(scan);
		DataChunk chunk;
		chunk.Initialize(context, data.Types());
		DataChunk result;
		result.Initialize(context, {order.return_type});
		idx_t offset = 0;
		while (data.Scan(scan, chunk)) {
			const idx_t count = chunk.size();
			result.Reset();
			ExecuteOnReadRows(executor, chunk, offset, &map.anchors, result);
			Vector keys(LogicalType::BLOB, count);
			CreateSortKeyHelpers::CreateSortKey(result.data[0], count, modifiers, keys);
			UnifiedVectorFormat format;
			keys.ToUnifiedFormat(count, format);
			auto key_data = UnifiedVectorFormat::GetData<string_t>(format);
			for (idx_t i = 0; i < count; i++) {
				if (map.anchors[offset + i]) {
					sort_keys[offset + i] = key_data[format.sel->get_index(i)].GetString();
				}
			}
			offset += count;
		}
	}

	// Each timeline's anchors in order, cut into peers: a stable sort keeps a peer's rows
	// ascending, and its first row is the one a selection reads.
	vector<vector<idx_t>> rows_of(timelines);
	for (idx_t row = 0; row < num_rows; row++) {
		if (map.anchors[row] && map.timeline[row] != DConstants::INVALID_INDEX) {
			rows_of[map.timeline[row]].push_back(row);
		}
	}
	map.peer.assign(num_rows, DConstants::INVALID_INDEX);
	map.selected_peer.assign(num_rows, DConstants::INVALID_INDEX);
	vector<idx_t> first_peer(timelines + 1, 0);
	for (idx_t t = 0; t < timelines; t++) {
		if (context.interrupted) {
			throw InterruptException();
		}
		auto &rows = rows_of[t];
		std::stable_sort(rows.begin(), rows.end(),
		                 [&](idx_t left, idx_t right) { return sort_keys[left] < sort_keys[right]; });
		first_peer[t] = map.peer_begin.size();
		for (idx_t i = 0; i < rows.size(); i++) {
			if (i == 0 || sort_keys[rows[i]] != sort_keys[rows[i - 1]]) {
				map.peer_begin.push_back(map.peer_rows.size());
			}
			map.peer_rows.push_back(rows[i]);
			map.peer[rows[i]] = map.peer_begin.size() - 1;
		}
	}
	first_peer[timelines] = map.peer_begin.size();
	map.peer_begin.push_back(map.peer_rows.size());

	// The position each anchor selects (syntax_reference.md §5.6): over peers, not over
	// distances in the order key.
	const auto k = frame.from_k;
	for (idx_t row = 0; row < num_rows; row++) {
		if (map.peer[row] == DConstants::INVALID_INDEX) {
			continue;
		}
		auto t = map.timeline[row];
		const auto peers = static_cast<int64_t>(first_peer[t + 1] - first_peer[t]);
		const auto at = static_cast<int64_t>(map.peer[row] - first_peer[t]);
		int64_t position;
		switch (frame.from) {
		case DecideFrameAnchor::FIRST:
			position = 0;
			break;
		case DecideFrameAnchor::LAST:
			position = peers - 1;
			break;
		case DecideFrameAnchor::PREVIOUS:
			position = frame.cyclic ? at - k % peers : (k > at ? -1 : at - k);
			break;
		case DecideFrameAnchor::NEXT:
			position = frame.cyclic ? at + k % peers : (k >= peers - at ? peers : at + k);
			break;
		default:
			throw InternalException("DECIDE: a frame position of no known kind");
		}
		if (frame.cyclic) {
			position = ((position % peers) + peers) % peers;
		} else if (position < 0 || position >= peers) {
			continue; // missing: the frame selects nothing here
		}
		map.selected_peer[row] = first_peer[t] + static_cast<idx_t>(position);
	}
	return map;
}

//! D67: `frame` selects `peer`, whose rows read different payloads.
[[noreturn]] static void ThrowAmbiguousPeer(ClientContext &context, const DecideFrameSpec &frame,
                                            const DecideFrameMap &map, idx_t peer,
                                            const vector<DecideGroupKey> &group_keys, ColumnDataCollection &data,
                                            idx_t num_rows, ChunkExprCache &chunk_expr_cache) {
	const auto row = map.peer_rows[map.peer_begin[peer]];
	vector<bool> at(num_rows, false);
	at[row] = true;
	auto value_at = [&](const Expression &expr) {
		return EvaluateRowValues(expr, chunk_expr_cache, context, data, num_rows, at)[row];
	};
	vector<string> names = frame.timeline_names;
	vector<Value> values;
	if (frame.scope_key != DConstants::INVALID_INDEX) {
		for (auto &expr : group_keys[frame.scope_key].expressions) {
			values.push_back(value_at(*expr));
		}
	}
	for (auto &expr : frame.within) {
		values.push_back(value_at(*expr));
	}
	names.push_back(frame.order_name);
	values.push_back(value_at(*frame.order));
	throw InvalidInputException("DECIDE: %s finds %llu rows at %s; a position must hold one row.", frame.text,
	                            map.PeerSize(peer), DescribeKeyValues(names, values));
}

//! Whether some anchor selects `peer`, for each peer of `map`.
static vector<bool> SelectedPeers(const DecideFrameMap &map) {
	vector<bool> selected(map.peer_begin.size(), false);
	for (auto peer : map.selected_peer) {
		if (peer != DConstants::INVALID_INDEX) {
			selected[peer] = true;
		}
	}
	return selected;
}

void AppendDecideFrameColumns(ClientContext &context, const vector<DecideFrameMap> &maps,
                              const DecidePreparedModel &prepared, ColumnDataCollection &data, idx_t num_rows,
                              ChunkExprCache &chunk_expr_cache) {
	auto &columns = prepared.frame_columns;
	if (columns.empty()) {
		return;
	}
	// Stage 05 numbered these columns after the input's own, before any other was added.
	if (data.ColumnCount() != prepared.frame_column_base) {
		throw InternalException("DECIDE: the frame columns follow %llu input columns, not %llu",
		                        prepared.frame_column_base, data.ColumnCount());
	}
	auto types = data.Types();
	vector<unique_ptr<Vector>> values_of;
	for (auto &column : columns) {
		auto &map = maps[column.frame];
		auto out = make_uniq<Vector>(column.type, num_rows);
		if (column.kind == DecideFrameColumn::Kind::PRESENT) {
			auto present = FlatVector::GetData<bool>(*out);
			for (idx_t row = 0; row < num_rows; row++) {
				present[row] = map.anchors[row] && map.selected_peer[row] != DConstants::INVALID_INDEX;
			}
		} else {
			// A payload is read at the first row of each selected peer, the row every anchor of
			// the peer reads (D67 holds the others to it); an `else` at the anchors whose
			// position is missing, so an unused one is never evaluated (syntax_reference.md
			// §5.6). Where neither stands, a coefficient is 0 and a value NULL.
			const bool fallback_only = column.kind == DecideFrameColumn::Kind::FALLBACK;
			const Expression *payload = fallback_only ? nullptr : column.value.get();
			const Expression *fallback = fallback_only ? column.value.get() : column.fallback.get();
			vector<bool> selected_rows(num_rows, false);
			vector<bool> missing_rows(num_rows, false);
			for (idx_t row = 0; row < num_rows; row++) {
				auto selected_row = payload ? map.SelectedRow(row) : DConstants::INVALID_INDEX;
				if (selected_row != DConstants::INVALID_INDEX) {
					selected_rows[selected_row] = true;
				}
				missing_rows[row] = fallback && map.Missing(row);
			}
			// One source to gather from: the payload's values at [0, n), the default at n, the
			// else's at [n + 1, 2n + 1).
			Vector source(column.type, 2 * num_rows + 1);
			if (payload) {
				auto values = EvaluateOnRows(context, *payload, data, num_rows, selected_rows, chunk_expr_cache);
				VectorOperations::Copy(values, source, num_rows, 0, 0);
			}
			source.SetValue(num_rows, column.kind == DecideFrameColumn::Kind::VALUE
			                              ? Value(column.type)
			                              : Value::INTEGER(0).DefaultCastAs(column.type));
			if (fallback) {
				auto values = EvaluateOnRows(context, *fallback, data, num_rows, missing_rows, chunk_expr_cache);
				VectorOperations::Copy(values, source, num_rows, 0, num_rows + 1);
			}
			SelectionVector gather(num_rows);
			for (idx_t row = 0; row < num_rows; row++) {
				auto selected_row = payload ? map.SelectedRow(row) : DConstants::INVALID_INDEX;
				gather.set_index(row, selected_row != DConstants::INVALID_INDEX ? selected_row
				                      : missing_rows[row]                       ? num_rows + 1 + row
				                                                                : num_rows);
			}
			VectorOperations::Copy(source, *out, gather, num_rows, 0, 0);
		}
		types.push_back(column.type);
		values_of.push_back(std::move(out));
	}

	// The input again, these columns after its own.
	ColumnDataCollection widened(context, types);
	ColumnDataAppendState append;
	widened.InitializeAppend(append);
	ColumnDataScanState scan;
	data.InitializeScan(scan);
	DataChunk chunk;
	chunk.Initialize(context, data.Types());
	DataChunk wide;
	wide.InitializeEmpty(types);
	idx_t offset = 0;
	while (data.Scan(scan, chunk)) {
		const idx_t count = chunk.size();
		for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
			wide.data[col].Reference(chunk.data[col]);
		}
		for (idx_t j = 0; j < values_of.size(); j++) {
			wide.data[chunk.ColumnCount() + j].Slice(*values_of[j], offset, offset + count);
		}
		wide.SetCardinality(count);
		widened.Append(append, wide);
		offset += count;
	}
	// Emptied, the collection takes the wider types, and the widened rows' segments move in.
	data.Reset();
	data.Types() = types;
	data.Combine(widened);
}

//! A payload over decisions at one row as the model reads it: the coefficient on each
//! decision instance, keyed (decision, instance), and the constant, keyed
//! (INVALID_INDEX, 0); no zero.
using DecidePayloadForm = map<pair<idx_t, idx_t>, double>;

void CheckDecideFramePeers(ClientContext &context, const vector<DecideFrameSpec> &frames,
                           const vector<DecideFrameMap> &maps, const DecidePreparedModel &prepared,
                           const vector<DecideVarScopeInfo> &variable_scopes,
                           const vector<EntityMapping> &entity_mappings, const vector<DecideGroupKey> &group_keys,
                           ColumnDataCollection &data, idx_t num_rows, ChunkExprCache &chunk_expr_cache) {
	static const vector<DecideFramePayloadTerm> NO_TERMS;
	for (idx_t f = 0; f < maps.size(); f++) {
		auto &map = maps[f];
		auto &terms = f < prepared.frame_payload_terms.size() ? prepared.frame_payload_terms[f] : NO_TERMS;
		// The selected peers of several rows -- the only ones whose rows can disagree -- and
		// their rows.
		auto selected = SelectedPeers(map);
		vector<idx_t> tied;
		vector<bool> tied_rows(num_rows, false);
		for (idx_t peer = 0; peer + 1 < map.peer_begin.size(); peer++) {
			if (selected[peer] && map.PeerSize(peer) > 1) {
				tied.push_back(peer);
				for (idx_t i = map.peer_begin[peer]; i < map.peer_begin[peer + 1]; i++) {
					tied_rows[map.peer_rows[i]] = true;
				}
			}
		}
		if (tied.empty()) {
			continue;
		}
		// What the frame reads on those rows: over data its payload (its VALUE column's), over
		// decisions the coefficients of its payload's terms (its READ columns').
		vector<idx_t> slot_of(prepared.frame_columns.size(), DConstants::INVALID_INDEX);
		vector<Vector> values;
		for (idx_t j = 0; j < prepared.frame_columns.size(); j++) {
			auto &column = prepared.frame_columns[j];
			const bool payload = column.kind == DecideFrameColumn::Kind::VALUE ||
			                     column.kind == DecideFrameColumn::Kind::READ;
			if (payload && column.frame == f) {
				slot_of[j] = values.size();
				values.push_back(EvaluateOnRows(context, *column.value, data, num_rows, tied_rows, chunk_expr_cache));
			}
		}
		auto same_values = [&](idx_t a, idx_t b) {
			for (auto &column : values) {
				if (!Value::NotDistinctFrom(column.GetValue(a), column.GetValue(b))) {
					return false;
				}
			}
			return true;
		};
		auto has_null = [&](idx_t row) {
			for (auto &column : values) {
				if (column.GetValue(row).IsNull()) {
					return true;
				}
			}
			return false;
		};
		// A decision's instance at a row: the row for a per-row one, its class for a keyed one;
		// a query-wide one, like the constant, has one instance everywhere.
		auto instance = [&](idx_t decision, idx_t row) -> idx_t {
			if (decision == DConstants::INVALID_INDEX) {
				return 0;
			}
			auto scope = decision < variable_scopes.size() ? variable_scopes[decision] : DecideVarScopeInfo::Row();
			if (scope.IsScalar()) {
				return 0;
			}
			return scope.IsEntity() ? entity_mappings[scope.entity_scope_idx].row_to_entity[row] : row;
		};
		auto form_at = [&](idx_t row) {
			DecidePayloadForm form;
			for (auto &term : terms) {
				auto coefficient = values[slot_of[term.column]].GetValue(row).GetValue<double>();
				form[make_pair(term.decision, instance(term.decision, row))] += term.sign * coefficient;
			}
			for (auto it = form.begin(); it != form.end();) {
				it = it->second == 0.0 ? form.erase(it) : std::next(it);
			}
			return form;
		};
		for (auto peer : tied) {
			const auto first = map.peer_rows[map.peer_begin[peer]];
			// A NULL is no coefficient: a peer holding one reads one payload only where its rows
			// read the same values, and the NULL then fails where it is read (D78).
			bool by_value = terms.empty();
			for (idx_t i = map.peer_begin[peer]; !by_value && i < map.peer_begin[peer + 1]; i++) {
				by_value = has_null(map.peer_rows[i]);
			}
			const auto first_form = by_value ? DecidePayloadForm() : form_at(first);
			for (idx_t i = map.peer_begin[peer] + 1; i < map.peer_begin[peer + 1]; i++) {
				const auto row = map.peer_rows[i];
				if (by_value ? !same_values(first, row) : form_at(row) != first_form) {
					ThrowAmbiguousPeer(context, frames[f], map, peer, group_keys, data, num_rows, chunk_expr_cache);
				}
			}
		}
	}
}

} // namespace duckdb
