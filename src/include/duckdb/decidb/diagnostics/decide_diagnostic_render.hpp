//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/diagnostics/decide_diagnostic_render.hpp
//
// Clause rendering for DECIDE diagnostics: turn a solved model row back into the
// SQL text the user wrote, and build the LOOSEN / offset edits quoted against it.
// Split out of decide_diagnostic_engines so the engines file holds only search
// and repair logic. Internal to the diagnostics layer — the nested namespace
// keeps these deliberately generic names (FormatNum, SenseStr, ...) out of
// `duckdb`.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/decidb/diagnostics/decide_diagnostic.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"

namespace duckdb {
namespace decide_render {

//! Compact numeric formatting for user-facing constraint text (drops trailing
//! zeros: 12.5 not 12.500000, 10 not 10.0).
string FormatNum(double v);

//! Clean the sub-tolerance noise an I3 stage-2 read carries: snap to the nearest
//! integer when within a relative tolerance of one, otherwise trim a genuinely
//! fractional value to a fixed absolute precision.
double SnapDiagnosticValue(double v);

//! Round to the last decimal that is meaningful at this value's own magnitude, so
//! a reported repair is never one part in a million SHORT of actually repairing.
double SnapToPrecision(double v);

//! Render a row's LHS as the user wrote it when the model carries a source
//! spelling, falling back to rebuilding it from the row's own coefficients.
string SourceAwareLhs(const SolverModel &model, const ModelConstraint &row,
                      const vector<ColumnProvenance> &columns);

//! The written RHS spelling for a row, or empty when the model has none to honour.
string SourceWrittenRhs(const SolverModel &model, const ConstraintProvenance &provenance);

//! SourceAwareLhs for a quadratic row.
string SourceAwareQuadraticLhs(const SolverModel &model, const SolverModel::QuadraticConstraint &row,
                               const vector<ColumnProvenance> &columns);

//! Provenance re-pointed at the source clause, so an edit is attributed to the
//! clause the user typed rather than to the row the formulation emitted.
ConstraintProvenance SourceAwareProvenance(const SolverModel &model, const ConstraintProvenance &provenance);

//! Render a row's RHS from its source spelling, falling back to `fallback`.
string SourceAwareRhs(const SolverModel &model, const ConstraintProvenance &provenance, double fallback);

//! Build a LOOSEN edit from a constraint's rendered LHS + sense + RHS and the
//! solved slack `amount` (signed; for `=` it is the net s⁺−s⁻).
ClauseEdit MakeLoosenEdit(const ConstraintProvenance &prov, const string &lhs, double rhs, char sense,
                          double amount);

//! Name the clauses whose rows can never be satisfied by any assignment.
vector<UnreachableClause> CollectUnreachableClauses(const SolverModel &model,
                                                    const vector<ColumnProvenance> &columns);

//! Fold a clause's per-row data slack into one synthetic query-level offset
//! (`x <= col + delta`; `>=` loosens downward → `col - delta`).
ClauseEdit MakeVirtualOffsetEdit(const ConstraintProvenance &prov, const string &lhs, const string &rhs_text,
                                 char sense, double delta);

} // namespace decide_render
} // namespace duckdb
