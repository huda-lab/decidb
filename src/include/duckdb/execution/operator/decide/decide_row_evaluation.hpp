//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_row_evaluation.hpp
//
// How PhysicalDecide's translation units read the materialized input.
//===----------------------------------------------------------------------===//
//
// Every clause reads the rows its scope keeps and nothing else (language/guide.md §6.1),
// so each evaluation below takes the rows to read and computes nothing on the others.
// Defined in physical_decide.cpp, which reads the input for every clause; declared here so
// the frame selectors' timelines (physical_decide_frames.cpp) read it the same way.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"

#include <functional>

namespace duckdb {

//! Starts a scan of the operator's materialized input, counted: the evaluation reads it once (deck §3, the one-pass
//! algorithm), which the profile counter `execution.input_scans` reports.
void InitializeDecideScan(const ColumnDataCollection &data, ColumnDataScanState &scan);
//! The scans of the input this thread's Finalize has made (InitializeDecideScan).
idx_t &DecideInputScans();

//! `expr` lowered for ExpressionExecutor over the input's chunks, cached by node.
const Expression &CachedTransformToChunkExpression(ChunkExprCache &cache, const Expression &expr,
                                                   ClientContext &context);

//! Runs each expression of `executor` over `chunk`, whose first row is row `row_offset` of
//! the data, into `results`: only on the rows `*rows` marks (every row when null), NULL on
//! the others.
void ExecuteOnReadRows(ExpressionExecutor &executor, DataChunk &chunk, idx_t row_offset, const vector<bool> *rows,
                       DataChunk &results);
//! The same, expression i only on the rows `*reads[i]` marks (every row when null).
void ExecuteOnReadRows(ExpressionExecutor &executor, DataChunk &chunk, idx_t row_offset,
                       const vector<const vector<bool> *> &reads, DataChunk &results);

//! The rows among `read` (every row when null) that `condition` keeps, NULL dropping a row.
vector<bool> KeptRows(const Expression &condition, ChunkExprCache &chunk_expr_cache, ClientContext &context,
                      ColumnDataCollection &data, idx_t num_rows, const vector<bool> *read = nullptr);

//! The rows `read_rows` holds (every row when null) and `row_filter` keeps numbered by their group of `keys[key]` --
//! a row whose written key column is NULL is in none --, the groups renumbered densely in key-value order;
//! INVALID_INDEX on every other row. Returns the kept groups' numbers in the key's cache entry.
vector<idx_t> LookupOrBuildPerGroupIds(PerGroupCache &cache, const vector<DecideKey> &keys, idx_t key,
                                       ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                       ColumnDataCollection &data, idx_t num_rows,
                                       const std::function<bool(idx_t)> &row_filter, const vector<bool> *read_rows,
                                       vector<idx_t> &out_row_group_ids, idx_t &out_num_groups,
                                       const std::function<bool(idx_t)> *loose_row_filter = nullptr,
                                       vector<idx_t> *out_loose_row_group_ids = nullptr);

//! `expr` on each row `rows` keeps (every row when null), NULL on the others.
vector<Value> ReadRowValues(const Expression &expr, ChunkExprCache &chunk_expr_cache, ClientContext &context,
                            ColumnDataCollection &data, const vector<bool> *rows);

//! A class or group, as a message names it: `D.depotID = 'D1', D.region = 'East'`.
string DescribeKeyValues(const vector<string> &names, const vector<Value> &values);

} // namespace duckdb
