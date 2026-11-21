//===----------------------------------------------------------------------===//
//                         DeciDB
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
idx_t AddGlobalContinuousAux(SolverInput &input, const VarIndexer &indexer, const AuxRange &range, double obj_coeff,
                             const string &label = string());

//! Append a binary auxiliary to the flat global block and return its index.
idx_t AddGlobalBinaryAux(SolverInput &input, const VarIndexer &indexer, double obj_coeff,
                         const string &label = string());

//! The constant part of one row's LHS: the terms whose variable index is fixed. `written` names the clause when the
//! sum grows past a DOUBLE (AddCoefficient).
double DecideRowFixedLhsOffset(const vector<idx_t> &variable_indices, const vector<CoefficientColumn> &row_coefficients,
                               idx_t row, const DecideClauseRef &written);

//! One row's RHS with its fixed LHS offset already moved across.
double DecideRowEffectiveBound(const EvaluatedConstraint &ec, idx_t row, const DecideClauseRef &written);

//! One row's expression bracketed against every contributing variable's box, each end on its own: the one walk behind
//! every signed range, the constraint side's (DecideRowSignedRange) and every MIN/MAX auxiliary's.
//!
//! An end that no contributing variable's box can derive is OPEN: `lo_open` / `hi_open` say so, and the value beside
//! the flag is meaningless (`Lo()` / `Hi()` read an open end as an infinity). A caller adding rows up ORs the flags. An
//! end that is not open but whose value overflowed a DOUBLE stays an infinite value with its flag clear: a box carries
//! it, and the hand-off names the clause, as every number too large for a DOUBLE does.
struct SignedRowRange {
	double lo = 0.0;
	double hi = 0.0;
	bool lo_open = false;
	bool hi_open = false;
	//! The first variable that opened either end.
	idx_t unbounded_var = DConstants::INVALID_INDEX;

	//! The ends as numbers, an open end as an infinity.
	double Lo() const {
		return lo_open ? -std::numeric_limits<double>::infinity() : lo;
	}
	double Hi() const {
		return hi_open ? std::numeric_limits<double>::infinity() : hi;
	}

	//! Fold this bracket, shifted by `constant`, into `range` as one row, each end on its own.
	void CoverInto(AuxRange &range, double constant = 0.0) const {
		range.CoverRowEnds(lo + constant, hi + constant, lo_open, hi_open, unbounded_var);
	}

	//! The bracket of a row of `term_count` terms. `variable_at(t)` and `coefficient_at(t)` read term `t`, a term with
	//! no column is a constant, and `lower_of(v)` / `upper_of(v)` read a variable's box.
	template <class VARIABLE_AT, class COEFFICIENT_AT, class LOWER_OF, class UPPER_OF>
	static SignedRowRange Of(idx_t term_count, VARIABLE_AT variable_at, COEFFICIENT_AT coefficient_at,
	                         LOWER_OF lower_of, UPPER_OF upper_of) {
		SignedRowRange out;
		for (idx_t t = 0; t < term_count; t++) {
			double c = coefficient_at(t);
			if (std::abs(c) < 1e-15) {
				continue;
			}
			idx_t v = variable_at(t);
			if (v == DConstants::INVALID_INDEX) {
				out.lo += c;
				out.hi += c;
				continue;
			}
			double lb = lower_of(v);
			double ub = upper_of(v);
			// A negative coefficient swaps which end of the variable's box feeds which end of the term, so an open
			// ceiling on `v` can open the term's FLOOR: sign has to be respected before blaming a side.
			bool lb_open = DecideIsOpenBelow(lb);
			bool ub_open = DecideIsOpenAbove(ub);
			bool lo_from_ub = c < 0.0;
			if (lo_from_ub ? ub_open : lb_open) {
				if (out.unbounded_var == DConstants::INVALID_INDEX) {
					out.unbounded_var = v;
				}
				out.lo_open = true;
			} else {
				out.lo += lo_from_ub ? c * ub : c * lb;
			}
			if (lo_from_ub ? lb_open : ub_open) {
				if (out.unbounded_var == DConstants::INVALID_INDEX) {
					out.unbounded_var = v;
				}
				out.hi_open = true;
			} else {
				out.hi += lo_from_ub ? c * lb : c * ub;
			}
		}
		return out;
	}
};

//! Signed reachable interval of the variable part of `ec`'s LHS on `row`, as opposed to
//! `DecideRowTermRange`, which returns an unsigned magnitude because a Big-M only has to
//! dominate one. A collapse needs to know which side of `K` the range lies on, so the
//! coefficient sign has to be respected rather than taken through `abs`.
//!
//! `skip_idx` leaves one column out of the walk: the
//! ABS envelope rows carry the auxiliary itself as a term, and what has to be bracketed
//! there is the expression the auxiliary is pinned AGAINST, not the auxiliary.
//!
//! `lower_bounds` / `upper_bounds` must be the rigid box: see
//! `SolverInput::rigid_lower_bounds`.
//!
//! `instances`, when given, reads each instance's box (InstanceBox) instead of the decision's: a lowered decision keeps
//! its default floor of 0 on the instances it was not lowered on, and a formulation view also reads a helper's reach.
//! A reader of the rigid box passes the rigid view.
SignedRowRange DecideRowSignedRange(const EvaluatedConstraint &ec, idx_t row, const vector<double> &lower_bounds,
                                    const vector<double> &upper_bounds, idx_t skip_idx = DConstants::INVALID_INDEX,
                                    const InstanceBox *instances = nullptr);

//! Refuse a Big-M that has no finite M. Every caller refuses in the same words, so the
//! message the user reads lives in one place. `open` says whether a column of the row
//! has an open box (an auxiliary's counts): `bad` is then the column to name. With
//! no open column the reach overflowed a DOUBLE, and the refusal is the TOO_LARGE
//! message pointing at `written`, the clause the row belongs to (DecideWrittenClause).
[[noreturn]] void ThrowUnboundedBigMNaming(bool open, idx_t bad, const vector<string> &var_names, const char *construct,
                                           const DecideClauseRef &written);

//! The first term whose column is unbounded in the direction a Big-M would need,
//! or INVALID_INDEX when every contributor is boxed.
idx_t FindUnboundedContributor(const EvaluatedConstraint &ec, const vector<double> &lower_bounds,
                               const vector<double> &upper_bounds);

//! The first decision among a helper's listed members whose box is open, which a refusal names; INVALID_INDEX when
//! every one is boxed.
idx_t FirstUnboundedMemberVariable(const ReducerValueMembers &members, const vector<double> &lower_bounds,
                                   const vector<double> &upper_bounds);

} // namespace decide_linearize
} // namespace duckdb
