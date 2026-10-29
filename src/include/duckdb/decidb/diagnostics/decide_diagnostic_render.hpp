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

//! The source clause a row came from, or null for a row with no clause. A label quotes it
//! (QuoteWrittenClause, QuoteEditedClause) so it reads as the clause the user typed.
const ConstraintSourceInfo *FindConstraintSource(const SolverModel &model, const ConstraintProvenance &provenance);

//! Render a row's RHS from its source spelling, falling back to `fallback`.
string SourceAwareRhs(const SolverModel &model, const ConstraintProvenance &provenance, double fallback);

//! Whether a row of `source` is one end of a range clause the user wrote as one: a half of a BETWEEN. A message quotes
//! the whole clause (QuoteWrittenClause), and an edit spells it back as a BETWEEN with the end moved (SpellRangeEdits).
//! A BETWEEN whose halves do not read as one range (a canonicalization that turned one half around) is not.
bool IsRangeEnd(const SolverModel &model, idx_t source_clause_id, char sense);

//! Replace the edits of a range clause's ends by one edit of the clause: `x BETWEEN lo AND hi` with the moved ends
//! spelled and the other end as written; both ends moved make one edit with the summed amount. Other edits pass
//! through. Ends merge when they share a group and a row.
vector<ClauseEdit> SpellRangeEdits(vector<ClauseEdit> edits, const SolverModel &model);

//! Build a LOOSEN edit from a constraint's rendered LHS + sense + RHS and the
//! solved slack `amount` (signed; for `=` it is the net s⁺−s⁻).
ClauseEdit MakeLoosenEdit(const ConstraintProvenance &prov, const ConstraintSourceInfo *source, const string &lhs,
                          double rhs, char sense, double amount);

//! The findings one emitted row stands for (D28): one per class of its group's `per` key
//! (`expanded_group`, the value in `group`); in the expanded slack scope, one per kept row
//! when the clause has no `per` key (`expanded_row`, the row in `row`) -- each kept row
//! its group's row stands for, or the kept row a per-row row was emitted at. A group's
//! row that names no class (`per ()`, or kept rows in the query scope) is the edit with
//! no group; any other row, the edit itself. The elastic readback asks for the expanded
//! scope only, the unreachable-bound scan for either.
vector<ClauseEdit> FanOutByClass(ClauseEdit edit, const ConstraintProvenance &prov, const SolverModel &model,
                                 bool expanded);

//! Name the clauses whose rows can never be satisfied by any assignment. `expanded`
//! reports a grouped clause once per class (FanOutByClass), as the elastic readback does
//! in that scope.
vector<UnreachableClause> CollectUnreachableClauses(const SolverModel &model, const vector<ColumnProvenance> &columns,
                                                    bool expanded);

//! Fold a clause's per-row data slack into one synthetic query-level offset
//! (`x <= col + delta`; `>=` loosens downward → `col - delta`; `=` by its signed slack),
//! a strict `<` / `>` keeping its operator.
ClauseEdit MakeVirtualOffsetEdit(const ConstraintProvenance &prov, const ConstraintSourceInfo *source,
                                 const string &lhs, const string &rhs_text, char sense, double delta);

} // namespace decide_render
} // namespace duckdb
