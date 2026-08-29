//===----------------------------------------------------------------------===//
//                         DecidB
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
#include <limits>
#include <unordered_map>
#include "duckdb/decidb/formulation/ilp_linearization_internal.hpp"

namespace duckdb {

using namespace decide_linearize; // NOLINT: internal DECIDE linearization helpers

//===--------------------------------------------------------------------===//
// `<>` disjunctions
//===--------------------------------------------------------------------===//

//! The ±1 band is only semantically exact when the LHS is integer-valued. For a REAL
//! variable or a non-integer coefficient the band `(K-1, K+1)` wrongly excludes
//! feasible continuous points. Mirrors the strict-inequality guard in
//! `ilp_model_builder.cpp::IsEvalConstraintLhsIntegerValued`, and splits its result the
//! same way: only one of the two failures is still the user's to fix here.
//!
//! A REAL decision is knowable from the declared type and is rejected at bind time by
//! `ValidateDecideNoIntegerStepComparisonOnReal`, which names the variable and quotes
//! the clause. Seeing one arrive here means a `<>` reached the model builder without
//! passing that gate — an invariant violation, not a query error. The branch is kept
//! rather than deleted so that a future rewrite introducing a REAL auxiliary inside a
//! `<>` fails loudly instead of silently cutting the band.
//!
//! A fractional coefficient comes from evaluating a data column and is knowable only
//! now, so it stays a user-facing refusal.
enum class NELhsIntegrality : uint8_t { INTEGER, REAL_VARIABLE, FRACTIONAL_COEFFICIENT };

static NELhsIntegrality NELhsIsIntegerValued(const EvaluatedConstraint &ec,
                                             const vector<LogicalType> &variable_types) {
    for (idx_t i = 0; i < ec.variable_indices.size(); i++) {
        idx_t vi = ec.variable_indices[i];
        if (vi == DConstants::INVALID_INDEX) {
            continue;
        }
        const auto &t = variable_types[vi];
        if (t == LogicalType::DOUBLE || t == LogicalType::FLOAT) {
            return NELhsIntegrality::REAL_VARIABLE;
        }
        if (!ec.row_coefficients[i].AllIntegral()) {
            return NELhsIntegrality::FRACTIONAL_COEFFICIENT;
        }
    }
    return NELhsIntegrality::INTEGER;
}

//! Companion check on the RHS. With integer-valued LHS and a non-integer K,
//! `LHS <> K` is a tautology (no integer can equal K). The ±1 Big-M rewrite would
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

//! Classify one row of a `<>` constraint. Only called once both integrality guards have
//! passed, so the LHS and `K` are both on the integer lattice and a half-unit tolerance
//! cleanly separates "can reach K" from "cannot".
static NECollapse ClassifyNERow(const EvaluatedConstraint &ec, idx_t row,
                                const vector<double> &lower_bounds,
                                const vector<double> &upper_bounds) {
    double lo;
    double hi;
    DecideRowSignedRange(ec, row, lower_bounds, upper_bounds, lo, hi);
    double k = DecideRowEffectiveBound(ec, row);
    if (!std::isfinite(k)) {
        return NECollapse::DISJUNCTION;
    }
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

//! The collapse verdict for a whole constraint, or DISJUNCTION if its active rows do not
//! agree on one.
//!
//! Per-row `<>` shares one EvaluatedConstraint across every row, so a per-row verdict
//! would mean splitting it into up to three constraints carrying complementary row masks.
//! Rows disagree only when their bounds or their RHS vary across rows, which is not the
//! shape the collapse exists to serve — a uniform `x <> 0` or `SUM(x) <> 0` yields one
//! verdict for every row. So a mixed constraint keeps the Big-M pair, unsplit.
static NECollapse ClassifyNEConstraint(const EvaluatedConstraint &ec, idx_t num_rows,
                                       const vector<double> &lower_bounds,
                                       const vector<double> &upper_bounds) {
    // A bilinear or quadratic LHS is not captured by variable_indices alone, so its
    // reachable range is not the linear one computed above.
    if (!ec.bilinear_terms.empty() || !ec.quadratic_groups.empty()) {
        return NECollapse::DISJUNCTION;
    }
    bool seen = false;
    NECollapse verdict = NECollapse::DISJUNCTION;
    for (idx_t r = 0; r < num_rows; r++) {
        if (!ec.row_group_ids.empty() && ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
            continue;
        }
        NECollapse row_verdict = ClassifyNERow(ec, r, lower_bounds, upper_bounds);
        if (!seen) {
            verdict = row_verdict;
            seen = true;
        } else if (row_verdict != verdict) {
            return NECollapse::DISJUNCTION;
        }
    }
    return seen ? verdict : NECollapse::DISJUNCTION;
}

//! The clause text stage 05 recorded for a `<>`, naming every binary it allocates so a
//! diagnosis renders `SUM(x) <> 0` rather than an internal column.
static string NotEqualClauseLabel(const SolverInput &input, const EvaluatedConstraint &ec) {
    if (ec.ne_clause_idx < input.ne_clause_labels.size()) {
        return input.ne_clause_labels[ec.ne_clause_idx];
    }
    return string();
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
static void EmitNotEqualRows(SolverInput &input, const VarIndexer &indexer,
                             const EvaluatedConstraint &ec, NECollapse collapse,
                             const string &label) {
    const idx_t num_rows = input.num_rows;
    bool has_groups = !ec.row_group_ids.empty();
    for (idx_t r = 0; r < num_rows; r++) {
        if (has_groups && ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
            continue; // masked out by WHEN/PER, or a non-integer bound on this row
        }
        double k = ec.rhs_values.Get(r);

        vector<int> indices;
        vector<double> coefficients;
        double constant = 0.0;
        for (idx_t t = 0; t < ec.variable_indices.size(); t++) {
            idx_t v = ec.variable_indices[t];
            double coef = ec.row_coefficients[t].Get(r);
            if (v == DConstants::INVALID_INDEX) {
                constant += coef; // constant LHS part folds into the bound
                continue;
            }
            if (coef == 0.0) {
                continue;
            }
            indices.push_back(static_cast<int>(indexer.Get(v, r)));
            coefficients.push_back(coef);
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
            rc.kind = ConstraintKind::USER_MECHANISM;
            rc.shape = ElasticShape::PER_ROW_DATA;
            rc.source_clause_id = ec.source_clause_id;
            rc.repair_group_id = ec.repair_group_id;
            rc.removal_group_id = ec.removal_group_id;
            rc.indicator_col = z_col;
            rc.group_key = has_groups ? ec.row_group_ids[r] : DConstants::INVALID_INDEX;
            // The folded LHS constant, so a report quotes the user's `K` and not the
            // bound this fold produced.
            rc.rhs_mechanism_offset = -constant;
        };

        if (collapse != NECollapse::DISJUNCTION) {
            bool lower = collapse == NECollapse::LOWER_ONLY;
            SolverInput::RawConstraint rc;
            rc.indices = std::move(indices);
            rc.coefficients = std::move(coefficients);
            rc.sense = lower ? '<' : '>';
            rc.rhs = (lower ? k - 1.0 : k + 1.0) - constant;
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
        auto emit = [&](vector<int> row_indices, vector<double> row_coefficients, int binval,
                        char sense, double rhs) {
            SolverInput::IndicatorConstraintSpec ic;
            ic.binary_column = static_cast<int>(z_col);
            ic.binary_value = binval;
            ic.row.indices = std::move(row_indices);
            ic.row.coefficients = std::move(row_coefficients);
            ic.row.sense = sense;
            ic.row.rhs = rhs;
            stamp(ic.row);
            input.indicator_constraints.push_back(std::move(ic));
        };
        // z = 0  =>  LHS <= K - 1
        emit(indices, coefficients, 0, '<', k - 1.0 - constant);
        // z = 1  =>  LHS >= K + 1. Last use of the row's terms.
        emit(std::move(indices), std::move(coefficients), 1, '>', k + 1.0 - constant);
    }
}

static void ExpandAggregateNotEqual(SolverInput &input, const VarIndexer &var_indexer,
                                    vector<EvaluatedConstraint> &deferred_aggregate,
                                    const vector<string> &var_names);

void LinearizeNotEqual(SolverInput &input, const VarIndexer &indexer,
                       const vector<string> &var_names) {
    const idx_t num_rows = input.num_rows;

    // The aggregate spelling is collected on this walk and finished below: it needs one
    // global binary per GROUP, which the per-row loop has no notion of.
    vector<EvaluatedConstraint> deferred_aggregate;
    vector<EvaluatedConstraint> new_constraints;
    for (auto &ec : input.constraints) {
        if (ec.ne_clause_idx == DConstants::INVALID_INDEX) {
            new_constraints.push_back(std::move(ec));
            continue;
        }
        if (NELhsIsIntegerValued(ec, input.variable_types) != NELhsIntegrality::INTEGER) {
            throw InternalException(
                "DECIDE: a '<>' whose left-hand side is not integer-valued reached the "
                "model builder. Both halves of that refusal are stated on declared types "
                "at bind time — ValidateDecideNoIntegerStepComparisonOnReal for the "
                "decision, ValidateDecideIntegralComparisonOperands for every other "
                "operand — so arriving here is an invariant violation, not a query error.");
        }
        if (ec.lhs_is_aggregate) {
            // Aggregate NE: defer to after var_indexer is built. Expanded with a
            // single global z per group, whose Big-M comes from that group's SUMMED
            // range — a single per-row bound would be far too small. The per-group
            // integer-RHS check (for AVG <> where the rescaled K*N_g may or may not
            // be integer) lives in the deferred expansion; we don't filter here.
            deferred_aggregate.push_back(ec); // copy before the loop moves on
            continue;                         // not added to new_constraints
        }

        // Per-row NE: expand inline with the row-scoped indicator variable.
        //
        // Integer-valued RHS guard. If RHS is uniform and non-integer, every row's
        // `LHS <> K` is a tautology — drop the whole constraint. If RHS varies per
        // row (e.g. correlated subquery), mask out only the non-integer rows by
        // marking them INVALID_INDEX in row_group_ids so the model builder skips
        // them. The remaining rows still get the real Big-M pair.
        if (ec.rhs_values.IsUniform()) {
            if (!NEIsIntegerValuedRhs(ec.rhs_values.UniformValue())) {
                continue; // drop ec entirely (tautology)
            }
        } else {
            // Build/extend a mask. row_group_ids may be empty (no WHEN/PER); in that
            // case materialize one initialised to group 0 so individual rows can be
            // excluded by setting INVALID_INDEX.
            if (ec.row_group_ids.empty()) {
                ec.row_group_ids.assign(num_rows, 0);
                ec.num_groups = 1;
            }
            idx_t dropped = 0;
            for (idx_t r = 0; r < num_rows; r++) {
                if (ec.row_group_ids[r] == DConstants::INVALID_INDEX) {
                    continue;
                }
                if (!NEIsIntegerValuedRhs(ec.rhs_values.Get(r))) {
                    ec.row_group_ids[r] = DConstants::INVALID_INDEX;
                    dropped++;
                }
            }
            if (dropped == num_rows) {
                continue; // every row is a tautology — drop the constraint
            }
        }

        // Range collapse, decided before anything is emitted. When the LHS cannot reach
        // the far side of K, one disjunct is dead and the clause is a plain inequality.
        NECollapse collapse = ClassifyNEConstraint(ec, num_rows, input.rigid_lower_bounds,
                                                   input.rigid_upper_bounds);
        if (collapse == NECollapse::ALWAYS_TRUE) {
            continue; // excludes nothing reachable — drop, like the tautology case
        }
        EmitNotEqualRows(input, indexer, ec, collapse, NotEqualClauseLabel(input, ec));
    }
    input.constraints = std::move(new_constraints);

    ExpandAggregateNotEqual(input, indexer, deferred_aggregate, var_names);
}

//! The AGGREGATE spelling of `<>`. It cannot expand against the row-scoped indicator
//! the per-row spelling uses: it needs one *global* binary per group, and the group's
//! Big-M must cover the summed range over its rows rather than a single row's, which a
//! per-row bound would silently cap. `aux_var_expressions` supplies the clause text
//! stage 05 recorded for the indicator, so a dropped aggregate `<>` can be named in a
//! repair.
static void ExpandAggregateNotEqual(SolverInput &input, const VarIndexer &var_indexer,
                                    vector<EvaluatedConstraint> &deferred_aggregate,
                                    const vector<string> &var_names) {
    if (deferred_aggregate.empty()) {
        return;
    }
    const idx_t num_rows = input.num_rows;

    // Reusable scratch for per-group LHS accumulation (replaces a per-group
    // unordered_map<int,double>). The decide-variable flat indices are bounded by
    // var_indexer.global_block_start, so the dense accumulator is sized to that —
    // tighter than total_vars and unaffected by the globals appended as we go.
    SparseCoeffAccumulator accum;
    {
        constexpr idx_t DENSE_CAP = 1u << 20;
        idx_t decide_var_index_span = var_indexer.global_block_start;
        if (decide_var_index_span <= DENSE_CAP) {
            accum.BeginDense(decide_var_index_span);
        } else {
            accum.BeginSparse(num_rows); // hint; per-group merging keeps actual size small
        }
    }

    for (auto &ec : deferred_aggregate) {
        bool has_groups = !ec.row_group_ids.empty();

        // I4 (aggregate `<>`): clause text used to name a dropped aggregate `<>`, carried
        // onto every global z this `ec` allocates so the removal dial can label the edit.
        string ne_label = NotEqualClauseLabel(input, ec);

        // Build group → rows mapping. For grouped constraints reuse the CSR index
        // already attached to ec; for ungrouped, materialize the trivial
        // single-group CSR locally.
        idx_t num_groups_to_process = 1;
        vector<idx_t> ungrouped_offsets;
        vector<idx_t> ungrouped_flat;
        const vector<idx_t> *offsets_ptr = nullptr;
        const vector<idx_t> *flat_ptr = nullptr;
        if (has_groups) {
            BuildGroupCSR(ec.row_group_ids, ec.num_groups, ec.group_offsets, ec.group_row_ids);
            num_groups_to_process = ec.num_groups;
            offsets_ptr = &ec.group_offsets;
            flat_ptr = &ec.group_row_ids;
        } else {
            ungrouped_offsets = {0, num_rows};
            ungrouped_flat.resize(num_rows);
            for (idx_t r = 0; r < num_rows; r++) {
                ungrouped_flat[r] = r;
            }
            offsets_ptr = &ungrouped_offsets;
            flat_ptr = &ungrouped_flat;
        }
        const auto &offsets = *offsets_ptr;
        const auto &flat_rows = *flat_ptr;

        for (idx_t g = 0; g < num_groups_to_process; g++) {
            idx_t g_begin = offsets[g];
            idx_t g_end = offsets[g + 1];
            if (g_begin == g_end) {
                continue;
            }
            idx_t g_size = g_end - g_begin;

            // The fixed (decision-free) part of the LHS does not depend on which
            // excluded value is being stated, so it is computed once per group.
            double fixed_offset = SumFixedAggregateLhsOffset(
                ec, &flat_rows, g_begin, g_end, "fixed aggregate <> term");

            // Every value this group's bound takes, deduplicated. `ReduceAggregate-
            // RhsPerGroup` (physical_decide.cpp) leaves a `<>` bound exactly as it
            // arrived when it varies, rather than collapsing it to one value the way
            // it does for `<=`/`>=` — standing decision 3: a `<>` bound does not
            // collapse, every excluded value is kept. The common case (a uniform
            // bound, or no PER at all) yields exactly one value here and this loop
            // runs once, identically to before this supported more than one.
            vector<double> distinct_rhs;
            for (idx_t k = g_begin; k < g_end; k++) {
                double raw = ec.rhs_values.Get(flat_rows[k]);
                if (std::find(distinct_rhs.begin(), distinct_rhs.end(), raw) == distinct_rhs.end()) {
                    distinct_rhs.push_back(raw);
                }
            }

            // Range collapse, per group. Same reasoning as the per-row path, over the
            // group's summed interval: each row of a group contributes its own solver
            // column, so the group's reachable range is the sum of its rows' ranges.
            // Unlike the per-row path a mixed verdict costs nothing here, because groups
            // are already emitted independently — each gets the encoding its own range
            // earns. Neither the range nor the LHS accumulation below depends on which
            // excluded value is being tested, so both are computed once per group and
            // reused for every value in `distinct_rhs`.
            const bool has_linear_range = ec.bilinear_terms.empty() && ec.quadratic_groups.empty();
            double grp_lo = 0.0;
            double grp_hi = 0.0;
            if (has_linear_range) {
                for (idx_t k = g_begin; k < g_end; k++) {
                    double row_lo;
                    double row_hi;
                    DecideRowSignedRange(ec, flat_rows[k], input.rigid_lower_bounds,
                                         input.rigid_upper_bounds, row_lo, row_hi);
                    grp_lo += row_lo; // -inf is absorbing, and only ever accumulates here
                    grp_hi += row_hi;
                }
            }

            // Accumulate LHS coefficients for active rows in this group.
            for (idx_t term_idx = 0; term_idx < ec.variable_indices.size(); term_idx++) {
                idx_t decide_var_idx = ec.variable_indices[term_idx];
                if (decide_var_idx == DConstants::INVALID_INDEX) {
                    continue;
                }
                auto &col = ec.row_coefficients[term_idx];
                for (idx_t k = g_begin; k < g_end; k++) {
                    idx_t row = flat_rows[k];
                    double coeff = col.Get(row);
                    if (std::abs(coeff) < 1e-15) {
                        continue;
                    }
                    int var_idx = static_cast<int>(var_indexer.Get(decide_var_idx, row));
                    accum.Add(var_idx, coeff);
                }
            }

            // Flush once into a deduped (idx, coeff) snapshot, copied for every
            // excluded value's disjunction below (only the last consumes it by move).
            vector<int> group_indices;
            vector<double> group_coefs;
            accum.Flush(group_indices, group_coefs);

            for (idx_t vi = 0; vi < distinct_rhs.size(); vi++) {
                // Base (unscaled) value. For AVG(x) <> K we store the original K in
                // rhs_values and multiply by the group size here.
                double rhs = distinct_rhs[vi];
                if (ec.ne_avg_rhs_scale) {
                    rhs *= static_cast<double>(g_size);
                }
                rhs -= fixed_offset;

                // Integer-RHS guard: with integer LHS (already enforced by
                // NELhsIsIntegerValued at deferral time) and a non-integer K,
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
                if (has_linear_range) {
                    if (grp_hi < rhs - 0.5 || grp_lo > rhs + 0.5) {
                        collapse = NECollapse::ALWAYS_TRUE;
                    } else if (grp_lo > rhs - 0.5) {
                        collapse = NECollapse::UPPER_ONLY;
                    } else if (grp_hi < rhs + 0.5) {
                        collapse = NECollapse::LOWER_ONLY;
                    }
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
                bool last_value = (vi + 1 == distinct_rhs.size());
                vector<int> indices = last_value ? std::move(group_indices) : group_indices;
                vector<double> coefs = last_value ? std::move(group_coefs) : group_coefs;

                // The provenance both halves carry, whichever shape they end up in.
                auto stamp = [&](SolverInput::RawConstraint &rc) {
                    rc.kind = ConstraintKind::USER_MECHANISM;
                    rc.source_clause_id = ec.source_clause_id;
                    rc.repair_group_id = ec.repair_group_id;
                    rc.removal_group_id = ec.removal_group_id;
                    rc.indicator_col = z_idx;
                    rc.group_key = has_groups ? g : DConstants::INVALID_INDEX;
                    rc.qualifier = ec.qualifier;
                    rc.is_aggregate = true;
                    if (has_groups && g < ec.group_labels.size()) {
                        rc.group_label = ec.group_labels[g];
                    }
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
                auto emit = [&](vector<int> row_indices, vector<double> row_coefs, int binval,
                                char sense, double bound) {
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
