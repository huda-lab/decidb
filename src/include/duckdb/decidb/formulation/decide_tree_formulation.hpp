//===----------------------------------------------------------------------===//
//                         DeciDB
//
// Formulation of synthesized E/G/O values, with no prepared linear-form input.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/execution/operator/decide/decide_tree_evaluation.hpp"

namespace duckdb {

//! Consume actual root values. SUMs are expanded algebraically; an extremum
//! acquires a column only when more than one surviving member needs one.
//! The caller runs the evaluation's RequireSupported, initializes declaration
//! boxes, scopes and keyed mappings, and constructs the indexer after marking
//! rewritten helper decisions INLINED.
void FormulateDecideTree(const DecideTree &tree, const DecideDescent &descent, const DecideTreeEvaluation &evaluation,
                         SolverInput &input, const VarIndexer &indexer,
                         NativeConstructPolicy policy = NativeConstructPolicy(),
                         const vector<string> &variable_names = vector<string>(), bool repair = false);

} // namespace duckdb
