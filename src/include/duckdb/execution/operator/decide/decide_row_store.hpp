//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_row_store.hpp
//
// The one-pass evaluation (deck §3, pp. 77-119; pipeline/08_execution/done.md).
//===----------------------------------------------------------------------===//
//
// The deck evaluates a query in two passes over its annotated tree: a top-down construction reads the rows once and
// fills each node's container with what the node reads on a row, and a bottom-up evaluation computes each node from
// its children's containers. Here every node of the plan -- a scoped expression's `when`, key and signature fields, a
// reducer's key, filter and operand, a frame's timeline, order and payload -- registers the expressions it reads on a
// row (PhysicalDecide::RegisterEvaluation), one scan of the operator's input evaluates every registration on every row
// (DecideRowStore::Build), and every loop of the evaluation after it reads its values from here (DecideRowWindows): no
// loop scans the input again, and a read the plan did not register is an internal error.
//
// The pass evaluates an expression on rows a node may not read, because which rows a node reads is decided later
// (a `when` the same pass computes, a frame's presence the bottom-up evaluation finds). So an error an expression
// raises on a row is kept, and raised only when a read reaches that row, as evaluating it there would have: no error
// of a row a clause drops is seen (language/guide.md §6.3).
//
// An expression over a column the pass cannot know -- a frame's, computed bottom-up (AppendColumn) -- is the deck's
// bottom-up evaluation of a parent node: it is evaluated when first read, over the computed columns and the values the
// pass kept of the input columns it reads, which it registered.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"

#include <map>

namespace duckdb {

class DecideRowStore {
public:
	DecideRowStore(ClientContext &context, ColumnDataCollection &data);

	//! What a node reads on a row: `lowered` (CachedTransformToChunkExpression, or held as long as the store) is
	//! evaluated in the pass, or -- reading a computed column -- when first read, over the input columns it reads,
	//! which the pass keeps. Only before Build.
	void Register(const Expression &lowered);
	//! An evaluation over the row and values of its own reads `lowered`'s input columns (DecideRowWindows::Input):
	//! the pass keeps them. Only before Build.
	void KeepColumnsOf(const Expression &lowered);
	//! The pass: one scan of the input, evaluating every registered expression on every row.
	void Build();
	//! A column computed bottom-up (a frame's), numbered after the input's and the computed ones before it.
	void AppendColumn(const Vector &values);
	void AppendColumn(const Vector &values, const std::map<idx_t, ErrorData> &errors);
	//! The columns an expression may read: the input's, then the computed ones; and their types.
	idx_t ColumnCount() const {
		return types.size();
	}
	const vector<LogicalType> &Types() const {
		return types;
	}
	idx_t RowCount() const {
		return num_rows;
	}

	//! One expression's values on every row, and the rows its evaluation raised on, with the error: raised when a read
	//! reaches the row.
	struct Slot {
		const Expression *lowered;
		Vector values;
		std::map<idx_t, ErrorData> errors;
		//! A slot made after the pass holds its own copy: what a read names may not outlive it.
		unique_ptr<Expression> owned;
		//! A constant (Expression::IsFoldable) is evaluated once, not per row: `values` holds it, then a NULL, and an
		//! error it raises is every row's.
		bool constant = false;
		ErrorData constant_error;
	};
	//! The slot of `lowered`: the pass's, or -- over a computed column -- one evaluated now.
	const Slot &Values(const Expression &lowered);
	//! `lowered` on row `row`, as a message reads it.
	Value ValueAt(const Expression &lowered, idx_t row);
	//! Rows `offset` to `offset + count` of every column below ColumnCount the expressions of `executor` read, into
	//! `out` (over Types()), each other column NULL: what an evaluation over the row and values of its own reads.
	void SliceColumns(const ExpressionExecutor &executor, idx_t offset, idx_t count, DataChunk &out);

private:
	Slot *Find(const Expression &lowered);
	//! A slot for `lowered`, which outlives the store (a registration's), or a copy of it.
	Slot &AddSlot(const Expression &lowered, bool copy);
	//! Refuses `lowered` when the pass did not keep an input column it reads: a registration missing from the plan.
	void RequireKept(const Expression &lowered) const;
	//! `slot` on every row, over the kept and computed columns.
	void Fill(Slot &slot);

	ClientContext &context;
	ColumnDataCollection &data;
	const idx_t num_rows;
	const idx_t input_columns;
	vector<LogicalType> types;
	bool built = false;
	vector<unique_ptr<Slot>> slots;
	//! Slots by the address of the expression they hold, and by its hash, for an equal one at another address -- not
	//! remembered by that address, which a temporary may give up to another expression.
	std::unordered_map<const Expression *, idx_t> slot_at;
	std::unordered_multimap<hash_t, idx_t> slot_hash;
	//! Per input column, its values once the pass kept it (null otherwise); then the computed columns.
	vector<unique_ptr<Vector>> columns;
	//! Latent errors of computed columns, exposed only if an expression actually reads that column on that row.
	vector<std::map<idx_t, ErrorData>> column_errors;
	vector<bool> keep;
};

//! A loop of the evaluation over the input's rows, one window of rows at a time, reading the pass's store
//! (ChunkExprCache::store).
class DecideRowWindows {
public:
	explicit DecideRowWindows(ChunkExprCache &cache);

	//! Moves to the next window; false after the last.
	bool Next();
	//! The window's first row, and its rows.
	idx_t Offset() const {
		return offset;
	}
	idx_t Count() const {
		return count;
	}
	//! Each expression of `executor` on the window's rows `*reads[i]` marks (every row when null), NULL on the others:
	//! `reads` is indexed from row `mask_offset` of the window.
	void Execute(ExpressionExecutor &executor, idx_t mask_offset, const vector<const vector<bool> *> &reads,
	             DataChunk &results);
	//! The same with one set of rows, indexed by the input's rows, for every expression.
	void Execute(ExpressionExecutor &executor, const vector<bool> *rows, DataChunk &results);
	//! `lowered` on the window's row `i`, as a message reads it.
	Value ValueAt(const Expression &lowered, idx_t i);
	//! The window's rows of every column the expressions of `executor` read, over the store's columns: for an
	//! evaluation over the row and values of its own, numbered after them.
	DataChunk &Input(const ExpressionExecutor &executor);

private:
	DecideRowStore &store;
	//! The slot each expression of the loop reads, once found.
	std::unordered_map<const Expression *, const DecideRowStore::Slot *> resolved;
	DataChunk columns;
	bool started = false;
	idx_t offset = 0;
	idx_t count = 0;
};

} // namespace duckdb
