//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/execution/operator/decide/decide_row_evaluation.hpp
//
// How PhysicalDecide's translation units read the materialized input.
//===----------------------------------------------------------------------===//
//
// How a message names a class. Defined in physical_decide.cpp; declared here so the frame selectors
// (physical_decide_frames.cpp) name one the same way.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/decide/physical_decide.hpp"

#include <functional>

namespace duckdb {

//! A class or group, as a message names it: `D.depotID = 'D1', D.region = 'East'`.
string DescribeKeyValues(const vector<string> &names, const vector<Value> &values);

} // namespace duckdb
