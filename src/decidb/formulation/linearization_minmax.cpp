//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/formulation/linearization_minmax.cpp
//
// The extremum link: the one place a MIN/MAX helper's definition is stated. See ilp_linearization.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/exception.hpp"

#include <cmath>
#include <limits>
#include <unordered_map>
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"

namespace duckdb {

using namespace decide_linearize; // NOLINT: internal DECIDE linearization helpers

//===--------------------------------------------------------------------===//
// Extremum links — the one place a MIN/MAX auxiliary is pinned
//===--------------------------------------------------------------------===//

//! The column a native general constraint takes for one member.
//!
//! The member's OWN column where the expression is nothing but that column. `MAX(x)`
//! over a decision variable is the common shape, and there a pinned `t = x` costs one
//! continuous column and one equality row per member to say nothing — and they are not
//! free, because a general constraint READS its arguments, so presolve cannot substitute
//! the copies away. Measured on `MAXIMIZE MAX(x)` at 15K rows, handing the column itself
//! to the general constraint took the solve from 0.97s to 0.29s for the same answer.
//!
//! Only an EXACT renaming qualifies: one surviving column, unit coefficient, no constant.
//! `MAX(2 * x)` and `MAX(x + 1)` are genuine expressions, and a general constraint relates
//! columns, so those still earn one. A pinned column is BOXED by the member's own reach —
//! a free continuous column is a measured cliff at the root LP — and left free only where
//! no reach is derivable at all, which is exactly the query the native arm exists for.
//!
//! `link` holds member `i`'s `(result - expr)`, so a renaming reads as the single coefficient `-1`. The member's reach
//! is asked of `members` only once it is known to need a column.
static int ExtremumArgumentColumn(SolverInput &input, const VarIndexer &indexer, const MinMaxLinkRow &link,
                                  const ExtremumMemberSource &members, idx_t i, const string &label) {
	int sole_column = -1;
	double sole_coefficient = 0.0;
	idx_t surviving = 0;
	for (idx_t k = 0; k < link.indices.size(); k++) {
		if (std::abs(link.coefficients[k]) < 1e-15) {
			continue;
		}
		surviving++;
		sole_column = link.indices[k];
		sole_coefficient = link.coefficients[k];
	}
	if (surviving == 1 && sole_coefficient == -1.0 && link.constant == 0.0) {
		return sole_column;
	}
	idx_t t_idx = AddGlobalContinuousAux(input, indexer, members.range(i), 0.0, label);
	auto pin = link.LeftHandSide((int)t_idx);
	pin.sense = '=';
	pin.rhs = link.constant;
	pin.provenance.kind = ConstraintKind::STRUCTURAL;
	input.global_constraints.push_back(std::move(pin));
	return (int)t_idx;
}

void EmitExtremumLink(SolverInput &input, const VarIndexer &indexer, const ExtremumLinkSpec &spec,
                      const ExtremumMemberSource &members, const vector<string> &var_names,
                      NativeConstructPolicy policy) {
	if (members.count == 0) {
		return;
	}
	int result_col = (int)spec.result_column;
	// One scratch row serves every member: each is built into it, copied into the row the model receives, and
	// overwritten by the next.
	MinMaxLinkRow link;
	auto fill_member = [&](idx_t i) -> const MinMaxLinkRow & {
		link.indices.clear();
		link.coefficients.clear();
		link.constant = 0.0;
		members.fill_link(i, link);
		return link;
	};

	// A family of one member is that member (deck p. 112): one equality pins `result` from both sides, with no
	// indicator and no Big-M -- so a side no box bounds needs none.
	if (members.count == 1) {
		auto &member = fill_member(0);
		auto rc = member.LeftHandSide(result_col);
		rc.sense = '=';
		rc.rhs = member.constant;
		rc.provenance.kind = ConstraintKind::STRUCTURAL;
		input.global_constraints.push_back(std::move(rc));
		return;
	}

	// The envelope, on both arms. It is one row per member with no constant to dominate,
	// and on the native arm it is implied by the general constraint rather than
	// contradicted by it — so whether it is emitted is a question about the SITE (does
	// the surrounding model already supply this side?), never about the backend.
	if (spec.need_envelope) {
		char sense = spec.is_max ? '>' : '<';
		for (idx_t i = 0; i < members.count; i++) {
			auto &member = fill_member(i);
			auto rc = member.LeftHandSide(result_col);
			rc.sense = sense;
			rc.rhs = member.constant;
			rc.provenance.kind = ConstraintKind::STRUCTURAL;
			input.global_constraints.push_back(std::move(rc));
		}
	}
	if (!spec.need_closing) {
		return;
	}

	// THE CHOICE, made once. A general constraint states the closing side outright and
	// needs no constant, which is the whole reason the arm exists: a family with no
	// derivable reach has no Big-M and therefore no lowering. Where a Big-M does exist
	// the lowering is the smaller model — measured — so native is the fallback.
	if (policy.Use(spec.closing_underivable)) {
		vector<int> args;
		args.reserve(members.count);
		for (idx_t i = 0; i < members.count; i++) {
			args.push_back(ExtremumArgumentColumn(input, indexer, fill_member(i), members, i, spec.label));
		}
		GeneralConstraintSpec gc;
		gc.kind = spec.is_max ? GeneralConstraintKind::MAX : GeneralConstraintKind::MIN;
		gc.result_column = result_col;
		gc.argument_columns = std::move(args);
		input.general_constraints.push_back(std::move(gc));
		return;
	}
	// A span past a DOUBLE (`MAX(x + c)` over `c` of either sign near the limit) is no Big-M either, and no column is
	// open: the refusal names the clause. The closing rows carry no clause of their own to name at the hand-off.
	if (spec.closing_underivable || !std::isfinite(spec.closing_big_m)) {
		ThrowUnboundedBigMNaming(spec.blame_var != DConstants::INVALID_INDEX, spec.blame_var, var_names, "MIN or MAX",
		                         spec.written);
	}

	// One indicator per member, a Big-M row that binds `result` to that member when the
	// indicator is on, and `SUM(y) >= 1` so at least one binds.
	double M = spec.closing_big_m;
	double m_coeff = spec.is_max ? M : -M;
	char sense = spec.is_max ? '<' : '>';
	SolverInput::RawConstraint sum_y;
	sum_y.indices.reserve(members.count);
	sum_y.coefficients.reserve(members.count);
	for (idx_t i = 0; i < members.count; i++) {
		auto &member = fill_member(i);
		idx_t y_idx = AddGlobalBinaryAux(input, indexer, 0.0, spec.label);
		sum_y.indices.push_back((int)y_idx);
		sum_y.coefficients.push_back(1.0);

		auto rc = member.LeftHandSide(result_col, 1);
		rc.indices.push_back((int)y_idx);
		rc.coefficients.push_back(m_coeff);
		rc.sense = sense;
		rc.rhs = m_coeff + member.constant;
		rc.provenance.kind = ConstraintKind::STRUCTURAL;
		input.global_constraints.push_back(std::move(rc));
	}
	sum_y.sense = '>';
	sum_y.rhs = 1.0;
	sum_y.provenance.kind = ConstraintKind::STRUCTURAL;
	input.global_constraints.push_back(std::move(sum_y));
}

} // namespace duckdb
