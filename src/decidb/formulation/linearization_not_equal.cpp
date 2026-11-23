//===----------------------------------------------------------------------===//
//                         DeciDB
//
// src/decidb/formulation/linearization_not_equal.cpp
//
// `<>` lowering: collapse classification and the Big-M disjunction. See ilp_linearization.cpp.
//
//===----------------------------------------------------------------------===//
#include "duckdb/decidb/formulation/ilp_linearization.hpp"
#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cmath>
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"

namespace duckdb {

using namespace decide_linearize; // NOLINT: internal DECIDE linearization helpers

//===--------------------------------------------------------------------===//
// `<>` disjunctions
//===--------------------------------------------------------------------===//

//! The ±1 band is only semantically exact when the LHS is integer-valued, which the bind-time type gate
//! (DecideShapeCheck::CheckIntegerStep) guarantees. With
//! such an LHS and a non-integer K, `LHS <> K` is a tautology (no integer can equal K). The ±1 Big-M rewrite would
//! emit `LHS <= K-1 ∨ LHS >= K+1`, which on the integer lattice wrongly excludes
//! floor(K) and ceil(K) — both of which the original predicate accepted. Treat such
//! RHS values as a silent drop. An infinite K is the same case for the same reason —
//! no integer equals it — and it must drop rather than reach the Big-M constant,
//! which cannot dominate infinity.
static bool NEIsIntegerValuedRhs(double k) {
	return std::isfinite(k) && std::abs(k - std::round(k)) < 1e-9;
}

//! What a row's reachable range says about the point `<>` excludes.
//!
//! `LHS <> K` is a disjunction only when `K` sits strictly inside the range the LHS can
//! actually reach. When the range lies wholly on one side of `K`, one branch is dead and
//! the surviving branch is a plain inequality — no indicator, no Big-M, and an LP
//! relaxation that is tight instead of empty. The common `SUM(x) <> 0` over nonnegative
//! decisions is exactly this case: it is `SUM(x) >= 1`.
enum class NECollapse : uint8_t {
	DISJUNCTION, //!< K is interior: both branches live, keep the Big-M pair
	ALWAYS_TRUE, //!< K is unreachable: the row excludes nothing, drop it
	LOWER_ONLY,  //!< the range never exceeds K: `<> K` is exactly `<= K-1`
	UPPER_ONLY   //!< the range never falls below K: `<> K` is exactly `>= K+1`
};

//! Classify one row of a `<>` constraint. Only called for an integer-valued LHS and an integer `K`, so a half-unit
//! tolerance cleanly separates "can reach K" from "cannot".
static NECollapse ClassifyNERow(const EvaluatedConstraint &ec, idx_t e, const vector<double> &lower_bounds,
                                const vector<double> &upper_bounds, const InstanceBox &floors) {
	auto range = DecideRowSignedRange(ec, e, lower_bounds, upper_bounds, DConstants::INVALID_INDEX, &floors);
	double lo = range.Lo();
	double hi = range.Hi();
	// K is finite: a row whose bound is not whole (an infinite one included) is no row of the clause.
	double k = ec.records.EffectiveBound(e);
	if (hi < k - 0.5 || lo > k + 0.5) {
		return NECollapse::ALWAYS_TRUE;
	}
	if (lo > k - 0.5) {
		return NECollapse::UPPER_ONLY; // lo == K on the lattice: LHS >= K
	}
	if (hi < k + 0.5) {
		return NECollapse::LOWER_ONLY; // hi == K on the lattice: LHS <= K
	}
	return NECollapse::DISJUNCTION;
}

//! The collapse verdict for a whole constraint, or DISJUNCTION if its rows do not agree on one.
//!
//! Per-row `<>` shares one EvaluatedConstraint across every row, so a per-row verdict
//! would mean splitting it into up to three constraints. Rows disagree only when their
//! bounds or their RHS vary across rows, which is not the shape the collapse exists to
//! serve — a uniform `x <> 0` or `SUM(x) <> 0` yields one verdict for every row. So a
//! mixed constraint keeps the Big-M pair, unsplit. A row whose bound is not a whole number
//! excludes nothing (NEIsIntegerValuedRhs) and has no verdict.
static NECollapse ClassifyNEConstraint(const EvaluatedConstraint &ec, const vector<double> &lower_bounds,
                                       const vector<double> &upper_bounds, const InstanceBox &floors) {
	bool seen = false;
	NECollapse verdict = NECollapse::ALWAYS_TRUE;
	for (idx_t e = 0; e < ec.records.Count(); e++) {
		if (!NEIsIntegerValuedRhs(ec.records.bounds[e])) {
			continue;
		}
		NECollapse row_verdict = ClassifyNERow(ec, e, lower_bounds, upper_bounds, floors);
		if (!seen) {
			verdict = row_verdict;
			seen = true;
		} else if (row_verdict != verdict) {
			return NECollapse::DISJUNCTION;
		}
	}
	return verdict;
}

//! The clause a `<>` came from, quoted as written, naming every binary it allocates so a
//! diagnosis renders `SUM(x) <> 0` rather than an internal column.
static string NotEqualClauseLabel(const SolverInput &input, const EvaluatedConstraint &ec) {
	return QuoteWrittenClause(input.constraint_sources, ec.provenance.origin.source_clause_id, input.source_text);
}

//! One per-row `<>`, in whichever shape its own reachable range earns.
//!
//! `DISJUNCTION` is the general case: `z == 0 => LHS <= K-1` and `z == 1 => LHS >= K+1`,
//! a pair of CONDITIONAL ROWS on a binary of this row's own. That is the only spelling
//! emitted — whether the chosen backend states a condition itself or needs it encoded
//! with a Big-M is settled once, afterwards, by `LowerDecideConstructs`, so nothing here
//! asks for a bound or consults a backend.
//!
//! A COLLAPSED row is a plain inequality: one branch was unreachable, so there is no
//! disjunction left. It still allocates the binary, appearing in no row, because that
//! column is what carries the clause's text and what groups the clause's rows for the
//! remove-only `<>` repair — diagnosis must still offer this clause as a `<>` to drop
//! rather than as a bound the user can nudge. A row dropped as a tautology allocates
//! nothing at all.
static void EmitNotEqualRows(SolverInput &input, const VarIndexer &indexer, const EvaluatedConstraint &ec,
                             NECollapse collapse, const string &label) {
	auto &records = ec.records;
	for (idx_t e = 0; e < records.Count(); e++) {
		double k = records.bounds[e];
		if (!NEIsIntegerValuedRhs(k)) {
			continue; // a bound that is not a whole number: every integer LHS differs from it
		}

		// A per-row clause holds no data-only term: its data is on the bound side already.
		vector<int> indices;
		vector<double> coefficients;
		for (idx_t t = records.term_begin[e]; t < records.term_begin[e + 1]; t++) {
			indices.push_back(static_cast<int>(indexer.Get(records.variables[t], records.rows[t])));
			coefficients.push_back(records.coefficients[t]);
		}

		// Each row's disjunction gets its own binary, allocated HERE — by the pass that
		// knows whether this row has a disjunction at all. Stage 05 used to allocate a
		// row-scoped one per data row before the range collapse could be computed, so a
		// clause that collapsed, or dropped as a tautology, or turned out to be an
		// aggregate (which needs a global per group instead), left a column per row that
		// nothing referenced.
		idx_t z_col = AddGlobalBinaryAux(input, indexer, 0.0, label);

		// The provenance every shape of this clause carries. `indicator_col` is both the
		// marker that says "remove-only `<>`" and the key that groups the clause's rows.
		auto stamp = [&](SolverInput::RawConstraint &rc) {
			rc.provenance.kind = ConstraintKind::USER_MECHANISM;
			rc.provenance.shape = ElasticShape::PER_ROW_DATA;
			rc.provenance.origin = ec.provenance.origin;
			rc.provenance.indicator_col = z_col;
			rc.StampGroup(ec, records.groups[e]);
		};

		if (collapse != NECollapse::DISJUNCTION) {
			bool lower = collapse == NECollapse::LOWER_ONLY;
			SolverInput::RawConstraint rc;
			rc.indices = std::move(indices);
			rc.coefficients = std::move(coefficients);
			rc.sense = lower ? '<' : '>';
			rc.rhs = lower ? k - 1.0 : k + 1.0;
			stamp(rc);
			input.global_constraints.push_back(std::move(rc));
			continue;
		}

		// Both halves carry the clause's provenance ON THE ROW, which is why `<>` is
		// stated as a conditional row rather than as a general constraint: a general
		// constraint has no row for diagnosis to reach, and dropping the clause is the
		// only repair a `<>` has.
		// The row's terms are taken by value so the half that no longer needs them can
		// hand them straight over: only the first half copies.
		auto emit = [&](vector<int> row_indices, vector<double> row_values, int binval, char sense, double rhs) {
			SolverInput::IndicatorConstraintSpec ic;
			ic.binary_column = static_cast<int>(z_col);
			ic.binary_value = binval;
			ic.row.indices = std::move(row_indices);
			ic.row.coefficients = std::move(row_values);
			ic.row.sense = sense;
			ic.row.rhs = rhs;
			stamp(ic.row);
			input.indicator_constraints.push_back(std::move(ic));
		};
		// z = 0  =>  LHS <= K - 1
		emit(indices, coefficients, 0, '<', k - 1.0);
		// z = 1  =>  LHS >= K + 1. Last use of the row's terms.
		emit(std::move(indices), std::move(coefficients), 1, '>', k + 1.0);
	}
}

static void ExpandAggregateNotEqual(SolverInput &input, const VarIndexer &var_indexer,
                                    const vector<const NotEqualClause *> &deferred_aggregate,
                                    const vector<string> &var_names);

void LinearizeNotEqual(SolverInput &input, const VarIndexer &indexer, const vector<string> &var_names) {

	// The aggregate spelling is collected on this walk and finished below: it needs one
	// global binary per GROUP, which the per-row loop has no notion of.
	vector<const NotEqualClause *> deferred_aggregate;
	for (auto &clause : input.not_equals) {
		auto &ec = clause.row;
		if (ec.lhs_is_aggregate) {
			// Aggregate NE: defer to after var_indexer is built. Expanded with a
			// single global z per group, whose Big-M comes from that group's SUMMED
			// range — a single per-row bound would be far too small. The per-group
			// integer-RHS check (for AVG <> where the rescaled K*N_g may or may not
			// be integer) lives in the deferred expansion; we don't filter here.
			deferred_aggregate.push_back(&clause);
			continue;
		}

		// Per-row NE. With an integer LHS (the bind-time type gate) a bound that is not a whole number is a tautology,
		// so such a row is passed over, and a clause of such rows alone emits nothing.
		//
		// Range collapse, decided before anything is emitted. When the LHS cannot reach
		// the far side of K, one disjunct is dead and the clause is a plain inequality.
		NECollapse collapse =
		    ClassifyNEConstraint(ec, input.rigid_lower_bounds, input.rigid_upper_bounds, InstanceBox {input, indexer});
		if (collapse == NECollapse::ALWAYS_TRUE) {
			continue; // excludes nothing reachable — drop, like the tautology case
		}
		EmitNotEqualRows(input, indexer, ec, collapse, NotEqualClauseLabel(input, ec));
	}

	ExpandAggregateNotEqual(input, indexer, deferred_aggregate, var_names);
	// Every `<>` is expanded: what is left of them is the rows and binaries above.
	input.not_equals.clear();
}

//! The AGGREGATE spelling of `<>`. It cannot expand against the row-scoped indicator
//! the per-row spelling uses: it needs one *global* binary per group, and the group's
//! Big-M must cover the summed range over its rows rather than a single row's, which a
//! per-row bound would silently cap. The indicator is named by the clause as the user
//! wrote it (NotEqualClauseLabel), so a dropped aggregate `<>` can be named in a repair.
static void ExpandAggregateNotEqual(SolverInput &input, const VarIndexer &var_indexer,
                                    const vector<const NotEqualClause *> &deferred_aggregate,
                                    const vector<string> &var_names) {
	if (deferred_aggregate.empty()) {
		return;
	}
	const InstanceBox floors {input, var_indexer};
	for (auto *clause : deferred_aggregate) {
		const auto &ec = clause->row;
		auto &records = ec.records;
		const auto written = DecideWrittenClause(input, ec.provenance.origin.source_clause_id);
		// The group's LHS, one row the clause emits (ClauseEmittedRows): a group of a grouped clause, the whole
		// clause for an ungrouped one.
		auto &emitted = ClauseEmittedRows(ec, var_indexer, written);

		// I4 (aggregate `<>`): clause text used to name a dropped aggregate `<>`, carried
		// onto every global z this `ec` allocates so the removal dial can label the edit.
		string ne_label = NotEqualClauseLabel(input, ec);

		for (idx_t g = 0; g < records.Count(); g++) {
			// Every value this group's bound takes (NotEqualClause::excluded): a `<>` bound does not collapse,
			// every excluded value is kept. The common case (a uniform bound, or no groups at all) is one value.
			const idx_t excluded_begin = clause->excluded_begin[g];
			const idx_t excluded_end = clause->excluded_begin[g + 1];

			// Range collapse, per group. Same reasoning as the per-row path, over the
			// group's summed interval: each row of a group contributes its own solver
			// column, so the group's reachable range is the sum of its rows' ranges.
			// Unlike the per-row path a mixed verdict costs nothing here, because groups
			// are already emitted independently — each gets the encoding its own range
			// earns. Neither the range nor the LHS accumulation below depends on which
			// excluded value is being tested, so both are computed once per group and
			// reused for every excluded value.
			// A term beside the reducers enters the group's row once: execution kept its coefficient on one row of
			// the group (CountOnce), so the per-row ranges count it once too.
			// The clause is linear: stage 05 refuses a `<>` over a product (RewriteNotEqual).
			auto range = DecideRowSignedRange(ec, g, input.rigid_lower_bounds, input.rigid_upper_bounds,
			                                  DConstants::INVALID_INDEX, &floors);
			double grp_lo = range.Lo();
			double grp_hi = range.Hi();

			// The group's LHS as a snapshot, copied for every excluded value's disjunction below (only the last
			// consumes it by move).
			auto row_begin = NumericCast<int64_t>(emitted.offsets[g]);
			auto row_end = NumericCast<int64_t>(emitted.offsets[g + 1]);
			vector<int> group_indices(emitted.columns.begin() + row_begin, emitted.columns.begin() + row_end);
			vector<double> group_coefs(emitted.values.begin() + row_begin, emitted.values.begin() + row_end);

			for (idx_t vi = excluded_begin; vi < excluded_end; vi++) {
				// The value less the fixed (decision-free) part of the LHS, which is the group's whatever the value.
				double rhs = clause->excluded[vi] - records.constants[g];

				// Integer-RHS guard: with integer LHS (the bind-time type gate) and a non-integer K,
				// `LHS <> K` is a tautology for this value — every integer LHS
				// satisfies it, so this exclusion alone is dropped, not the whole
				// group. The ±1 Big-M rewrite would wrongly cut floor(K) and ceil(K).
				// The predicate is shared with the per-row path: spelling the negation
				// inline here let an infinite K through, because `inf - round(inf)` is
				// NaN and every comparison against NaN is false — so the group reached
				// the Big-M below and built an infinite coefficient instead of dropping
				// as a tautology.
				if (!NEIsIntegerValuedRhs(rhs)) {
					continue;
				}

				NECollapse collapse = NECollapse::DISJUNCTION;
				if (grp_hi < rhs - 0.5 || grp_lo > rhs + 0.5) {
					collapse = NECollapse::ALWAYS_TRUE;
				} else if (grp_lo > rhs - 0.5) {
					collapse = NECollapse::UPPER_ONLY;
				} else if (grp_hi < rhs + 0.5) {
					collapse = NECollapse::LOWER_ONLY;
				}
				if (collapse == NECollapse::ALWAYS_TRUE) {
					continue; // this group's aggregate cannot reach this K — excludes nothing
				}

				// Allocate one global binary z per (group, excluded value). A collapsed
				// one still gets one, unreferenced by any row: it is what carries the
				// clause's label and groups its rows for the remove-only `<>` repair,
				// so allocating it keeps diagnosis identical whichever encoding the
				// value received. The per-row path is in the same position — its
				// indicator is allocated at stage 05, before any range is knowable.
				idx_t z_idx = AddGlobalBinaryAux(input, var_indexer, 0.0, ne_label);

				// Copy the group's LHS snapshot for every value but the last, which
				// consumes it by move — the same by-value handoff the per-row twin
				// above uses between its two halves.
				bool last_value = vi + 1 == excluded_end;
				vector<int> indices = last_value ? std::move(group_indices) : group_indices;
				vector<double> coefs = last_value ? std::move(group_coefs) : group_coefs;

				// The provenance both halves carry, whichever shape they end up in.
				auto stamp = [&](SolverInput::RawConstraint &rc) {
					rc.provenance.kind = ConstraintKind::USER_MECHANISM;
					rc.provenance.origin = ec.provenance.origin;
					rc.provenance.indicator_col = z_idx;
					rc.StampGroup(ec, records.groups[g]);
					rc.provenance.is_aggregate = true;
				};

				// Collapsed: one plain inequality, no indicator and no disjunction left.
				if (collapse != NECollapse::DISJUNCTION) {
					bool lower = collapse == NECollapse::LOWER_ONLY;
					SolverInput::RawConstraint rc;
					rc.sense = lower ? '<' : '>';
					rc.rhs = lower ? rhs - 1.0 : rhs + 1.0;
					rc.indices = std::move(indices);
					rc.coefficients = std::move(coefs);
					stamp(rc);
					input.global_constraints.push_back(std::move(rc));
					continue;
				}

				// The disjunction, as two conditional rows on this value's binary. The
				// summed Big-M that used to be computed here — and that is what forced a
				// finite bound on every contributing variable — is not computed at all now:
				// `LowerDecideConstructs` derives one per half, and only where the chosen
				// backend cannot state the condition itself.
				//
				// By value, as in the per-row twin above: only the first half copies the
				// snapshot, the second hands it over.
				auto emit = [&](vector<int> row_indices, vector<double> row_coefs, int binval, char sense,
				                double bound) {
					SolverInput::IndicatorConstraintSpec ic;
					ic.binary_column = static_cast<int>(z_idx);
					ic.binary_value = binval;
					ic.row.indices = std::move(row_indices);
					ic.row.coefficients = std::move(row_coefs);
					ic.row.sense = sense;
					ic.row.rhs = bound;
					stamp(ic.row);
					input.indicator_constraints.push_back(std::move(ic));
				};
				emit(indices, coefs, 0, '<', rhs - 1.0);
				// Last use of this value's snapshot.
				emit(std::move(indices), std::move(coefs), 1, '>', rhs + 1.0);
			}
		}
	}
}

} // namespace duckdb
