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
	// One value per row, then a NULL a window points its unread rows at (DecideRowWindows::Execute); a constant's one
	// value, then the NULL.
	const bool constant = held.IsFoldable();
	const idx_t values = constant ? 1 : num_rows;
	slots.push_back(make_uniq<Slot>(Slot {&held, Vector(held.return_type, values + 1), {}, nullptr}));
	auto &slot = *slots.back();
	FlatVector::SetNull(slot.values, values, true);
	slot.owned = std::move(owned);
	slot.constant = constant;
	if (constant) {
		try {
			slot.values.SetValue(0, ExpressionExecutor::EvaluateScalar(context, held));
		} catch (std::exception &ex) {
			slot.constant_error = ErrorData(ex);
			if (StopsTheQuery(slot.constant_error)) {
				throw;
			}
			FlatVector::SetNull(slot.values, 0, true);
		}
	}
	slot_at.emplace(&held, index);
	slot_hash.emplace(held.Hash(), index);
	return slot;
}

void DecideRowStore::RequireKept(const Expression &lowered) const {
	ForEachColumnRead(lowered, [&](idx_t column) {
		if (column >= types.size() || (column < input_columns && !columns[column])) {
			throw InternalException("DECIDE: %s reads column %llu, which the one pass did not keep", lowered.ToString(),
			                        column);
		}
	});
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
	// The pass evaluates every expression that varies by row; a constant is evaluated already.
	vector<Slot *> varying;
	for (auto &slot : slots) {
		if (!slot->constant) {
			varying.push_back(slot.get());
		}
	}
	auto remake = [&]() {
		auto executor = make_uniq<ExpressionExecutor>(context);
		for (auto *slot : varying) {
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
		for (idx_t i = 0; i < varying.size(); i++) {
			EvaluateWindow(context, executor, i, remake, *varying[i], chunk, offset);
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
	RequireKept(*slot.lowered);
	auto remake = [&]() {
		return make_uniq<ExpressionExecutor>(context, *slot.lowered);
	};
	auto executor = remake();
	DataChunk chunk;
	chunk.InitializeEmpty(types);
	for (idx_t offset = 0; offset < num_rows; offset += chunk.size()) {
		const idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, num_rows - offset);
		for (idx_t column = 0; column < types.size(); column++) {
			if (columns[column]) {
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
	if (!built || !ReadsColumnFrom(lowered, input_columns)) {
		throw InternalException("DECIDE: %s is read, but no node of the one pass registered it", lowered.ToString());
	}
	auto &slot = AddSlot(lowered, true);
	Fill(slot);
	return slot;
}

void DecideRowStore::SliceColumns(const ExpressionExecutor &executor, idx_t offset, idx_t count, DataChunk &out) {
	for (auto *expr : executor.expressions) {
		ForEachColumnRead(*expr, [&](idx_t column) {
			if (column < input_columns && !columns[column]) {
				throw InternalException("DECIDE: %s reads column %llu, which the one pass did not keep",
				                        expr->ToString(), column);
			}
		});
	}
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
	auto &slot = Values(lowered);
	if (slot.constant) {
		if (slot.constant_error.HasError()) {
			slot.constant_error.Throw();
		}
		return slot.values.GetValue(0);
	}
	auto error = slot.errors.find(row);
	if (error != slot.errors.end()) {
		error->second.Throw();
	}
	return slot.values.GetValue(row);
}

static DecideRowStore &PassOf(ChunkExprCache &cache) {
	if (!cache.store) {
		throw InternalException("DECIDE: the evaluation reads its rows before the one pass");
	}
	return *cache.store;
}

DecideRowWindows::DecideRowWindows(ChunkExprCache &cache) : store(PassOf(cache)) {
}

bool DecideRowWindows::Next() {
	if (started) {
		offset += count;
	}
	started = true;
	const idx_t num_rows = store.RowCount();
	count = offset < num_rows ? MinValue<idx_t>(STANDARD_VECTOR_SIZE, num_rows - offset) : 0;
	return count > 0;
}

void DecideRowWindows::Execute(ExpressionExecutor &executor, idx_t mask_offset,
                               const vector<const vector<bool> *> &reads, DataChunk &results) {
	results.SetCardinality(count);
	for (idx_t e = 0; e < executor.expressions.size(); e++) {
		auto *expr = executor.expressions[e];
		auto found = resolved.find(expr);
		if (found == resolved.end()) {
			found = resolved.emplace(expr, &store.Values(*expr)).first;
		}
		auto &slot = *found->second;
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
		// The window reads the slot in place: its rows -- a constant's one value -- or, where some are not read,
		// those it reads and the NULL after the values on the others.
		const idx_t first = slot.constant ? 0 : offset;
		const idx_t null = slot.constant ? 1 : store.RowCount();
		idx_t unread = 0;
		SelectionVector window(count);
		for (idx_t i = 0; i < count; i++) {
			const bool row_read = read(i);
			unread += !row_read;
			window.set_index(i, row_read ? first + (slot.constant ? 0 : i) : null);
		}
		if (slot.constant && slot.constant_error.HasError() && unread < count) {
			slot.constant_error.Throw();
		}
		if (unread == 0 && slot.constant) {
			results.data[e].Reference(slot.values.GetValue(0));
		} else if (unread == 0) {
			results.data[e].Slice(slot.values, offset, offset + count);
		} else {
			results.data[e].Slice(slot.values, window, count);
		}
	}
}

void DecideRowWindows::Execute(ExpressionExecutor &executor, const vector<bool> *rows, DataChunk &results) {
	Execute(executor, offset, vector<const vector<bool> *>(executor.expressions.size(), rows), results);
}

Value DecideRowWindows::ValueAt(const Expression &lowered, idx_t i) {
	return store.ValueAt(lowered, offset + i);
}

DataChunk &DecideRowWindows::Input(const ExpressionExecutor &executor) {
	if (columns.ColumnCount() != store.Types().size()) {
		columns.Destroy();
		columns.InitializeEmpty(store.Types());
	}
	store.SliceColumns(executor, offset, count, columns);
	return columns;
}

} // namespace duckdb
