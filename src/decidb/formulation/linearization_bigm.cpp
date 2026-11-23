//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/formulation/linearization_bigm.cpp
//
// Data-driven Big-M sizing and the per-row range walks it rests on. See ilp_linearization.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/common/decide_value_messages.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"

namespace duckdb {

using namespace decide_linearize; // NOLINT: internal DECIDE linearization helpers

// --- Data-driven Big-M support ---------------------------------------------
// A Big-M linearization toggles a constraint on/off via a binary indicator. The
// constant M must be at least the reachable magnitude of the constraint's
// left-hand expression: too small silently distorts the feasible region (wrong
// answer), too large degrades numerical stability. We therefore derive M from
// the actual variable bounds and per-row coefficient data instead of a fixed
// constant, mirroring the long-standing ABS-maximize path.

double DecideRowTermRange(const ClauseRecords &records, idx_t e, const vector<double> &lower_bounds,
                          const vector<double> &upper_bounds, bool &has_unbounded, const InstanceBox &instances) {
	double sum = 0.0;
	for (idx_t t = records.term_begin[e]; t < records.term_begin[e + 1]; t++) {
		double coef = std::abs(records.coefficients[t]);
		if (coef < 1e-15) {
			continue;
		}
		double lb = instances.LowerOfRow(records.variables[t], records.rows[t], lower_bounds);
		double ub = instances.UpperOfRow(records.variables[t], records.rows[t], upper_bounds);
		if (DecideIsOpenAbove(ub) || DecideIsOpenBelow(lb)) {
			has_unbounded = true;
			continue;
		}
		sum += coef * std::max(std::abs(lb), std::abs(ub));
	}
	return sum;
}

//! Signed reachable interval of the decision terms of row `e` that `ec` emits, as opposed to
//! `DecideRowTermRange`, which returns an unsigned magnitude because a Big-M only has to
//! dominate one. A collapse needs to know which side of `K` the range lies on, so the
//! coefficient sign has to be respected rather than taken through `abs`.
//!
//! An unbounded side is an open end (SignedRowRange) rather than a failure: the two
//! collapses are one-sided, and `SUM(x) <> 0` over decisions that are merely
//! non-negative — the case worth serving — has a finite floor and no ceiling at all.
//! `Lo()` / `Hi()` read an open end as an infinity, against which comparisons simply
//! never fire, so the classifier needs no separate unbounded branch.
//!
//! `lower_bounds` / `upper_bounds` must be the rigid box: see
//! `SolverInput::rigid_lower_bounds`.
SignedRowRange decide_linearize::DecideRowSignedRange(const EvaluatedConstraint &ec, idx_t e,
                                                      const vector<double> &lower_bounds,
                                                      const vector<double> &upper_bounds, idx_t skip_idx,
                                                      const InstanceBox *instances) {
	auto &records = ec.records;
	const idx_t begin = records.term_begin[e];
	auto variable = [&](idx_t t) {
		return records.variables[begin + t];
	};
	auto row = [&](idx_t t) {
		return records.rows[begin + t];
	};
	return SignedRowRange::Of(
	    records.term_begin[e + 1] - begin, variable,
	    // The row's constant is its bound's (ClauseRecords::EffectiveBound), and `skip_idx` is left out: read as zero.
	    [&](idx_t t) { return variable(t) == skip_idx ? 0.0 : records.coefficients[begin + t]; },
	    [&](idx_t t) {
		    return instances ? instances->LowerOfRow(variable(t), row(t), lower_bounds) : lower_bounds[variable(t)];
	    },
	    [&](idx_t t) {
		    return instances ? instances->UpperOfRow(variable(t), row(t), upper_bounds) : upper_bounds[variable(t)];
	    });
}

//! Refuse a Big-M linearization that has no finite M, naming the column that has no finite bound.
//!
//! A Big-M constant must dominate the reachable magnitude of the row's left-hand
//! side. When a contributing decision variable has no finite bound, no constant does
//! — so there is no M, only a guess. DeciDB refuses rather than guessing: a guessed M
//! that the true range exceeds does not fail, it silently cuts the feasible region
//! and returns a confidently wrong answer.
//!
//! The lowered ABS (`EmitAbsConstraints`) has always refused in exactly this situation. This is the
//! same refusal, in the same words, for the constructs that used to take a 1e6 floor
//! instead — and it applies on every backend, so one query never answers correctly on
//! one solver and wrongly on another.
//!
//! The callers differ only in how they locate the column to blame, so the words the
//! user reads live here, once. `open` says whether a column of the row has an open box
//! (an auxiliary's counts); `bad` is then that column. With no open column the reach
//! overflowed a DOUBLE: nothing is missing a bound, and the refusal points at `written`, the
//! clause the row belongs to, as every number too large for a DOUBLE does (AddCoefficient).
[[noreturn]] void decide_linearize::ThrowUnboundedBigMNaming(bool open, idx_t bad, const vector<string> &var_names,
                                                             const char *construct, const DecideClauseRef &written) {
	if (!open) {
		throw DecideTooLargeError(written);
	}
	throw InvalidInputException(
	    StringUtil::Format("'%s' has no finite bound, which %s needs", var_names[bad], construct),
	    Exception::InitializeExtraInfo(written.place));
}

//! Locate the blame for a row Big-M: the first contributing decision variable whose box
//! is open, and so a column the user can actually bound. The indicators a rewrite created
//! are skipped — they are ours, not theirs, and they are binary anyway.
//!
//! Separate from the throw because a caller working in FLAT columns still has to name a
//! decide variable, and the range walk that discovers the openness there
//! (`DecideRowSignedRange`) reports an infinity rather than a culprit.
idx_t decide_linearize::FindUnboundedContributor(const EvaluatedConstraint &ec, const vector<double> &lower_bounds,
                                                 const vector<double> &upper_bounds, idx_t skipped) {
	for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
		idx_t v = ec.variable_indices[t];
		if (v == DConstants::INVALID_INDEX || v == skipped) {
			continue;
		}
		if (DecideIsOpenAbove(upper_bounds[v]) || DecideIsOpenBelow(lower_bounds[v])) {
			return v;
		}
	}
	return DConstants::INVALID_INDEX;
}

