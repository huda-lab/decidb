//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/formulation/ilp_linearization_internal.hpp
//
// Helpers shared between the lowering passes in ilp_linearization.cpp and its
// linearization_*.cpp siblings: global-auxiliary allocation and the per-row
// range walks every Big-M sizes itself from. Internal to layer 6 — nothing
// outside src/decidb/formulation/ includes this. The nested namespace keeps
// these short names out of `duckdb`; each file pulls it in with a file-scope
// `using namespace decide_linearize;`.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/formulation/ilp_linearization.hpp"

namespace duckdb {
namespace decide_linearize {

//! Append a continuous auxiliary to the flat global block and return its index.
idx_t AddGlobalContinuousAux(SolverInput &input, const VarIndexer &indexer, const AuxRange &range,
                             double obj_coeff, const string &label = string());

//! Append a binary auxiliary to the flat global block and return its index.
idx_t AddGlobalBinaryAux(SolverInput &input, const VarIndexer &indexer, double obj_coeff,
                         const string &label = string());

//! Fix a global auxiliary to a constant by collapsing its box onto `value`.
void PinGlobalAux(SolverInput &input, const VarIndexer &indexer, idx_t aux_idx, double value);

//! The constant part of one row's LHS: the terms whose variable index is fixed.
double DecideRowFixedLhsOffset(const vector<idx_t> &variable_indices,
                               const vector<CoefficientColumn> &row_coefficients, idx_t row);

//! One row's RHS with its fixed LHS offset already moved across.
double DecideRowEffectiveBound(const EvaluatedConstraint &ec, idx_t row);

//! Signed reachable interval of the variable part of `ec`'s LHS on `row`, as opposed to
//! `DecideRowTermRange`, which returns an unsigned magnitude because a Big-M only has to
//! dominate one. A collapse needs to know which side of `K` the range lies on, so the
//! coefficient sign has to be respected rather than taken through `abs`.
//!
//! `skip_idx` leaves one column out of the walk, the way DecideRowTermRange does: the
//! ABS envelope rows carry the auxiliary itself as a term, and what has to be bracketed
//! there is the expression the auxiliary is pinned AGAINST, not the auxiliary.
//!
//! `lower_bounds` / `upper_bounds` must be the rigid box: see
//! `SolverInput::rigid_lower_bounds`.
void DecideRowSignedRange(const EvaluatedConstraint &ec, idx_t row, const vector<double> &lower_bounds,
                          const vector<double> &upper_bounds, double &out_lo, double &out_hi,
                          idx_t skip_idx = DConstants::INVALID_INDEX);

//! Refuse a Big-M that has no finite M, naming the column to bound. Every caller
//! refuses in the same words, so the message the user reads lives in one place.
//! `bad` is the column to blame, or INVALID_INDEX when the open bound could not be
//! attributed to any named column.
[[noreturn]] void ThrowUnboundedBigMNaming(idx_t bad, const vector<string> &var_names, const char *construct);

//! The first term whose column is unbounded in the direction a Big-M would need,
//! or INVALID_INDEX when every contributor is boxed.
idx_t FindUnboundedContributor(const EvaluatedConstraint &ec, const vector<double> &lower_bounds,
                               const vector<double> &upper_bounds);

} // namespace decide_linearize
} // namespace duckdb
