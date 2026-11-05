//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/execution/operator/decide/decide_row_store.cpp
//
// The one-pass evaluation's store and the windows its loops read (decide_row_store.hpp).
//===----------------------------------------------------------------------===//

#include "duckdb/execution/operator/decide/decide_row_store.hpp"

#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/operator/decide/decide_row_evaluation.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//! Calls `read` with each column `lowered` reads.
static void ForEachColumnRead(const Expression &lowered, const std::function<void(idx_t)> &read) {
	if (lowered.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		read(lowered.Cast<BoundReferenceExpression>().index);
		return;
	}
	ExpressionIterator::EnumerateChildren(lowered, [&](const Expression &child) { ForEachColumnRead(child, read); });
}

//! Whether `lowered` reads a column numbered `first` or after.
static bool ReadsColumnFrom(const Expression &lowered, idx_t first) {
	bool reads = false;
	ForEachColumnRead(lowered, [&](idx_t column) { reads = reads || column >= first; });
	return reads;
}

//! Whether an error stops the query wherever it is raised, rather than belonging to the row it is raised on.
static bool StopsTheQuery(const ErrorData &error) {
	auto type = error.Type();
	return type == ExceptionType::INTERRUPT || type == ExceptionType::OUT_OF_MEMORY || type == ExceptionType::FATAL;
}

//! Expression `expr_idx` of `*executor` on `chunk`, whose first row is row `offset` of the input, into `slot`. When a
//! row raises, each row is evaluated alone, a failing one keeping its error and NULL; the executor, whose states the
//! failure may have left half written, is made again by `remake`.
static void EvaluateWindow(ClientContext &context, unique_ptr<ExpressionExecutor> &executor, idx_t expr_idx,
                           const std::function<unique_ptr<ExpressionExecutor>()> &remake, DecideRowStore::Slot &slot,
                           DataChunk &chunk, idx_t offset) {
	const idx_t count = chunk.size();
	const auto &type = slot.lowered->return_type;
	Vector out(type);
	try {
		executor->SetChunk(chunk);
		executor->ExecuteExpression(expr_idx, out);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (StopsTheQuery(error)) {
			throw;
		}
		executor = remake();
		Vector rows(type);
		auto row_executor = make_uniq<ExpressionExecutor>(context, *slot.lowered);
		for (idx_t i = 0; i < count; i++) {
			SelectionVector at_row(1);
			at_row.set_index(0, i);
			DataChunk row;
			row.InitializeEmpty(chunk.GetTypes());
			row.Slice(chunk, at_row, 1);
			Vector value(type);
			try {
				row_executor->ExecuteExpression(row, value);
				rows.SetValue(i, value.GetValue(0));
			} catch (std::exception &row_ex) {
				ErrorData row_error(row_ex);
				if (StopsTheQuery(row_error)) {
					throw;
				}
				slot.errors.emplace(offset + i, std::move(row_error));
				rows.SetValue(i, Value(type));
				row_executor = make_uniq<ExpressionExecutor>(context, *slot.lowered);
			}
		}
		out.Reference(rows);
	}
	VectorOperations::Copy(out, slot.values, count, 0, offset);
}

DecideRowStore::DecideRowStore(ClientContext &context, ColumnDataCollection &data)
    : context(context), data(data), num_rows(data.Count()), input_columns(data.ColumnCount()), types(data.Types()),
      columns(data.ColumnCount()), keep(data.ColumnCount(), false) {
}

DecideRowStore::Slot *DecideRowStore::Find(const Expression &lowered) {
	auto at = slot_at.find(&lowered);
	if (at != slot_at.end()) {
		return slots[at->second].get();
	}
	auto equal = slot_hash.equal_range(lowered.Hash());
	for (auto it = equal.first; it != equal.second; ++it) {
		if (slots[it->second]->lowered->Equals(lowered)) {
			return slots[it->second].get();
		}
	}
	return nullptr;
}

DecideRowStore::Slot &DecideRowStore::AddSlot(const Expression &lowered, bool copy) {
	const idx_t index = slots.size();
	auto owned = copy ? lowered.Copy() : nullptr;
	auto &held = owned ? *owned : lowered;
	slots.push_back(make_uniq<Slot>(Slot {&held, Vector(held.return_type, MaxValue<idx_t>(num_rows, 1)), {}, nullptr}));
	slots.back()->owned = std::move(owned);
	slot_at.emplace(&held, index);
	slot_hash.emplace(held.Hash(), index);
	return *slots.back();
}

bool DecideRowStore::Kept(const Expression &lowered) const {
	bool kept = true;
	ForEachColumnRead(lowered, [&](idx_t column) { kept = kept && (column >= input_columns || columns[column]); });
	return kept;
}

void DecideRowStore::KeepColumnsOf(const Expression &lowered) {
	if (built) {
		throw InternalException("DECIDE: %s's columns are kept after the pass", lowered.ToString());
	}
	ForEachColumnRead(lowered, [&](idx_t column) {
		if (column < input_columns) {
			keep[column] = true;
		}
	});
}