double DecideTightPerRowBigM(const EvaluatedConstraint &ec, const FormulationBox &box, const vector<string> &var_names,
                             const InstanceBox &instances, const DecideClauseRef &written, const char *construct) {
	const vector<double> &lower_bounds = box.lower;
	const vector<double> &upper_bounds = box.upper;
	bool has_unbounded = false;
	double M = 0.0;
	for (idx_t e = 0; e < ec.records.Count(); e++) {
		double rhs = ec.records.EffectiveBound(e);
		// Finite: the only rows asking for an M here are the links of a count of nonzeros, whose bound is the
		// constant of the norm's argument, and RewriteAbs refuses that where it reads the argument (a NaN, an
		// infinity, or finite values whose sum passes a DOUBLE).
		D_ASSERT(std::isfinite(rhs));
		double rhs_mag = std::abs(rhs);
		double range =
		    rhs_mag + DecideRowTermRange(ec.records, e, lower_bounds, upper_bounds, has_unbounded, instances);
		M = std::max(M, range);
	}
	// Refuse before finishing the constant: an open contributor means there is no M
	// to return, so the slack term below would only be computed and thrown away.
	if (has_unbounded) {
		ThrowUnboundedBigMNaming(true,
		                         FindUnboundedContributor(ec, lower_bounds, upper_bounds, DConstants::INVALID_INDEX),
		                         var_names, construct, written);
	}
	return M + 1.0;
}

