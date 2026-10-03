#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/decide/direct/direct_rule.hpp"
#include "duckdb/optimizer/decide/direct/direct_solve.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/operator_extension.hpp"

namespace duckdb {

//! Wraps a rule's relational proposal in the result boundary. The boundary keeps DECIDE's external bindings and
//! types, checks the proposal's output-slot map against its child, and pins validation-only slots as live.
unique_ptr<LogicalOperator> MapDirectResult(DirectRelationalProposal proposal, vector<ColumnBinding> output_bindings,
                                            vector<LogicalType> output_types, idx_t decide_index,
                                            DirectSolveDecisionRecord record);

//! Lets a serialized plan containing the result boundary be read back.
unique_ptr<OperatorExtension> MakeDirectResultExtension();

} // namespace duckdb