void DecideRowStore::Register(const Expression &lowered) {
	if (built) {
		throw InternalException("DECIDE: %s is registered after the pass", lowered.ToString());
	}
	if (ReadsColumnFrom(lowered, input_columns)) {
		KeepColumnsOf(lowered);
		return;
	}
	if (!Find(lowered)) {
		AddSlot(lowered, false);
	}
}

void DecideRowStore::Build() {
	for (idx_t column = 0; column < input_columns; column++) {
		if (keep[column]) {
			columns[column] = make_uniq<Vector>(types[column], MaxValue<idx_t>(num_rows, 1));
		}
	}
	auto remake = [&]() {
		auto executor = make_uniq<ExpressionExecutor>(context);
		for (auto &slot : slots) {
			executor->AddExpression(*slot->lowered);
		}
		return executor;
	};
	auto executor = remake();
	ColumnDataScanState scan;
	InitializeDecideScan(data, scan);
	DataChunk chunk;
	chunk.Initialize(context, data.Types());
	idx_t offset = 0;
	while (data.Scan(scan, chunk)) {
		for (idx_t column = 0; column < input_columns; column++) {
			if (columns[column]) {
				VectorOperations::Copy(chunk.data[column], *columns[column], chunk.size(), 0, offset);
			}
		}
		for (idx_t i = 0; i < slots.size(); i++) {
			EvaluateWindow(context, executor, i, remake, *slots[i], chunk, offset);
		}
		offset += chunk.size();
	}
	built = true;
}

void DecideRowStore::AppendColumn(const Vector &values) {
	auto column = make_uniq<Vector>(values.GetType(), MaxValue<idx_t>(num_rows, 1));
	VectorOperations::Copy(values, *column, num_rows, 0, 0);
	types.push_back(values.GetType());
	columns.push_back(std::move(column));
}

void DecideRowStore::Fill(Slot &slot) {
	ForEachColumnRead(*slot.lowered, [&](idx_t column) {
		if (column >= types.size()) {
			throw InternalException("DECIDE: %s reads column %llu before it is computed", slot.lowered->ToString(),
			                        column);
		}
	});
	// An input column the pass did not keep is read again from the input.
	const bool scan_input = !Kept(*slot.lowered);
	ColumnDataScanState scan;
	DataChunk input;
	if (scan_input) {
		InitializeDecideScan(data, scan);
		input.Initialize(context, data.Types());
	}
	auto remake = [&]() {
		return make_uniq<ExpressionExecutor>(context, *slot.lowered);
	};
	auto executor = remake();
	DataChunk chunk;
	chunk.InitializeEmpty(types);
	for (idx_t offset = 0; offset < num_rows; offset += chunk.size()) {
		idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, num_rows - offset);
		if (scan_input) {
			if (!data.Scan(scan, input)) {
				throw InternalException("DECIDE: the input ends before row %llu", offset);
			}
			count = input.size();
		}
		for (idx_t column = 0; column < types.size(); column++) {
			if (column < input_columns && scan_input) {
				chunk.data[column].Reference(input.data[column]);
			} else if (columns[column]) {
				chunk.data[column].Slice(*columns[column], offset, offset + count);
			} else {
				chunk.data[column].Reference(Value(types[column]));
			}
		}
		chunk.SetCardinality(count);
		EvaluateWindow(context, executor, 0, remake, slot, chunk, offset);
	}
}

const DecideRowStore::Slot &DecideRowStore::Values(const Expression &lowered) {
	if (auto *slot = Find(lowered)) {
		return *slot;
	}
	if (!built) {
		throw InternalException("DECIDE: %s is read before the pass", lowered.ToString());
	}
	if (!ReadsColumnFrom(lowered, input_columns)) {
		late_reads++;
	}
	auto &slot = AddSlot(lowered, true);
	Fill(slot);
	return slot;
}

void DecideRowStore::KeepLate(const vector<const Expression *> &lowered) {
	vector<idx_t> missing;
	for (auto *expr : lowered) {
		ForEachColumnRead(*expr, [&](idx_t column) {
			if (column < input_columns && !columns[column] &&
			    std::find(missing.begin(), missing.end(), column) == missing.end()) {
				missing.push_back(column);
			}
		});
	}
	if (missing.empty()) {
		return;
	}
	late_reads++;
	for (auto column : missing) {
		keep[column] = true;
		columns[column] = make_uniq<Vector>(types[column], MaxValue<idx_t>(num_rows, 1));
	}
	ColumnDataScanState scan;
	InitializeDecideScan(data, scan);
	DataChunk chunk;
	chunk.Initialize(context, data.Types());
	idx_t offset = 0;
	while (data.Scan(scan, chunk)) {
		for (auto column : missing) {
			VectorOperations::Copy(chunk.data[column], *columns[column], chunk.size(), 0, offset);
		}
		offset += chunk.size();
	}
}

