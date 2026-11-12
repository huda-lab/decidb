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
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/execution/operator/decide/decide_row_evaluation.hpp"
#include "duckdb/execution/operator/decide/decide_row_store.hpp"
#include "duckdb/execution/operator/decide/decide_value_extraction.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/decide/decide_source_provenance.hpp"
#include "duckdb/planner/expression_iterator.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>

namespace duckdb {

static bool StopsFrameEvaluation(const ErrorData &error) {
	auto type = error.Type();
	return type == ExceptionType::INTERRUPT || type == ExceptionType::OUT_OF_MEMORY || type == ExceptionType::FATAL;
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
			// A position is read at its first row, as a point reads it; its other rows are held to that row's payload
			// (D73, DecideTreeColumns::CheckPositions).
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

void SelectDecideFramePeers(ClientContext &context, const DecideFrameSpec &frame, const vector<idx_t> &first_peer,
                            DecideFrameMap &map) {
	const idx_t num_rows = map.anchors.size();
	map.selected_peer.assign(num_rows, DConstants::INVALID_INDEX);
	if (frame.range) {
		ListRangeWindows(context, frame, first_peer, map);
		MarkUntimedAbsent(map);
		return;
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
}

string DecideAmbiguousPositionMessage(const DecideFrameSpec &frame, idx_t rows, const vector<Value> &position) {
	auto names = frame.timeline_names;
	names.push_back(frame.order_name);
	return StringUtil::Format("DECIDE: %s finds %llu rows at %s; a position must hold one row.", frame.text, rows,
	                          DescribeKeyValues(names, position));
}

string DecideFramePartitionMessage(const DecideFrameSpec &frame, const vector<string> &class_names,
                                   const vector<Value> &class_values, const vector<Value> &left,
                                   const vector<Value> &right) {
	vector<string> left_text, right_text;
	for (idx_t i = 0; i < left.size(); i++) {
		left_text.push_back(DecideSqlLiteral(left[i]));
		right_text.push_back(DecideSqlLiteral(right[i]));
	}
	auto spanned = left.size() == 1
	                   ? left_text[0] + ", " + right_text[0]
	                   : "(" + StringUtil::Join(left_text, ", ") + "), (" + StringUtil::Join(right_text, ", ") + ")";
	auto names = StringUtil::Join(frame.timeline_names, ", ");
	if (frame.timeline_names.size() > 1) {
		names = "(" + names + ")";
	}
	auto spans =
	    class_names.empty() ? string("Its rows span") : DescribeKeyValues(class_names, class_values) + " spans";
	return StringUtil::Format("DECIDE: %s. %s %s = %s.", frame.partition_subject, spans, names, spanned);
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

//! Whether one of `values`' first `count` entries is a NaN, a NULL being none.
template <class T>
static bool HoldsNan(Vector &values, idx_t count) {
	UnifiedVectorFormat format;
	values.ToUnifiedFormat(count, format);
	auto data = UnifiedVectorFormat::GetData<T>(format);
	for (idx_t i = 0; i < count; i++) {
		const auto index = format.sel->get_index(i);
		if (format.validity.RowIsValid(index) && std::isnan(data[index])) {
			return true;
		}
	}
	return false;
}

//! An aggregate run through its state functions over values of `sources`, one vector per argument, so the value has
//! SQL's type and arithmetic, and a NULL is skipped as SQL's aggregate skips it. A NaN in what it reads (`read`,
//! indexed as `sources`) is an error, except to COUNT, which counts it (language/guide.md §3, as a reducer's
//! ArgumentMessage): checked on the values, since SQL's MIN passes over one. `written` names the reducer in that error.
class AggregateFold {
public:
	AggregateFold(ClientContext &context, const BoundAggregateExpression &aggregate, vector<Vector> &sources_p,
	              Vector &read_p, idx_t count, const string &written_p)
	    : allocator(BufferAllocator::Get(context)), input(aggregate.bind_info.get(), allocator), sources(sources_p),
	      read(read_p), states(aggregate.function, input, count), selection(STANDARD_VECTOR_SIZE),
	      targets(LogicalType::POINTER, STANDARD_VECTOR_SIZE), written(written_p),
	      refuses_nan(StringUtil::Lower(aggregate.function.name) != "count") {
	}

	//! State `state` reads the values at `index`, `times` times.
	void Feed(idx_t index, idx_t state, idx_t times = 1) {
		auto target_states = FlatVector::GetData<data_ptr_t>(targets);
		for (idx_t i = 0; i < times; i++) {
			if (pending == STANDARD_VECTOR_SIZE) {
				Flush();
			}
			selection.set_index(pending, index);
			target_states[pending++] = states.At(state);
		}
	}

	//! Each state's value, in `values`.
	void Finish(Vector &values) {
		Flush();
		states.function.finalize(states.pointers, input, values, states.count, 0);
	}

private:
	void Flush() {
		if (pending == 0) {
			return;
		}
		vector<Vector> slices;
		slices.reserve(sources.size());
		for (auto &source : sources) {
			slices.emplace_back(source, selection, pending);
		}
		Vector values(read, selection, pending);
		if (refuses_nan && ReadsNan(values)) {
			throw InvalidInputException(DecideFrameNanMessage(written));
		}
		states.function.update(slices.data(), input, slices.size(), targets, pending);
		pending = 0;
	}

	bool ReadsNan(Vector &values) const {
		switch (values.GetType().InternalType()) {
		case PhysicalType::DOUBLE:
			return HoldsNan<double>(values, pending);
		case PhysicalType::FLOAT:
			return HoldsNan<float>(values, pending);
		default:
			return false;
		}
	}

	ArenaAllocator allocator;
	AggregateInputData input;
	vector<Vector> &sources;
	Vector &read;
	FrameAggregateStates states;
	SelectionVector selection;
	Vector targets;
	const string &written;
	const bool refuses_nan;
	idx_t pending = 0;
};

//! Every row of `out` NULL, in each field of a nested type too: the parent's mask alone leaves a STRUCT's fields
//! holding whatever the allocator did, which a copy of the row reads.
static void SetEveryRowNull(Vector &out, idx_t num_rows) {
	for (idx_t row = 0; row < num_rows; row++) {
		FlatVector::SetNull(out, row, true);
	}
}

//! Fold states together on the usual path. If a data error affects one state, recover each state's result and
//! error separately: an anchor that never reads the failing state must still be able to read the others.
template <class FEED>
static Vector FoldFrameStates(ClientContext &context, const BoundAggregateExpression &aggregate,
                              vector<Vector> &sources, Vector &read, idx_t count, const string &written,
                              const FEED &feed, std::map<idx_t, ErrorData> &errors) {
	Vector values(aggregate.return_type, count);
	try {
		AggregateFold fold(context, aggregate, sources, read, count, written);
		for (idx_t state = 0; state < count; state++) {
			if (!errors.count(state)) {
				feed(state, fold, state);
			}
		}
		fold.Finish(values);
		return values;
	} catch (std::exception &ex) {
		if (StopsFrameEvaluation(ErrorData(ex))) {
			throw;
		}
	}
	for (idx_t state = 0; state < count; state++) {
		if (errors.count(state)) {
			FlatVector::SetNull(values, state, true);
			continue;
		}
		try {
			AggregateFold fold(context, aggregate, sources, read, 1, written);
			feed(state, fold, 0);
			Vector one(aggregate.return_type);
			fold.Finish(one);
			values.SetValue(state, one.GetValue(0));
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (StopsFrameEvaluation(error)) {
				throw;
			}
			errors.emplace(state, std::move(error));
			FlatVector::SetNull(values, state, true);
		}
	}
	return values;
}

//! A range over data's REDUCED column (DecideFrameColumn) into `out`: the frame's own reducer run (AggregateFold) over
//! each window's values -- the payload at each row it reads, once per visit, and the `else` at its anchor once per
//! missing position; a NULL among them is skipped (D84; `count` counts the others, and of none is 0). Each value is
//! computed on the rows that read it only. An anchor whose window reads nothing reads NULL.
static void ReduceRangeValues(ClientContext &context, const DecideFrameSpec &frame, const DecideFrameColumn &column,
                              const DecideFrameMap &map, idx_t num_rows, const DecideFrameRead &read, Vector &out,
                              std::map<idx_t, ErrorData> &errors) {
	auto &marker = column.value->Cast<BoundAggregateExpression>();
	if (marker.children.empty()) {
		throw InternalException("DECIDE: %s's column reads no reducer over its payload", frame.text);
	}
	const idx_t windows = map.WindowCount();
	if (windows == 0) {
		SetEveryRowNull(out, num_rows); // no anchor: nothing to read
		return;
	}
	if (map.fallback != (column.fallback != nullptr)) {
		throw InternalException("DECIDE: %s's column and its map disagree on its else", frame.text);
	}
	// One source per argument to reduce from: the payload's values at [0, n), the else's at [n, 2n). Only a collated
	// MIN or MAX has a second argument, and it takes no `else` (BindDecideFrame).
	vector<Vector> sources;
	std::map<idx_t, ErrorData> source_errors;
	auto read_rows = map.ReadRows();
	for (auto &argument : marker.children) {
		sources.emplace_back(argument->return_type, 2 * num_rows);
		auto payload = read(*argument, read_rows);
		VectorOperations::Copy(payload.values, sources.back(), num_rows, 0, 0);
		for (auto &error : payload.errors) {
			source_errors.emplace(error.first, error.second);
		}
	}
	if (map.fallback) {
		auto fallback = read(*column.fallback, map.FallbackAnchors());
		VectorOperations::Copy(fallback.values, sources[0], num_rows, 0, num_rows);
		for (auto &error : fallback.errors) {
			source_errors.emplace(num_rows + error.first, error.second);
		}
	}

	std::map<idx_t, ErrorData> window_errors;
	for (idx_t w = 0; w < windows; w++) {
		if (map.WindowAbsent(w)) {
			continue;
		}
		auto inherit = [&](idx_t source) {
			auto error = source_errors.find(source);
			if (error != source_errors.end()) {
				window_errors.emplace(w, error->second);
			}
		};
		map.ForEachRead(w, [&](idx_t row, idx_t) { inherit(row); });
		if (map.WindowFallbacks(w) > 0) {
			inherit(num_rows + map.window_anchor[w]);
		}
	}
	auto feed = [&](idx_t w, AggregateFold &fold, idx_t state) {
		if (map.WindowAbsent(w)) {
			return;
		}
		map.ForEachRead(w, [&](idx_t row, idx_t times) { fold.Feed(row, state, times); });
		const auto fallbacks = map.WindowFallbacks(w);
		if (fallbacks > 0) {
			fold.Feed(num_rows + map.window_anchor[w], state, fallbacks);
		}
	};
	auto values = FoldFrameStates(context, marker, sources, sources[0], windows, frame.text, feed, window_errors);
	// Each anchor reads its window's value; every other row, and an anchor whose window reads nothing, reads NULL.
	SelectionVector gather(num_rows);
	vector<bool> unread(num_rows, false);
	for (idx_t row = 0; row < num_rows; row++) {
		const auto window = map.anchors[row] ? map.window[row] : DConstants::INVALID_INDEX;
		unread[row] = window == DConstants::INVALID_INDEX || map.Absent(row);
		gather.set_index(row, unread[row] ? 0 : window);
		if (!unread[row]) {
			auto error = window_errors.find(window);
			if (error != window_errors.end()) {
				errors.emplace(row, error->second);
			}
		}
	}
	VectorOperations::Copy(values, out, gather, num_rows, 0, 0);
	for (idx_t row = 0; row < num_rows; row++) {
		if (unread[row]) {
			FlatVector::SetNull(out, row, true);
		}
	}
}

void SkipDecideRangeNullReads(const vector<const Expression *> &payload, const vector<const Expression *> &fallback,
                              DecideFrameMap &map, idx_t num_rows, const DecideFrameRead &read,
                              const vector<bool> &absent_payload) {
	auto null_on = [&](const vector<const Expression *> &exprs, const vector<bool> &rows) {
		vector<bool> nulls(num_rows, false);
		bool any = false;
		for (auto *expr : exprs) {
			auto values = read(*expr, rows);
			UnifiedVectorFormat format;
			values.values.ToUnifiedFormat(num_rows, format);
			for (idx_t row = 0; row < num_rows; row++) {
				// A failed value is not a NULL member: its error must reach the window's actual consumer.
				if (rows[row] && !values.errors.count(row) && !format.validity.RowIsValid(format.sel->get_index(row))) {
					nulls[row] = any = true;
				}
			}
		}
		return any ? nulls : vector<bool>();
	};
	auto payload_nulls = null_on(payload, map.ReadRows());
	if (!absent_payload.empty()) {
		const auto reads = map.ReadRows();
		for (idx_t row = 0; row < num_rows; row++) {
			if (reads[row] && absent_payload[row]) {
				payload_nulls.resize(num_rows, false);
				payload_nulls[row] = true;
			}
		}
	}
	auto fallback_nulls = null_on(fallback, map.FallbackAnchors());
	if (!payload_nulls.empty() || !fallback_nulls.empty()) {
		map.SkipNullReads(std::move(payload_nulls), std::move(fallback_nulls));
	}
}

vector<bool> DecideReadPeers(const DecideFrameMap &map) {
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
		if (!map.WindowVisits(w)) {
			continue;
		}
		const auto list = map.window_list[w];
		for (idx_t i = map.list_begin[list]; i < map.list_begin[list + 1]; i++) {
			read[map.peer[map.list_rows[i]]] = true;
		}
	}
	return read;
}

DecideFrameColumnValues ComputeDecideFrameColumns(ClientContext &context, const vector<DecideFrameSpec> &frames,
                                                  const vector<DecideFrameMap> &maps, const DecideFrameReads &reads,
                                                  idx_t num_rows, const DecideFrameRead &read) {
	auto &columns = reads.columns;
	DecideFrameColumnValues computed;
	auto &values_of = computed.values;
	auto &errors_of = computed.errors;
	// DEFINED reads the frame's other columns, so it is computed after them. A reducer over data reading frames is
	// computed once they are appended, its column after every frame's.
	vector<idx_t> defined;
	for (idx_t j = 0; j < columns.size(); j++) {
		auto &column = columns[j];
		if (column.kind == DecideFrameColumn::Kind::AGGREGATE) {
			break;
		}
		auto &map = maps[column.frame];
		auto out = make_uniq<Vector>(column.type, num_rows);
		std::map<idx_t, ErrorData> errors;
		if (column.kind == DecideFrameColumn::Kind::DEFINED) {
			defined.push_back(j);
		} else if (column.kind == DecideFrameColumn::Kind::REDUCED) {
			ReduceRangeValues(context, frames[column.frame], column, map, num_rows, read, *out, errors);
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
			std::map<idx_t, ErrorData> source_errors;
			if (payload) {
				auto values = read(*payload, selected_rows);
				VectorOperations::Copy(values.values, source, num_rows, 0, 0);
				source_errors = std::move(values.errors);
			}
			const bool no_value = column.kind == DecideFrameColumn::Kind::VALUE ||
			                      (column.kind == DecideFrameColumn::Kind::READ && !frames[column.frame].fallback);
			source.SetValue(num_rows, no_value ? Value(column.type) : Value::INTEGER(0).DefaultCastAs(column.type));
			if (fallback) {
				auto values = read(*fallback, missing_rows);
				VectorOperations::Copy(values.values, source, num_rows, 0, num_rows + 1);
				for (auto &error : values.errors) {
					source_errors.emplace(num_rows + 1 + error.first, error.second);
				}
			}
			SelectionVector gather(num_rows);
			for (idx_t row = 0; row < num_rows; row++) {
				auto selected_row = payload ? map.SelectedRow(row) : DConstants::INVALID_INDEX;
				auto source_row = selected_row != DConstants::INVALID_INDEX ? selected_row
				                  : missing_rows[row]                       ? num_rows + 1 + row
				                                                            : num_rows;
				gather.set_index(row, source_row);
				auto error = source_errors.find(source_row);
				if (!map.Absent(row) && error != source_errors.end()) {
					errors.emplace(row, error->second);
				}
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
		values_of.push_back(std::move(out));
		errors_of.push_back(std::move(errors));
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
		for (idx_t k = 0; k < values_of.size() && !map.range; k++) {
			auto kind = columns[k].kind;
			if (columns[k].frame != columns[j].frame ||
			    (kind != DecideFrameColumn::Kind::READ && kind != DecideFrameColumn::Kind::FALLBACK)) {
				continue;
			}
			for (idx_t row = 0; row < num_rows; row++) {
				auto error = errors_of[k].find(row);
				if (error != errors_of[k].end()) {
					errors_of[j].emplace(row, error->second);
					continue;
				}
				if (values_of[k]->GetValue(row).IsNull()) {
					validity.SetInvalid(row);
				}
			}
		}
	}
	return computed;
}

//! The columns `expr` reads, each once, appended to `leaves` unless there: a position's signature reads each by value
//! (deck p. 110; D83). A constant reads none, and a clause refuses data that varies by row without a column.
static void AppendPayloadLeaves(const Expression &expr, vector<const Expression *> &leaves) {
	auto kind = expr.GetExpressionClass();
	if (kind == ExpressionClass::BOUND_COLUMN_REF || kind == ExpressionClass::BOUND_REF) {
		for (auto *leaf : leaves) {
			if (leaf->Equals(expr)) {
				return;
			}
		}
		leaves.push_back(&expr);
		return;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { AppendPayloadLeaves(child, leaves); });
}

void SkipDecideFrameNullReads(const DecideTree &tree, const vector<LogicalDecide::ReducerValueLink> &links,
                              vector<DecideFrameMap> &maps, idx_t num_rows, const DecideFrameRead &read,
                              const std::function<bool(idx_t variable, idx_t row)> &absent) {
	// A range over decisions is a helper (RewriteFrameRanges) whose members skip the values its windows read as NULL,
	// read before any window is numbered or listed: its payload's coefficients, and its `else`'s (FALLBACK columns).
	for (auto &link : links) {
		if (link.frame == DConstants::INVALID_INDEX) {
			continue;
		}
		vector<const Expression *> payload, fallback;
		vector<bool> absent_payload;
		for (auto &term : tree.nodes[tree.frame_nodes[link.frame]]->compiled.members) {
			payload.push_back(term.coefficient.get());
			for (idx_t row = 0; row < num_rows; row++) {
				if (absent(term.variable_index, row)) {
					absent_payload.resize(num_rows, false);
					absent_payload[row] = true;
				}
			}
		}
		for (auto &column : tree.frame_reads.columns) {
			if (column.frame == link.frame && column.kind == DecideFrameColumn::Kind::FALLBACK) {
				fallback.push_back(column.value.get());
			}
		}
		SkipDecideRangeNullReads(payload, fallback, maps[link.frame], num_rows, read, absent_payload);
	}
	// A range over data reads an `else` that is NULL at its anchor as no `else` there (D86, deck p. 50): a window
	// left with nothing to read has no value, `count`'s too. A NULL its payload reads is a value its aggregate skips.
	for (auto &column : tree.frame_reads.columns) {
		if (column.kind == DecideFrameColumn::Kind::REDUCED && column.fallback) {
			SkipDecideRangeNullReads({}, {column.fallback.get()}, maps[column.frame], num_rows, read);
		}
	}
}

KeyedMapping DecideFrameWindowMapping(const DecideFrameMap &map) {
	const idx_t num_rows = map.anchors.size();
	KeyedMapping mapping;
	mapping.num_classes = map.WindowCount();
	mapping.row_to_class.assign(num_rows, DConstants::INVALID_INDEX);
	for (idx_t row = 0; row < num_rows; row++) {
		if (map.anchors[row] && !map.Absent(row)) {
			mapping.row_to_class[row] = map.window[row];
		}
	}
	return mapping;
}

void DecideListFrameMembers(const DecideFrameMap &map, const LogicalDecide::ReducerValueLink &link,
                            const string &frame_text, const vector<CoefficientColumn> &payload,
                            const vector<idx_t> &payload_vars, const vector<CoefficientColumn> &fallback,
                            const vector<idx_t> &fallback_vars, ReducerValueMembers &out) {
	const bool extremum = link.agg_name == "min" || link.agg_name == "max";
	const bool is_avg = link.agg_name == "avg";
	for (idx_t w = 0; w < map.WindowCount(); w++) {
		if (map.WindowAbsent(w)) {
			continue;
		}
		const idx_t values = map.ValueCount(w);
		if (values == 0) {
			throw InternalException("DECIDE: %s's window reads nothing but is not absent", frame_text);
		}
		const double share = is_avg ? 1.0 / static_cast<double>(values) : 1.0;
		out.AddGroup(map.window_anchor[w]);
		map.ForEachRead(w, [&](idx_t row, idx_t times) {
			DecideListMember(payload, payload_vars, row, (extremum ? 1.0 : static_cast<double>(times)) * share,
			                 link.label, out);
		});
		const idx_t fallbacks = map.WindowFallbacks(w);
		if (fallbacks > 0) {
			DecideListMember(fallback, fallback_vars, map.window_anchor[w],
			                 (extremum ? 1.0 : static_cast<double>(fallbacks)) * share, link.label, out);
		}
	}
}

void DecideCountFrames(const vector<DecideFrameMap> &maps) {
	if (!DecideProfileScope::Enabled() || maps.empty()) {
		return;
	}
	idx_t windows = 0, positions = 0;
	for (auto &map : maps) {
		if (!map.range) {
			positions += std::count(map.anchors.begin(), map.anchors.end(), true);
			continue;
		}
		windows += map.WindowCount();
		for (idx_t w = 0; w < map.WindowCount(); w++) {
			positions += map.ValueCount(w);
		}
	}
	DecideProfileScope::Counter("anr.frames", static_cast<double>(maps.size()));
	DecideProfileScope::Counter("anr.frame_windows", static_cast<double>(windows));
	DecideProfileScope::Counter("anr.frame_positions", static_cast<double>(positions));
}

} // namespace duckdb
