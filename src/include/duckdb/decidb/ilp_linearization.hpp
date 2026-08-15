//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/ilp_linearization.hpp
//
// Stage 06 — linearization of the evaluated decision problem.
//
// A formulation chosen at stage 05 (a `<>` disjunction, a hard MIN/MAX, an ABS
// under MAXIMIZE, a bilinear product) reaches this stage as a *tag* on an
// `EvaluatedConstraint`: stage 05 recorded which encoding applies and stopped,
// because the rows that encode it and the constants that scale them are both
// functions of the evaluated data. This unit owns that second half — it turns a
// tagged constraint into the rows a solver can accept, and derives the Big-M
// constants those rows need.
//
// Everything here is a pure function of `SolverInput` data: evaluated
// coefficients, variable bounds, and row/group ids. No expression tree, no
// executor, no data scan.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/solver_input.hpp"

namespace duckdb {

//! Legacy fixed Big-M, retained only as a non-strict fallback for genuinely
//! unbounded variables (no query is rejected; behavior matches the prior code).
static constexpr double DECIDE_BIGM_FALLBACK = 1e6;

//! Worst-case absolute contribution of row `r`'s decision variables:
//! sum over terms of |coef[t][r]| * max(|lb|,|ub|). Constant terms
//! (var == INVALID_INDEX, folded into the RHS by callers) are skipped. If any
//! contributing variable lacks a finite bound, `has_unbounded` is set and that
//! term is omitted so the caller can apply a conservative fallback.
double DecideRowTermRange(const vector<idx_t> &variable_indices,
                          const vector<CoefficientColumn> &row_coefficients,
                          idx_t row, const vector<double> &lower_bounds,
                          const vector<double> &upper_bounds,
                          bool &has_unbounded,
                          idx_t skip_idx = DConstants::INVALID_INDEX);

//! Tight scalar Big-M for a per-row indicator constraint: the maximum over
//! active rows of |rhs[r]| + (worst-case row contribution), plus a 1-unit margin
//! that covers the integer-step band of the `<>` rewrite (harmless slack for the
//! MIN/MAX rewrites). When every contributing variable is bounded this is exact
//! and typically far below 1e6; otherwise we keep the 1e6 floor.
double DecideTightPerRowBigM(const EvaluatedConstraint &ec,
                             const vector<double> &lower_bounds,
                             const vector<double> &upper_bounds,
                             idx_t num_rows);

//! Data-driven implied-bound propagation. For a non-negative `<=`/`=` constraint
//! Sum_t a_t x_t (<=|=) K with a_t >= 0 and x_t >= 0, each variable instance
//! satisfies x <= K / a (the other non-negative terms only help), so a sound
//! shared upper bound is max over the variable's positive-coefficient rows of
//! K_r / a_r, where a_r is the variable's COMBINED coefficient at that row --
//! see the per-variable loop in the implementation. This converts many
//! declared-unbounded variables into bounded ones, enabling a finite, correct,
//! tighter Big-M. Only provably-implied bounds are applied, so the feasible
//! region — and the optimum — are unchanged.
//!
//! Single pass (not a fixpoint): a bound derived for one variable is not fed back
//! to tighten others in the same sweep. This is sound — it only leaves some
//! tightness on the table for chained implications — and avoids iterating to
//! convergence over potentially many constraints.
void DecidePropagateImpliedBounds(const vector<EvaluatedConstraint> &constraints,
                                  vector<double> &lower_bounds,
                                  vector<double> &upper_bounds, idx_t num_rows);

//! Encode every constraint stage 05 tagged with a hard MIN/MAX indicator:
//!   MAX(expr) >= K: for each row i, expr_i - M*y_i >= K - M, and SUM(y) >= 1
//!   MIN(expr) <= K: for each row i, expr_i + M*y_i <= K + M, and SUM(y) >= 1
//! Constraints are matched to their indicator variables via `minmax_indicator_idx`
//! (not positionally). Untagged constraints pass through unchanged, so `constraints`
//! is replaced in place by the encoded list. A group whose bound is unreachable in
//! its own direction is emitted as a plain per-row constraint instead, and a group
//! whose bound every assignment satisfies is dropped.
void LinearizeMinMaxIndicators(vector<EvaluatedConstraint> &constraints,
                               const vector<double> &lower_bounds,
                               const vector<double> &upper_bounds, idx_t num_rows);

} // namespace duckdb