void DecidePropagateImpliedBounds(SolverInput &input, const VarIndexer &indexer, idx_t num_rows) {
	const auto &constraints = input.constraints;
	const auto &lower_bounds = input.lower_bounds;
	auto &upper_bounds = input.upper_bounds;
	// The bound each solver column gets from the clause at hand, NaN for none; `touched` resets it per clause.
	vector<double> column_bound(indexer.global_block_start, std::numeric_limits<double>::quiet_NaN());
	vector<int> touched;
	for (auto &ec : constraints) {
		if (ec.comparison_type != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
		    ec.comparison_type != ExpressionType::COMPARE_EQUAL) {
			continue;
		}
		// Soundness requires every term to be non-negative — both the variable
		// lower bounds (x >= 0) AND the coefficients (a >= 0) — so that dropping
		// the other terms to derive x_t <= K/a_t is valid. Constraints with any
		// negative coefficient (e.g. the IN/ABS rewrites such as x - z1 - 3*z2 = 0)
		// must be skipped: dropping a negative term would wrongly tighten the
		// bound and cut feasible points (or make the model infeasible). A per-row
		// constraint over one variable drops nothing, so its sign does not matter there:
		// `x <= cap` bounds a signed `x` as well, by a bound that may itself be negative.
		// An aggregate one (`SUM(x) = -3`) sums every row's instance, so it does not.
		idx_t only_variable = DConstants::INVALID_INDEX;
		bool single_variable = !ec.lhs_is_aggregate;
		for (idx_t v : ec.variable_indices) {
			if (v == DConstants::INVALID_INDEX) {
				continue;
			}
			single_variable =
			    single_variable && only_variable == DConstants::INVALID_INDEX; // equal terms of one decision are merged
			only_variable = v;
		}
		bool nonneg = true;
		for (idx_t v : ec.variable_indices) {
			if (!single_variable && v != DConstants::INVALID_INDEX && lower_bounds[v] < 0.0) {
				nonneg = false;
				break;
			}
		}
		if (!nonneg) {
			continue;
		}
		const auto written = DecideWrittenClause(input, ec.provenance.origin.source_clause_id);
		// Each row the clause emits, read as the model builder emits it (ClauseEmittedRows). A solver column's
		// coefficient is the sum of its terms over the row's data rows, so a decision that stands in several rows of a
		// group, or one counted once beside the reducers, is bounded by what the row says about its instance. The
		// row's bound is the clause's less its fixed terms, as the builder states it. A per-row clause emits its kept
		// rows only: an excluded row's class's first row bounds the same instance.
		auto &emitted = ClauseEmittedRows(ec, input, indexer, written);
		auto &records = ec.records;
		for (idx_t e = 0; nonneg && e < records.Count(); e++) {
			double k = records.EffectiveBound(e);
			for (idx_t i = emitted.offsets[e]; i < emitted.offsets[e + 1]; i++) {
				double a = emitted.values[i];
				if (a < -1e-15) {
					nonneg = false;
					break;
				}
				if (a <= 1e-15 || (k < 0.0 && !single_variable)) {
					continue;
				}
				// A bound on one decision caps it at the greatest value it admits (stage 08), which the quotient
				// may miss: `0.1 * x <= 0.3` admits 3, while 0.3 / 0.1 is just below 3.
				double bound = records.admitted_upper.empty() ? k / a : records.admitted_upper[e];
				auto column = emitted.columns[i];
				if (std::isnan(column_bound[column])) {
					touched.push_back(column);
					column_bound[column] = bound;
				} else {
					column_bound[column] = std::min(column_bound[column], bound);
				}
			}
		}
		// The shared bound ub_x applies to EVERY instance of x, so the clause bounds x only when one of its rows
		// bounds each instance, and then by the loosest of their bounds: an instance a row the clause excludes reads
		// (a `when` keeps it out) and no kept row bounds leaves x unbounded. With no data rows the loop never runs and
		// the bound stays at its start: harmless, there is no instance of x to bind.
		for (idx_t t = 0; nonneg && t < ec.variable_indices.size(); t++) {
			idx_t v = ec.variable_indices[t];
			if (v == DConstants::INVALID_INDEX) {
				continue;
			}
			bool already_handled = false;
			for (idx_t prev = 0; prev < t; prev++) {
				already_handled = already_handled || ec.variable_indices[prev] == v;
			}
			if (already_handled) {
				continue;
			}
			double bound = single_variable ? -std::numeric_limits<double>::infinity() : 0.0;
			bool every_instance_bounded = true;
			for (idx_t r = 0; r < num_rows && every_instance_bounded; r++) {
				// A row in no class reads no instance, and has none to bound.
				const idx_t column = indexer.ColumnOrNone(v, r);
				if (column == DConstants::INVALID_INDEX) {
					continue;
				}
				double instance_bound = column_bound[column];
				every_instance_bounded = !std::isnan(instance_bound);
				bound = std::max(bound, instance_bound);
			}
			// A bound below the column's floor is kept too: the box is empty, which proves
			// the model infeasible, and the solver says so. Dropping it instead left the
			// column open, and a Big-M over it refused for a want of bounds the clauses had.
			// (One under the floor by rounding alone is not empty: the model builder
			// closes it onto the floor.) An end of -inf admits no value and is no box: the row stays, and the builder
			// proves it infeasible (ProvesInfeasible), as FoldKnownBound leaves such a row.
			if (every_instance_bounded && bound < upper_bounds[v] &&
			    bound != -std::numeric_limits<double>::infinity()) {
				upper_bounds[v] = bound;
			}
		}
		for (auto column : touched) {
			column_bound[column] = std::numeric_limits<double>::quiet_NaN();
		}
		touched.clear();
	}
}

} // namespace duckdb
