//===----------------------------------------------------------------------===//
//                         DeciDB
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
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/execution/operator/decide/decide_row_evaluation.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

#include <algorithm>
#include <iterator>

namespace duckdb {

//! `expr` on the rows `rows` marks, one slot per row and one more after them: NULL on the
//! rows not read, the extra slot for the caller to fill.
static Vector EvaluateOnRows(ClientContext &context, const Expression &expr, ColumnDataCollection &data, idx_t num_rows,
                             const vector<bool> &rows, ChunkExprCache &chunk_expr_cache) {
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

//! An anchor in no timeline (D75) selects nothing, whatever its `else`: the frame has no value there (D84).
static void MarkUntimedAbsent(DecideFrameMap &map) {
	for (idx_t row = 0; row < map.anchors.size(); row++) {
		if (map.Untimed(row)) {
			map.absent[row] = true;
		}
	}
}

//! The position `end` names on a timeline of `peers` positions for an anchor at `at` (language/guide.md §5.6):
//! `first` 0, `last` the last, `k previous` and `k next` the anchor's less or plus k -- off the timeline, it may be,
//! before a cyclic one wraps it. `k` is a 32-bit whole number (the grammar's Iconst) and positions count rows, so
//! every position fits an int64_t.
static int64_t LiftFramePosition(DecideFrameAnchor end, int64_t k, int64_t at, int64_t peers) {
	switch (end) {
	case DecideFrameAnchor::FIRST:
		return 0;
	case DecideFrameAnchor::LAST:
		return peers - 1;
	case DecideFrameAnchor::PREVIOUS:
		return at - k;
	case DecideFrameAnchor::NEXT:
		return at + k;
	}
	throw InternalException("DECIDE: a frame position of no known kind");
}

//! A range's windows over `map`'s timelines, `first_peer[t]` the first peer of timeline `t` (DecideFrameMap): every
//! `every`-th position from its first end to its last (LiftFramePosition), none when the first comes after the last.
static void ListRangeWindows(ClientContext &context, const DecideFrameSpec &frame, const vector<idx_t> &first_peer,
                             DecideFrameMap &map) {
	const idx_t num_rows = map.anchors.size();
	const idx_t timelines = first_peer.size() - 1;
	const idx_t limit = GetDecideFrameLimit(context);
	map.window.assign(num_rows, DConstants::INVALID_INDEX);
	map.absent.assign(num_rows, false);
	// The values listed so far, every row read and every `else` standing in, against the limit: `more` is checked
	// before the work that lists it, so the work stays bounded too.
	idx_t values = 0;
	auto check = [&](idx_t more) {
		if (more > limit || values > limit - more) {
			throw InvalidInputException("DECIDE: %s reads more than %llu values over its ranges, above "
			                            "decide_frame_limit (%llu). Narrow the range, or raise the setting.",
			                            frame.text, limit, limit);
		}
	};
	auto charge = [&](idx_t more) {
		check(more);
		values += more;
	};
	// Per peer of the timeline at hand, how often the list being built visits it, and the peers visited in the order
	// first visited; both reset as the list is closed.
	vector<idx_t> visits;
	vector<idx_t> visited;
	// The list read from position `at` of timeline `t`.
	auto list_from = [&](idx_t t, int64_t at) {
		const auto peers = static_cast<int64_t>(first_peer[t + 1] - first_peer[t]);
		const int64_t lo = LiftFramePosition(frame.from, frame.from_k, at, peers);
		const int64_t hi = LiftFramePosition(frame.to, frame.to_k, at, peers);
		const int64_t step = frame.every;
		idx_t missing = 0;
		if (lo <= hi) {
			const int64_t requested = (hi - lo) / step + 1;
			if (frame.cyclic) {
				// Every requested position exists, wrapped, and one visited again is read again (D76); each reads a
				// row at least.
				check(static_cast<idx_t>(requested));
				for (int64_t j = 0; j < requested; j++) {
					const auto p = static_cast<idx_t>(((lo + j * step) % peers + peers) % peers);
					if (visits[p]++ == 0) {
						visited.push_back(p);
					}
				}
			} else {
				// A position off the timeline is missing: counted, never visited.
				const int64_t first = lo >= 0 ? lo : lo + ((-lo + step - 1) / step) * step;
				const int64_t last = MinValue<int64_t>(hi, peers - 1);
				const int64_t present = first <= last ? (last - first) / step + 1 : 0;
				missing = static_cast<idx_t>(requested - present);
				// Under `all` a missing position leaves the window nothing to read: none of its rows is listed, so none
				// is evaluated.
				if (!(frame.all && missing > 0)) {
					check(static_cast<idx_t>(present));
					charge(frame.fallback ? missing : 0);
					for (int64_t q = first; q <= last; q += step) {
						visits[static_cast<idx_t>(q)] = 1;
						visited.push_back(static_cast<idx_t>(q));
					}
				}
			}
		}
		for (auto p : visited) {
			// A position is read at its first row, as a point reads it; CheckDecideFramePeers holds its other rows to
			// that row's payload (D73).
			const idx_t peer = first_peer[t] + p;
			charge(visits[p]);
			map.list_rows.push_back(map.peer_rows[map.peer_begin[peer]]);
			map.list_visits.push_back(visits[p]);
			visits[p] = 0;
		}
		visited.clear();
		map.list_missing.push_back(missing);
		map.list_begin.push_back(map.list_rows.size());
		return map.list_missing.size() - 1;
	};
	// What a list reads nothing from: no position (a reversed range), `all` meeting a missing one, or no requested
	// position that exists and no `else` -- `count` too (deck p. 50). A position whose value is NULL exists: `count`
	// reads 0 there.
	auto reads_nothing = [&](idx_t list) {
		const idx_t missing = map.list_missing[list];
		const bool rows = map.list_begin[list + 1] > map.list_begin[list];
		return (frame.all && missing > 0) || (!rows && (missing == 0 || !frame.fallback));
	};
	auto add_window = [&](idx_t anchor, idx_t list) {
		map.window_anchor.push_back(anchor);
		map.window_list.push_back(list);
		return map.window_anchor.size() - 1;
	};
	// An `else` that reads the anchor's own row parts a position's window at each anchor that reaches it; a constant
	// one reads alike at every anchor.
	const bool anchor_fallback = frame.fallback && !frame.fallback->IsFoldable();
	for (idx_t t = 0; t < timelines; t++) {
		if (context.interrupted) {
			throw InterruptException();
		}
		visits.assign(first_peer[t + 1] - first_peer[t], 0);
		if (!frame.Relative()) {
			// Neither end depends on the anchor: one window for the timeline.
			const auto list = list_from(t, 0);
			const auto window = add_window(map.peer_rows[map.peer_begin[first_peer[t]]], list);
			const bool nothing = reads_nothing(list);
			for (idx_t i = map.peer_begin[first_peer[t]]; i < map.peer_begin[first_peer[t + 1]]; i++) {
				map.window[map.peer_rows[i]] = window;
				map.absent[map.peer_rows[i]] = nothing;
			}
			continue;
		}
		for (idx_t peer = first_peer[t]; peer < first_peer[t + 1]; peer++) {
			if (context.interrupted) {
				throw InterruptException();
			}
			const auto list = list_from(t, static_cast<int64_t>(peer - first_peer[t]));
			const bool split = anchor_fallback && map.list_missing[list] > 0;
			const bool nothing = reads_nothing(list);
			auto window = DConstants::INVALID_INDEX;
			for (idx_t i = map.peer_begin[peer]; i < map.peer_begin[peer + 1]; i++) {
				const auto row = map.peer_rows[i];
				if (window == DConstants::INVALID_INDEX) {
					window = add_window(row, list);
				} else if (split) {
					// Each window of a parted position reads the list again: its values count.
					charge(map.ValueCount(window));
					window = add_window(row, list);
				}
				map.window[row] = window;
				map.absent[row] = nothing;
			}
		}
	}
}

DecideFrameMap BuildDecideFrameMap(ClientContext &context, const DecideFrameSpec &frame, const vector<DecideKey> &keys,
                                   PerGroupCache &per_group_cache, ColumnDataCollection &data, idx_t num_rows,
                                   ChunkExprCache &chunk_expr_cache) {
	DecideFrameMap map;
	map.range = frame.range;
	map.fallback = frame.fallback != nullptr;
	map.anchors =
	    frame.rows ? KeptRows(*frame.rows, chunk_expr_cache, context, data, num_rows) : vector<bool>(num_rows, true);
	idx_t timelines = 0;
	LookupOrBuildPerGroupIds(per_group_cache, keys, frame.timeline_key, chunk_expr_cache, context, data, num_rows,
	                         std::function<bool(idx_t)>(), &map.anchors, map.timeline, timelines);

	// Each anchor's order key as a sort key: bytes that compare as ORDER BY orders the
	// values, in the frame's direction and with NULL where `default_null_order` puts it
	// (D74), so equal keys are one peer and the peers sort by plain comparison.
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
	if (frame.range) {
		ListRangeWindows(context, frame, first_peer, map);
		MarkUntimedAbsent(map);
		return map;
	}

	// The position each anchor selects (language/guide.md §5.6): over peers, not over
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
	map.absent.assign(num_rows, false);
	for (idx_t row = 0; row < num_rows; row++) {
		map.absent[row] = map.Missing(row) && !frame.fallback;
	}
	MarkUntimedAbsent(map);
	return map;
}

//! The values of `frame`'s timeline key at `row`, as a message names them, and the names.
static vector<Value> TimelineValues(ClientContext &context, const DecideFrameSpec &frame, const vector<DecideKey> &keys,
                                    ColumnDataCollection &data, idx_t num_rows, idx_t row,
                                    ChunkExprCache &chunk_expr_cache) {
	vector<bool> at(num_rows, false);
	at[row] = true;
	vector<Value> values;
	for (auto &element : DecideKeyElements(keys, frame.timeline_key)) {
		values.push_back(ReadRowValues(*element.expression, chunk_expr_cache, context, data, &at)[row]);
	}
	return values;
}

//! D73: `frame` selects `peer`, whose rows read different payloads.
[[noreturn]] static void ThrowAmbiguousPeer(ClientContext &context, const DecideFrameSpec &frame,
                                            const DecideFrameMap &map, idx_t peer, const vector<DecideKey> &keys,
                                            ColumnDataCollection &data, idx_t num_rows,
                                            ChunkExprCache &chunk_expr_cache) {
	const auto row = map.peer_rows[map.peer_begin[peer]];
	vector<bool> at(num_rows, false);
	at[row] = true;
	auto names = frame.timeline_names;
	auto values = TimelineValues(context, frame, keys, data, num_rows, row, chunk_expr_cache);
	names.push_back(frame.order_name);
	values.push_back(ReadRowValues(*frame.order, chunk_expr_cache, context, data, &at)[row]);
	throw InvalidInputException("DECIDE: %s finds %llu rows at %s; a position must hold one row.", frame.text,
	                            map.PeerSize(peer), DescribeKeyValues(names, values));
}

void CheckDecideFramePartition(ClientContext &context, const DecideFrameSpec &frame, const DecideFrameMap &map,
                               const vector<DecideKey> &keys, PerGroupCache &per_group_cache,
                               ColumnDataCollection &data, idx_t num_rows, ChunkExprCache &chunk_expr_cache) {
	if (frame.partition_subject.empty()) {
		return;
	}
	vector<idx_t> ids;
	idx_t count = 0;
	LookupOrBuildPerGroupIds(per_group_cache, keys, frame.partition_key, chunk_expr_cache, context, data, num_rows,
	                         std::function<bool(idx_t)>(), &map.anchors, ids, count);
	vector<idx_t> first_row(count, DConstants::INVALID_INDEX);
	for (idx_t row = 0; row < num_rows; row++) {
		auto group = ids[row];
		if (group == DConstants::INVALID_INDEX) {
			continue;
		}
		if (first_row[group] == DConstants::INVALID_INDEX) {
			first_row[group] = row;
			continue;
		}
		auto first = first_row[group];
		if (map.timeline[row] == map.timeline[first]) {
			continue;
		}
		// The class, and the timeline's values at its first row and at this one.
		vector<bool> at(num_rows, false);
		at[first] = true;
		vector<string> class_names;
		vector<Value> class_values;
		for (auto &element : DecideKeyElements(keys, frame.partition_key)) {
			class_names.push_back(element.name);
			class_values.push_back(ReadRowValues(*element.expression, chunk_expr_cache, context, data, &at)[first]);
		}
		auto left = TimelineValues(context, frame, keys, data, num_rows, first, chunk_expr_cache);
		auto right = TimelineValues(context, frame, keys, data, num_rows, row, chunk_expr_cache);
		vector<string> left_text, right_text;
		for (idx_t i = 0; i < left.size(); i++) {
			left_text.push_back(DecideSqlLiteral(left[i]));
			right_text.push_back(DecideSqlLiteral(right[i]));
		}
		auto spanned = left.size() == 1 ? left_text[0] + ", " + right_text[0]
		                                : "(" + StringUtil::Join(left_text, ", ") + "), (" +
		                                      StringUtil::Join(right_text, ", ") + ")";
		auto names = StringUtil::Join(frame.timeline_names, ", ");
		if (frame.timeline_names.size() > 1) {
			names = "(" + names + ")";
		}
		auto spans =
		    class_names.empty() ? string("Its rows span") : DescribeKeyValues(class_names, class_values) + " spans";
		throw InvalidInputException("DECIDE: %s. %s %s = %s.", frame.partition_subject, spans, names, spanned);
	}
}

//! The states of one aggregate, one per window, initialized on construction and destroyed however the reduction ends:
//! what `list_aggregate` keeps per list.
struct FrameAggregateStates {
	FrameAggregateStates(const AggregateFunction &function_p, AggregateInputData &input_p, idx_t count_p)
	    : function(function_p), input(input_p), count(count_p), size(function.state_size(function)),
	      buffer(make_unsafe_uniq_array_uninitialized<data_t>(MaxValue<idx_t>(size * count, 1))),
	      pointers(LogicalType::POINTER, MaxValue<idx_t>(count, 1)) {
		auto states = FlatVector::GetData<data_ptr_t>(pointers);
		for (idx_t i = 0; i < count; i++) {
			states[i] = buffer.get() + size * i;
			function.initialize(function, states[i]);
		}
	}
	~FrameAggregateStates() {
		if (function.destructor) {
			function.destructor(pointers, input, count);
		}
	}
	data_ptr_t At(idx_t i) {
		return FlatVector::GetData<data_ptr_t>(pointers)[i];
	}

	const AggregateFunction &function;
	AggregateInputData &input;
	idx_t count;
	idx_t size;
	unsafe_unique_array<data_t> buffer;
	Vector pointers;
};

//! A range over data's REDUCED column (DecideFrameColumn) into `out`: the frame's own reducer run, through its
//! aggregate's state functions, over each window's values -- the payload at each row it reads, once per visit, and
//! the `else` at its anchor once per missing position -- so the value has SQL's type and arithmetic, and a NULL among
//! them is skipped as SQL's aggregate skips it (D84; `count` counts the others, and of none is 0). Each value is
//! computed on the rows that read it only. An anchor whose window reads nothing reads NULL.
static void ReduceRangeValues(ClientContext &context, const DecideFrameSpec &frame, const DecideFrameColumn &column,
                              const DecideFrameMap &map, ColumnDataCollection &data, idx_t num_rows,
                              ChunkExprCache &chunk_expr_cache, Vector &out) {
	auto &marker = column.value->Cast<BoundAggregateExpression>();
	if (marker.children.size() != 1) {
		throw InternalException("DECIDE: %s's column reads no reducer over its payload", frame.text);
	}
	const idx_t windows = map.WindowCount();
	if (windows == 0) {
		FlatVector::Validity(out).SetAllInvalid(num_rows); // no anchor: nothing to read
		return;
	}
	if (map.fallback != (column.fallback != nullptr)) {
		throw InternalException("DECIDE: %s's column and its map disagree on its else", frame.text);
	}
	// One source to reduce from: the payload's values at [0, n), the else's at [n, 2n).
	Vector source(marker.children[0]->return_type, 2 * num_rows);
	auto payload = EvaluateOnRows(context, *marker.children[0], data, num_rows, map.ReadRows(), chunk_expr_cache);
	VectorOperations::Copy(payload, source, num_rows, 0, 0);
	if (map.fallback) {
		auto fallback =
		    EvaluateOnRows(context, *column.fallback, data, num_rows, map.FallbackAnchors(), chunk_expr_cache);
		VectorOperations::Copy(fallback, source, num_rows, 0, num_rows);
	}

	ArenaAllocator allocator(BufferAllocator::Get(context));
	AggregateInputData input(marker.bind_info.get(), allocator);
	FrameAggregateStates states(marker.function, input, windows);
	SelectionVector selection(STANDARD_VECTOR_SIZE);
	Vector targets(LogicalType::POINTER, STANDARD_VECTOR_SIZE);
	auto target_states = FlatVector::GetData<data_ptr_t>(targets);
	idx_t pending = 0;
	auto flush = [&]() {
		if (pending > 0) {
			Vector slice(source, selection, pending);
			marker.function.update(&slice, input, 1, targets, pending);
			pending = 0;
		}
	};
	auto feed = [&](idx_t index, idx_t window, idx_t times) {
		for (idx_t i = 0; i < times; i++) {
			if (pending == STANDARD_VECTOR_SIZE) {
				flush();
			}
			selection.set_index(pending, index);
			target_states[pending++] = states.At(window);
		}
	};
	for (idx_t w = 0; w < windows; w++) {
		if (map.WindowAbsent(w)) {
			continue;
		}
		map.ForEachRead(w, [&](idx_t row, idx_t times) { feed(row, w, times); });
		const auto fallbacks = map.WindowFallbacks(w);
		if (fallbacks > 0) {
			feed(num_rows + map.window_anchor[w], w, fallbacks);
		}
	}
	flush();
	Vector values(column.type, windows);
	marker.function.finalize(states.pointers, input, values, windows, 0);
	// Each anchor reads its window's value; every other row, and an anchor whose window reads nothing, reads NULL.
	SelectionVector gather(num_rows);
	vector<bool> unread(num_rows, false);
	for (idx_t row = 0; row < num_rows; row++) {
		const auto window = map.anchors[row] ? map.window[row] : DConstants::INVALID_INDEX;
		unread[row] = window == DConstants::INVALID_INDEX || map.Absent(row);
		gather.set_index(row, unread[row] ? 0 : window);
	}
	VectorOperations::Copy(values, out, gather, num_rows, 0, 0);
	for (idx_t row = 0; row < num_rows; row++) {
		if (unread[row]) {
			FlatVector::SetNull(out, row, true);
		}
	}
}

void SkipDecideRangeNullReads(ClientContext &context, const vector<const Expression *> &payload,
                              const vector<const Expression *> &fallback, DecideFrameMap &map,
                              ColumnDataCollection &data, idx_t num_rows, ChunkExprCache &chunk_expr_cache) {
	auto null_on = [&](const vector<const Expression *> &exprs, const vector<bool> &rows) {
		vector<bool> nulls(num_rows, false);
		bool any = false;
		for (auto *expr : exprs) {
			auto values = EvaluateOnRows(context, *expr, data, num_rows, rows, chunk_expr_cache);
			UnifiedVectorFormat format;
			values.ToUnifiedFormat(num_rows, format);
			for (idx_t row = 0; row < num_rows; row++) {
				if (rows[row] && !format.validity.RowIsValid(format.sel->get_index(row))) {
					nulls[row] = any = true;
				}
			}
		}
		return any ? nulls : vector<bool>();
	};
	auto payload_nulls = null_on(payload, map.ReadRows());
	auto fallback_nulls = null_on(fallback, map.FallbackAnchors());
	if (!payload_nulls.empty() || !fallback_nulls.empty()) {
		map.SkipNullReads(std::move(payload_nulls), std::move(fallback_nulls));
	}
}

//! Whether the frame reads `peer`, for each peer of `map`: a point where some anchor selects it, a range where some
//! window that reads something visits it.
static vector<bool> ReadPeers(const DecideFrameMap &map) {
	vector<bool> read(map.peer_begin.size(), false);
	if (!map.range) {
		for (auto peer : map.selected_peer) {
			if (peer != DConstants::INVALID_INDEX) {
				read[peer] = true;
			}
		}
		return read;
	}
	for (idx_t w = 0; w < map.WindowCount(); w++) {
		if (map.WindowAbsent(w)) {
			continue;
		}
		const auto list = map.window_list[w];
		for (idx_t i = map.list_begin[list]; i < map.list_begin[list + 1]; i++) {
			read[map.peer[map.list_rows[i]]] = true;
		}
	}
	return read;
}

void AppendDecideFrameColumns(ClientContext &context, const vector<DecideFrameSpec> &frames,
                              const vector<DecideFrameMap> &maps, const DecidePreparedModel &prepared,
                              ColumnDataCollection &data, idx_t num_rows, ChunkExprCache &chunk_expr_cache) {
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
	// DEFINED reads the frame's other columns, so it is computed after them.
	vector<idx_t> defined;
	for (idx_t j = 0; j < columns.size(); j++) {
		auto &column = columns[j];
		auto &map = maps[column.frame];
		auto out = make_uniq<Vector>(column.type, num_rows);
		if (column.kind == DecideFrameColumn::Kind::DEFINED) {
			defined.push_back(j);
		} else if (column.kind == DecideFrameColumn::Kind::REDUCED) {
			ReduceRangeValues(context, frames[column.frame], column, map, data, num_rows, chunk_expr_cache, *out);
		} else {
			// A payload is read at the first row of each selected peer, the row every anchor of
			// the peer reads (D73 holds the others to it); an `else` at the anchors whose
			// position is missing, so an unused one is never evaluated (language/guide.md
			// §5.6). Where neither stands, a value is NULL, which is no value (D84), and so is a
			// coefficient read at a position with nothing to stand in for it; a coefficient
			// whose term does not stand is 0.
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
				missing_rows[row] = fallback && map.Missing(row) && !map.Absent(row);
			}
			// One source to gather from: the payload's values at [0, n), the default at n, the
			// else's at [n + 1, 2n + 1).
			Vector source(column.type, 2 * num_rows + 1);
			if (payload) {
				auto values = EvaluateOnRows(context, *payload, data, num_rows, selected_rows, chunk_expr_cache);
				VectorOperations::Copy(values, source, num_rows, 0, 0);
			}
			const bool no_value = column.kind == DecideFrameColumn::Kind::VALUE ||
			                      (column.kind == DecideFrameColumn::Kind::READ && !frames[column.frame].fallback);
			source.SetValue(num_rows, no_value ? Value(column.type) : Value::INTEGER(0).DefaultCastAs(column.type));
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
			// An anchor where the frame reads nothing -- one in no timeline among them, whatever its `else` -- has no
			// value in any of its columns.
			for (idx_t row = 0; row < num_rows; row++) {
				if (map.Absent(row)) {
					FlatVector::SetNull(*out, row, true);
				}
			}
		}
		types.push_back(column.type);
		values_of.push_back(std::move(out));
	}
	for (auto j : defined) {
		// TRUE where the frame has a value, NULL elsewhere: a range's window reads something, a point's every READ and
		// FALLBACK column holds a value.
		auto &out = *values_of[j];
		auto result = FlatVector::GetData<bool>(out);
		auto &validity = FlatVector::Validity(out);
		auto &map = maps[columns[j].frame];
		for (idx_t row = 0; row < num_rows; row++) {
			result[row] = true;
			if (map.range && (!map.anchors[row] || map.Absent(row))) {
				validity.SetInvalid(row);
			}
		}
		for (idx_t k = 0; k < columns.size() && !map.range; k++) {
			auto kind = columns[k].kind;
			if (columns[k].frame != columns[j].frame ||
			    (kind != DecideFrameColumn::Kind::READ && kind != DecideFrameColumn::Kind::FALLBACK)) {
				continue;
			}
			for (idx_t row = 0; row < num_rows; row++) {
				if (values_of[k]->GetValue(row).IsNull()) {
					validity.SetInvalid(row);
				}
			}
		}
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

vector<DecideFramePayload> DecideFramePayloads(const vector<DecideFrameSpec> &frames,
                                               const DecidePreparedModel &prepared,
                                               const vector<LogicalDecide::ReducerValueLink> &links) {
	vector<DecideFramePayload> payloads(frames.size());
	for (idx_t j = 0; j < prepared.frame_columns.size(); j++) {
		auto &column = prepared.frame_columns[j];
		auto &payload = payloads[column.frame];
		if (column.kind == DecideFrameColumn::Kind::REDUCED) {
			payload.values.push_back(column.value->Cast<BoundAggregateExpression>().children[0].get());
			continue;
		}
		if (column.kind != DecideFrameColumn::Kind::VALUE && column.kind != DecideFrameColumn::Kind::READ) {
			continue;
		}
		// A point's payload term names its READ column by its index among the frame columns.
		if (column.frame < prepared.frame_payload_terms.size()) {
			for (auto &term : prepared.frame_payload_terms[column.frame]) {
				if (term.column == j) {
					payload.terms.push_back(DecideFramePayloadTerm {term.decision, payload.values.size(), term.sign});
				}
			}
		}
		payload.values.push_back(column.value.get());
	}
	for (auto &link : links) {
		if (link.frame == DConstants::INVALID_INDEX) {
			continue;
		}
		auto &payload = payloads[link.frame];
		for (auto &term : link.inner_terms) {
			payload.terms.push_back(DecideFramePayloadTerm {term.variable_index, payload.values.size(), term.sign});
			payload.values.push_back(term.coefficient.get());
		}
	}
	return payloads;
}

void CheckDecideFramePeers(ClientContext &context, const vector<DecideFrameSpec> &frames,
                           const vector<DecideFrameMap> &maps, const vector<DecideFramePayload> &payloads,
                           const vector<DecideVarScopeInfo> &variable_scopes,
                           const vector<KeyedMapping> &keyed_mappings, const vector<DecideKey> &keys,
                           ColumnDataCollection &data, idx_t num_rows, ChunkExprCache &chunk_expr_cache) {
	for (idx_t f = 0; f < maps.size(); f++) {
		auto &map = maps[f];
		auto &terms = payloads[f].terms;
		// The peers the frame reads of several rows -- the only ones whose rows can disagree -- and their rows.
		auto read = ReadPeers(map);
		vector<idx_t> tied;
		vector<bool> tied_rows(num_rows, false);
		for (idx_t peer = 0; peer + 1 < map.peer_begin.size(); peer++) {
			if (read[peer] && map.PeerSize(peer) > 1) {
				tied.push_back(peer);
				for (idx_t i = map.peer_begin[peer]; i < map.peer_begin[peer + 1]; i++) {
					tied_rows[map.peer_rows[i]] = true;
				}
			}
		}
		if (tied.empty()) {
			continue;
		}
		// What the frame reads on those rows: over data its payload, over decisions the coefficients of its payload's
		// terms.
		vector<Vector> values;
		for (auto *value : payloads[f].values) {
			values.push_back(EvaluateOnRows(context, *value, data, num_rows, tied_rows, chunk_expr_cache));
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
			if (scope.IsGlobal()) {
				return 0;
			}
			return scope.IsKeyed() ? keyed_mappings[scope.keyed_scope_idx].row_to_class[row] : row;
		};
		auto form_at = [&](idx_t row) {
			DecidePayloadForm form;
			for (auto &term : terms) {
				auto coefficient = values[term.column].GetValue(row).GetValue<double>();
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
			// read the same values, and the NULL is then no value where it is read (D84).
			bool by_value = terms.empty();
			for (idx_t i = map.peer_begin[peer]; !by_value && i < map.peer_begin[peer + 1]; i++) {
				by_value = has_null(map.peer_rows[i]);
			}
			const auto first_form = by_value ? DecidePayloadForm() : form_at(first);
			for (idx_t i = map.peer_begin[peer] + 1; i < map.peer_begin[peer + 1]; i++) {
				const auto row = map.peer_rows[i];
				if (by_value ? !same_values(first, row) : form_at(row) != first_form) {
					ThrowAmbiguousPeer(context, frames[f], map, peer, keys, data, num_rows, chunk_expr_cache);
				}
			}
		}
	}
}

} // namespace duckdb
