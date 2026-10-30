//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/execution/operator/decide/decide_row_evaluation.hpp
//
// How PhysicalDecide's translation units read the materialized input.
//===----------------------------------------------------------------------===//
//
// Every clause reads the rows its scope keeps and nothing else (syntax_reference.md §6.1),
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

//! `expr` lowered for ExpressionExecutor over the input's chunks, cached by node.
const Expression &CachedTransformToChunkExpression(ChunkExprCache &cache, const Expression &expr,
                                                   ClientContext &context);

//! Runs each expression of `executor` over `chunk`, whose first row is row `row_offset` of
//! the data, into `results`: only on the rows `*rows` marks (every row when null), NULL on
//! the others.
void ExecuteOnReadRows(ExpressionExecutor &executor, DataChunk &chunk, idx_t row_offset, const vector<bool> *rows,
                       DataChunk &results);

//! The rows among `read` (every row when null) that `condition` keeps, NULL dropping a row.
vector<bool> KeptRows(const Expression &condition, ChunkExprCache &chunk_expr_cache, ClientContext &context,
                      ColumnDataCollection &data, idx_t num_rows, const vector<bool> *read = nullptr);

//! Numbers the rows by the value of `key_exprs` (already lowered), NULL a value, in
//! first-seen order; only rows `read_rows` holds (every row when null) and `row_filter`
//! keeps, INVALID_INDEX on the others. A key with no column has one group.
void BuildGroupIds(const vector<const Expression *> &key_exprs, ClientContext &context, ColumnDataCollection &data,
                   idx_t num_rows, const std::function<bool(idx_t)> &row_filter, vector<idx_t> &out_row_to_group,
                   idx_t &out_num_groups, vector<vector<Value>> *out_rep_keys = nullptr,
                   const vector<bool> *read_rows = nullptr);

//! `expr` on each of `rows`, as values (NULL on the others).
vector<Value> EvaluateRowValues(const Expression &expr, ChunkExprCache &chunk_expr_cache, ClientContext &context,
                                ColumnDataCollection &data, idx_t num_rows, const vector<bool> &rows);

//! A value as SQL writes it, for a message: 'East', 10, NULL.
string SqlLiteral(const Value &value);

//! A class or group, as a message names it: `D.depotID = 'D1', D.region = 'East'`.
string DescribeKeyValues(const vector<string> &names, const vector<Value> &values);

} // namespace duckdb
