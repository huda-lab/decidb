//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_row_evaluation.hpp
//
// How PhysicalDecide's translation units read the materialized input.
//===----------------------------------------------------------------------===//
//
// Every clause reads the rows its scope keeps and nothing else (language/guide.md §6.1),
// so each read below takes the rows to read and is NULL on the others, and raises no error
// of theirs. The rows come from the one pass (decide_row_store.hpp). Defined in
// physical_decide.cpp, which reads the rows for every clause; declared here so the frame
// selectors' timelines (physical_decide_frames.cpp) read them the same way.
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

//! A class or group, as a message names it: `D.depotID = 'D1', D.region = 'East'`.
string DescribeKeyValues(const vector<string> &names, const vector<Value> &values);

} // namespace duckdb