void DecideRowStore::SliceColumns(const ExpressionExecutor &executor, idx_t offset, idx_t count, DataChunk &out) {
	KeepLate(executor.expressions);
	for (idx_t column = 0; column < types.size(); column++) {
		if (columns[column]) {
			out.data[column].Slice(*columns[column], offset, offset + count);
		} else {
			out.data[column].Reference(Value(types[column]));
		}
	}
	out.SetCardinality(count);
}

Value DecideRowStore::ValueAt(const Expression &lowered, idx_t row) {
	if (auto *slot = Find(lowered)) {
		auto error = slot->errors.find(row);
		if (error != slot->errors.end()) {
			error->second.Throw();
		}
		return slot->values.GetValue(row);
	}
	// A read of one row: the input's row, when the pass kept no column it reads, is looked up, not scanned.
	ColumnDataScanState seek;
	DataChunk input;
	idx_t input_row = 0;
	if (!Kept(lowered)) {
		data.InitializeScan(seek);
		input.Initialize(context, data.Types());
		if (!data.Seek(row, seek, input)) {
			throw InternalException("DECIDE: row %llu is past the end of the input", row);
		}
		input_row = row - seek.current_row_index;
	}
	DataChunk one;
	one.InitializeEmpty(types);
	for (idx_t column = 0; column < types.size(); column++) {
		if (columns[column]) {
			one.data[column].Slice(*columns[column], row, row + 1);
		} else if (column < input_columns && input.ColumnCount() > 0) {
			one.data[column].Slice(input.data[column], input_row, input_row + 1);
		} else {
			one.data[column].Reference(Value(types[column]));
		}
	}
	one.SetCardinality(1);
	ExpressionExecutor executor(context, lowered);
	Vector value(lowered.return_type);
	executor.ExecuteExpression(one, value);
	return value.GetValue(0);
}

DecideRowWindows::DecideRowWindows(ChunkExprCache &cache, ClientContext &context, ColumnDataCollection &data)
    : store(cache.store), context(context), data(data) {
}

bool DecideRowWindows::Next() {
	if (!started) {
		started = true;
		if (!store) {
			InitializeDecideScan(data, scan);
			chunk.Initialize(context, data.Types());
		}
	} else {
		offset += count;
	}
	if (!store) {
		if (!data.Scan(scan, chunk)) {
			count = 0;
			return false;
		}
		count = chunk.size();
		return true;
	}
	const idx_t num_rows = data.Count();
	count = offset < num_rows ? MinValue<idx_t>(STANDARD_VECTOR_SIZE, num_rows - offset) : 0;
	return count > 0;
}

void DecideRowWindows::Execute(ExpressionExecutor &executor, idx_t mask_offset,
                               const vector<const vector<bool> *> &reads, DataChunk &results) {
	if (!store) {
		ExecuteOnReadRows(executor, chunk, mask_offset, reads, results);
		return;
	}
	results.SetCardinality(count);
	for (idx_t e = 0; e < executor.expressions.size(); e++) {
		auto &slot = store->Values(*executor.expressions[e]);
		auto *rows = reads[e];
		auto read = [&](idx_t i) {
			return !rows || (*rows)[mask_offset + i];
		};
		// The first error of a read row, as evaluating the window's read rows would raise it.
		for (auto error = slot.errors.lower_bound(offset); error != slot.errors.end() && error->first < offset + count;
		     ++error) {
			if (read(error->first - offset)) {
				error->second.Throw();
			}
		}
		Vector window(slot.values.GetType());
		VectorOperations::Copy(slot.values, window, offset + count, offset, 0);
		for (idx_t i = 0; rows && i < count; i++) {
			if (!read(i)) {
				FlatVector::SetNull(window, i, true);
			}
		}
		results.data[e].Reference(window);
	}
}

void DecideRowWindows::Execute(ExpressionExecutor &executor, const vector<bool> *rows, DataChunk &results) {
	Execute(executor, offset, vector<const vector<bool> *>(executor.expressions.size(), rows), results);
}

Value DecideRowWindows::ValueAt(const Expression &lowered, idx_t i) {
	if (store) {
		return store->ValueAt(lowered, offset + i);
	}
	SelectionVector at_row(1);
	at_row.set_index(0, i);
	DataChunk row;
	row.InitializeEmpty(chunk.GetTypes());
	row.Slice(chunk, at_row, 1);
	ExpressionExecutor executor(context, lowered);
	Vector value(lowered.return_type);
	executor.ExecuteExpression(row, value);
	return value.GetValue(0);
}

DataChunk &DecideRowWindows::Input(const ExpressionExecutor &executor) {
	if (!store) {
		return chunk;
	}
	if (columns.ColumnCount() != store->Types().size()) {
		columns.Destroy();
		columns.InitializeEmpty(store->Types());
	}
	store->SliceColumns(executor, offset, count, columns);
	return columns;
}

idx_t DecideColumnCount(const ChunkExprCache &cache, const ColumnDataCollection &data) {
	return cache.store ? cache.store->ColumnCount() : data.ColumnCount();
}

const vector<LogicalType> &DecideColumnTypes(const ChunkExprCache &cache, const ColumnDataCollection &data) {
	return cache.store ? cache.store->Types() : data.Types();
}

} // namespace duckdb
