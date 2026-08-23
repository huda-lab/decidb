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

#include "duckdb/decidb/ilp_model.hpp"
#include "duckdb/planner/decide/decide_prepared_model.hpp"

#include <cmath>
#include <unordered_map>

namespace duckdb {

//! Worst-case absolute contribution of row `r`'s decision variables:
//! sum over terms of |coef[t][r]| * max(|lb|,|ub|). Constant terms
//! (var == INVALID_INDEX, folded into the RHS by callers) are skipped. If any
//! contributing variable lacks a finite bound, `has_unbounded` is set and that term
//! is omitted — the caller must then REFUSE, not substitute a constant. There is no
//! fixed Big-M anywhere in DeciDB: a constant the true range exceeds does not fail,
//! it silently cuts the feasible region and returns a confidently wrong answer.
double DecideRowTermRange(const vector<idx_t> &variable_indices,
                          const vector<CoefficientColumn> &row_coefficients,
                          idx_t row, const vector<double> &lower_bounds,
                          const vector<double> &upper_bounds,
                          bool &has_unbounded,
                          idx_t skip_idx = DConstants::INVALID_INDEX);

//! Tight scalar Big-M for a per-row indicator constraint: the maximum over
//! active rows of |rhs[r]| + (worst-case row contribution), plus a 1-unit margin
//! that covers the integer-step band of the `<>` rewrite (harmless slack for the
//! MIN/MAX rewrites).
//!
//! **Throws** when a contributing variable has no finite bound. No constant dominates
//! an unbounded range, so there is no M to compute — only a guess, and a guess the
//! true range exceeds silently cuts the feasible region rather than failing. The
//! refusal names a column to bound, and it is the same refusal `LinearizeAbsMaximize`
//! has always made. `var_names` supplies that name.
double DecideTightPerRowBigM(const EvaluatedConstraint &ec,
                             const vector<double> &lower_bounds,
                             const vector<double> &upper_bounds,
                             idx_t num_rows,
                             const vector<string> &var_names);

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
//! (not positionally). Untagged constraints pass through unchanged, so
//! `input.constraints` is replaced in place by the encoded list. A group whose bound
//! is unreachable in its own direction is emitted as a plain per-row constraint
//! instead, and a group whose bound every assignment satisfies is dropped.
void LinearizeMinMaxIndicators(SolverInput &input, const vector<string> &var_names);

//! Whether a construct is stated natively or lowered, and the rule that decides.
//!
//! A general constraint (`z = MAX(t..)`) and a Big-M family encode the same thing, so
//! where both are valid this is a pure performance question — and the answer, measured,
//! is the lowering. Two reasons, both structural:
//!
//!   - A general constraint relates COLUMNS, so every member expression that is not
//!     already a column has to be pinned to a fresh one: an extra column and an extra
//!     equality row per data row, which presolve cannot substitute away because a
//!     general constraint reads them.
//!   - `z = MAX(t..)` is an EQUALITY, so the backend expands both directions. The
//!     lowering emits only the direction the clause needs.
//!
//! Measured on the Q9 benchmark shape (MAXIMIZE MAX over 5K-30K rows) the native arm ran
//! 1.7x-3.3x slower; on a `MAX(e) >= K` constraint at 30K rows, 40x slower and growing
//! with row count while the lowering stayed flat. Neither arm branched, so there was no
//! search quality to buy back: the Big-M here is per-row tight.
//!
//! So native is the FALLBACK, not the default — reserved for the case that has no valid
//! Big-M at all, which is the case it was built to answer and the only one where the
//! lowering must refuse the query.
struct NativeConstructPolicy {
    //! The backend declares this construct (stage 05, read off its capabilities).
    bool available = false;
    //! Test-only `DECIDB_NATIVE_CONSTRUCTS=force`: state it natively wherever it is
    //! declared, so the A/B equivalence tests still have two arms to compare.
    bool forced = false;

    //! The rule. `big_m_underivable` is this SITE's question, not the query's: one
    //! clause can have a derivable range while another in the same query does not.
    bool Use(bool big_m_underivable) const { return available && (forced || big_m_underivable); }
};

//! The native arm of the MIN/MAX gate, in two halves for the same reason the
//! aggregate `<>` is: a general constraint names flat columns, which exist only once
//! the VarIndexer does.
//!
//! Extract lifts every tagged MIN/MAX constraint out of `input.constraints` — it has
//! to, because until an arm rewrites it the row reads as `SUM(inner) <op> K` while the
//! clause means `MAX(inner) <op> K`, and anything walking the model in between would
//! believe the row. Both arms run the same bound classification first.
void ExtractNativeMinMaxConstraints(SolverInput &input, vector<EvaluatedConstraint> &deferred,
                                    NativeConstructPolicy policy);

//! Expand finishes them: a free column per active row pinned to that row's inner
//! expression, one extremum column per group pinned by a `MIN`/`MAX` general
//! constraint, and the user's own bound as a single row over that extremum. No Big-M,
//! no indicators, and therefore no requirement that any contributing variable be
//! bounded.
void ExpandNativeMinMaxConstraints(SolverInput &input, const VarIndexer &indexer,
                                   vector<EvaluatedConstraint> &deferred);

//! Encode every constraint stage 05 tagged with a `<>` indicator as the disjunctive
//! Big-M pair `x - M*z <= K-1` / `x - M*z >= K+1-M`.
//!
//! Per-row spellings are expanded in place with the row-scoped indicator. Aggregate
//! spellings cannot be: they need one *global* binary per group, and the group's
//! Big-M must cover the summed range rather than a single row's, so they are moved
//! into `deferred_aggregate` and finished by `ExpandDeferredAggregateNotEqual` once
//! the `VarIndexer` exists.
//!
//! Refuses a left-hand side that is not integer-valued — the ±1 band is only exact
//! on the integer lattice — and silently drops a comparison whose bound no integer
//! can equal, since every assignment already satisfies it.
void LinearizeNotEqual(SolverInput &input, vector<EvaluatedConstraint> &deferred_aggregate,
                       const vector<string> &var_names, bool native_not_equal,
                       vector<EvaluatedConstraint> &deferred_native);

//! The native arm of the `<>` gate: each row's disjunction as two implications,
//! `z == 0 => LHS <= K-1` and `z == 1 => LHS >= K+1`, instead of a Big-M pair. No
//! constant to dominate the row, so no contributing variable needs a finite bound.
//!
//! Deferred like every native emission — an indicator constraint names flat columns.
//! Both halves keep the clause's `indicator_col`, so the infeasible removal dial still
//! groups them into one droppable `<>`; that is why `<>` is expressed as indicator
//! constraints rather than as a general constraint, which carries no row for diagnosis
//! to reach.
void ExpandNativeNotEqual(SolverInput &input, const VarIndexer &indexer,
                          vector<EvaluatedConstraint> &deferred_native);

//! Finish the aggregate `<>` spellings `LinearizeNotEqual` deferred, one global
//! binary per non-empty group, emitting into `input.global_constraints` in flat
//! column space. `aux_var_expressions` supplies the clause text stage 05 recorded
//! for the indicator, so a dropped aggregate `<>` can be named in a repair.
void ExpandDeferredAggregateNotEqual(SolverInput &input, const VarIndexer &var_indexer,
                                     vector<EvaluatedConstraint> &deferred_aggregate,
                                     const vector<pair<idx_t, string>> &aux_var_expressions,
                                     const vector<string> &var_names, bool native_not_equal);

//! Emit the McCormick envelope for every `w = b * x` link. For `x >= 0` the lower
//! corner is implied by `w`'s own non-negative bound and the upper corner collapses
//! to the plain structural `w <= x`, so three rows suffice; for `x < 0` all four
//! corners are emitted and `w`'s lower bound is widened so the product can reach the
//! negative value. Requires a finite upper bound on `x` and names it if missing.
void LinearizeBilinear(SolverInput &input, const vector<string> &var_names);

//! Phase 1 of the ABS auxiliary formulation, and it runs before EVERY other
//! linearizer. For each `abs_maximize_links` entry it derives the largest |inner| any
//! row can reach and narrows the auxiliary's column box to it — the only place that
//! box is ever derived, since `aux >= inner` / `aux >= -inner` bound it from below
//! only. Every other linearizer computes its Big-M from column boxes, so an auxiliary
//! they read has to be boxed by the time they run.
//!
//! `refuse_when_unbounded` is the gate's answer for this construct. On the lowering
//! path it is true: no finite Big-M exists over an unbounded contributor, and the
//! query is refused naming a column to bound. On the native path it is false — a
//! general constraint needs no Big-M, so the auxiliary is simply left unboxed and the
//! query answers. That divergence is the capability's whole payoff.
void DeriveAbsAuxiliaryBounds(SolverInput &input, const vector<string> &var_names,
                              bool refuse_when_unbounded);

//! Phase 2, LOWERING path: the Big-M sign-indicator rows that pin `aux = |inner|`.
//! Reads the range DeriveAbsAuxiliaryBounds already derived; never recomputes it.
void LinearizeAbsMaximize(SolverInput &input);

//! Phase 2, NATIVE path: one column `t` per active row, an equality row `t = inner`,
//! and a `GeneralConstraintSpec` saying `aux = |t|`. Emitted in flat columns, so it runs
//! once the VarIndexer exists — the same phase as ExpandDeferredAggregateNotEqual, and
//! for the same reason.
//!
//! Each `t` is boxed by ITS OWN row's reach, keeping each end it can derive
//! independently, exactly as ExpandNativeMinMaxConstraints boxes its argument columns.
//! A row whose contributors are all bounded gets a real box even when another row's are
//! not; a column is left free only where nothing is derivable at all, which is the query
//! this arm exists to answer.
//!
//! Called only when the chosen backend declared `SolverConstructSupport::abs`. The routing
//! is the gate's; this only translates.
void EmitNativeAbs(SolverInput &input, const VarIndexer &indexer);

//! The reachable range of a family of row expressions, and the coefficient spread a
//! Big-M row needs — one object, because they come from the same walk over the data.
//!
//! Every continuous auxiliary stage 05 introduces stands for an extremum over such a
//! family, so its bounds are always derivable at the moment it is created. Returning
//! the range rather than a bare Big-M constant is what stops those endpoints from
//! being computed and then discarded, which used to leave every continuous auxiliary
//! declared `[-1e30, 1e30]` and the root LP with no box to work in.
//!
//! `lo`/`hi` INCLUDE constant terms: an auxiliary is pinned against the whole
//! expression, constant and all, so its box has to contain them. `spread` EXCLUDES
//! them, because a constant cancels in the `(aux - expr)` difference a Big-M row
//! slackens. Keeping the two separate is what lets bounds tighten without perturbing
//! any Big-M value the linearizer already emits.
//!
//! Both endpoints seed at 0. That only ever widens the box — it never cuts off a
//! reachable value — and it keeps the box consistent with the sites that pin an
//! empty auxiliary to exactly 0.
struct AuxRange {
    double lo = 0.0;
    double hi = 0.0;
    double spread = 0.0;
    //! The two ends are tracked separately because they fail separately. A decision
    //! variable declared `x >= 0` with no ceiling leaves `hi` underivable and `lo`
    //! perfectly well known, and a box keeping the closed side is strictly better than
    //! one discarding both — the root simplex gets half a region to start from instead
    //! of none. `lo`/`hi` are meaningful iff the matching flag is false.
    bool lo_unbounded = false;
    bool hi_unbounded = false;
    //! The first variable that opened either end, kept so a refusal can name a column
    //! the user can bound rather than say only that something was unbounded.
    //! Meaningful iff `Unbounded()`.
    idx_t unbounded_var = DConstants::INVALID_INDEX;

    //! True when EITHER end is open. This is the test for everything that needs both
    //! ends finite — every Big-M, which has to dominate the whole spread. Boxing a
    //! column is the one caller entitled to read the per-side flags instead, because a
    //! half-open box is still a valid box.
    bool Unbounded() const { return lo_unbounded || hi_unbounded; }

    //! The Big-M constant for this family. Defined ONLY when `!Unbounded()`: no
    //! constant dominates a range open at either end, so a caller must check
    //! `Unbounded()` and refuse before asking. There is no fallback value to return —
    //! that is the point. A half-open range is enough to box a column but not to
    //! scale a row, which is why boxing reads the per-side flags and this does not.
    double BigM() const {
        D_ASSERT(!Unbounded());
        return spread;
    }

    //! Record an unbounded contributor. Keeps the FIRST one seen, so the message a
    //! query produces does not depend on row order.
    void MarkUnbounded(idx_t var) {
        MarkLoUnbounded(var);
        MarkHiUnbounded(var);
    }

    //! Record that one end alone is underivable, keeping whatever the other end holds.
    void MarkLoUnbounded(idx_t var) {
        if (!Unbounded()) {
            unbounded_var = var;
        }
        lo_unbounded = true;
    }
    void MarkHiUnbounded(idx_t var) {
        if (!Unbounded()) {
            unbounded_var = var;
        }
        hi_unbounded = true;
    }

    //! Widen to also cover `other` — an extremum taken over several families.
    void Cover(const AuxRange &other) {
        lo = MinOf(lo, other.lo);
        hi = MaxOf(hi, other.hi);
        spread = MaxOf(spread, other.spread);
        // The blame travels with the openness. Merging the flags without the variable
        // leaves a range that reports `Unbounded()` but has no column to name, so a
        // refusal over the merged family degrades to the generic "one of them is
        // unbounded" message. Same rule as the `Mark*` helpers: keep the FIRST open end
        // seen, so the message does not depend on the order families were merged in.
        if (!Unbounded() && other.Unbounded()) {
            unbounded_var = other.unbounded_var;
        }
        lo_unbounded = lo_unbounded || other.lo_unbounded;
        hi_unbounded = hi_unbounded || other.hi_unbounded;
    }

    //! Extend by one more row expression bracketed by [`row_lo`, `row_hi`], whose
    //! variable-only part (constants excluded) is bracketed by [`var_lo`, `var_hi`].
    void CoverRow(double row_lo, double row_hi, double var_lo, double var_hi) {
        D_ASSERT(!std::isinf(row_lo) && !std::isinf(row_hi));
        lo = MinOf(lo, row_lo);
        hi = MaxOf(hi, row_hi);
        var_low = MinOf(var_low, var_lo);
        var_high = MaxOf(var_high, var_hi);
        spread = var_high - var_low;
    }

    //! `CoverRow` for a bracket that may be infinite on either end, as the signed walk
    //! over a row's terms returns. Each end is folded in or marked open on its own, so
    //! a row open on one side still contributes its closed side to the box. `var`
    //! names the variable to blame if a Big-M is later asked for over this family.
    //!
    //! `spread` is only ever read through `BigM()`, which refuses unless both ends are
    //! closed, so a partial `spread` left behind by a one-sided row is unreachable.
    void CoverRowSided(double row_lo, double row_hi, double var_lo, double var_hi, idx_t var) {
        if (std::isinf(row_lo)) {
            MarkLoUnbounded(var);
        } else {
            lo = MinOf(lo, row_lo);
            var_low = MinOf(var_low, var_lo);
        }
        if (std::isinf(row_hi)) {
            MarkHiUnbounded(var);
        } else {
            hi = MaxOf(hi, row_hi);
            var_high = MaxOf(var_high, var_hi);
        }
        spread = var_high - var_low;
    }

private:
    //! Running variable-only extremes behind `spread`. Held separately so `spread`
    //! stays exactly the value the pre-existing Big-M walk produced.
    double var_low = 0.0;
    double var_high = 0.0;

    static double MinOf(double a, double b) { return a < b ? a : b; }
    static double MaxOf(double a, double b) { return a > b ? a : b; }
};

//! Accumulator for a MIN/MAX linking row (`z - expr op bound`).
//!
//! DecideTerm arrays are indexed by term, not by variable, so the same solver column
//! reaches one row more than once in two situations: `(c + 1) * x` distributes
//! into `c*x + 1*x`, and an entity-scoped or scalar variable resolves to a
//! single column across every row it spans. A repeated column index is rejected
//! outright by both Gurobi and HiGHS, so coefficients are summed per column here
//! rather than pushed per term. Constant terms carry no column at all
//! (`variable_index == INVALID_INDEX`) and collect into `constant`, which the
//! caller folds into the bound: `z <= expr + k` is `z - expr <= k`.
//!
//! Columns keep first-appearance order — emitting straight from the hash map
//! would hand the solver a different matrix ordering run to run.
struct MinMaxLinkRow {
    vector<int> indices;
    vector<double> coefficients;
    double constant = 0.0;
    std::unordered_map<int, idx_t> column_slot;

    void AddColumn(int column, double coefficient) {
        auto entry = column_slot.find(column);
        if (entry == column_slot.end()) {
            column_slot.emplace(column, indices.size());
            indices.push_back(column);
            coefficients.push_back(coefficient);
        } else {
            coefficients[entry->second] += coefficient;
        }
    }

    //! True when no column survives accumulation — either nothing was added, or
    //! every column's terms cancelled (`c*x - c*x`). Such a row constrains the
    //! auxiliary against `constant` alone.
    bool HasNoColumns() const {
        for (auto coefficient : coefficients) {
            if (std::abs(coefficient) >= 1e-15) {
                return false;
            }
        }
        return true;
    }

    void AppendTo(SolverInput::RawConstraint &rc) const {
        for (idx_t i = 0; i < indices.size(); i++) {
            if (std::abs(coefficients[i]) < 1e-15) {
                continue;
            }
            rc.indices.push_back(indices[i]);
            rc.coefficients.push_back(coefficients[i]);
        }
    }
};

//! How stage 05 classified the objective's MIN/MAX shape, as `LinearizeMinMaxObjective`
//! needs it. `flat_*` describes an unqualified `MIN(expr)` / `MAX(expr)` objective;
//! `per_*` describes the two-level `OUTER(INNER(expr)) PER key` spelling, whose group
//! ids arrive on `SolverInput::objective_row_group_ids`. An objective is *easy* when the
//! optimization direction already drives the auxiliary to the extreme (MINIMIZE+MAX,
//! MAXIMIZE+MIN) and one envelope row per row suffices; the hard direction additionally
//! needs the per-row indicator layer. `when_mask` is the evaluated `WHEN` filter and is
//! read only when `has_when`.
struct MinMaxObjectiveSpec {
    ObjectiveAggregateType flat_agg = ObjectiveAggregateType::NONE;
    bool flat_is_easy = false;
    ObjectiveAggregateType per_inner_agg = ObjectiveAggregateType::NONE;
    ObjectiveAggregateType per_outer_agg = ObjectiveAggregateType::NONE;
    bool per_inner_is_easy = false;
    bool per_outer_is_easy = false;
    //! Inner reducer was AVG, rewritten to SUM with a per-group 1/n_g scale.
    bool per_inner_was_avg = false;
    bool has_when = false;
    vector<bool> when_mask;
};

//! Encode a MIN/MAX *objective* — the counterpart of `LinearizeMinMaxIndicators`, which
//! encodes a MIN/MAX *constraint*. The per-row objective is replaced by a global
//! auxiliary (`z` flat, `z_g` per group plus an outer `w` for the nested `PER` spelling),
//! pinned by envelope rows against the evaluated objective coefficients; the hard
//! direction adds one indicator binary per active row and a `SUM(y) >= 1` pin.
//!
//! Runs after the flat column space exists, so it emits into `input.global_constraints`
//! and appends its auxiliaries to the global block. Untouched when the objective carries
//! no MIN/MAX aggregate: `input.objective_coefficients` is then left as it arrived.
void LinearizeMinMaxObjective(SolverInput &input, const VarIndexer &indexer,
                              const MinMaxObjectiveSpec &spec, const vector<string> &var_names,
                              NativeConstructPolicy native_min_max);

//! One reducer term of a *composed* (additive) MIN/MAX clause — `SUM(a) + MAX(b) <= K`
//! or the objective spelling — with everything data-dependent already evaluated by the
//! physical layer. `inner_terms` is non-owning: the flattened terms live on the logical
//! operator's `ComposedMinMaxTerm`, prepared at stage 05, and `per_term_coefs[t][row]`
//! is that term's evaluated coefficient. `z_idx` is filled in by the linearizer.
struct ComposedMinMaxTermData {
    //! MIN/MAX reducer (gets a global auxiliary) vs SUM/AVG (folds into the outer row).
    bool is_minmax = false;
    //! Lowercase reducer name: "min", "max", "sum" or "avg".
    string agg_name;
    //! Sign this term carries in the additive composition.
    int sign = 1;
    //! Factor the canonicalizer peeled off this reducer, already evaluated. It stays
    //! outside the aggregate all the way to here, which is what makes its sign
    //! irrelevant to correctness.
    double scale = 1.0;
    //! True when the optimization/comparison direction already drives the auxiliary to
    //! the extremum, so the one-sided envelope pin suffices and no indicators are needed.
    bool is_easy = true;
    //! Rows this term reduces over: the term's `WHEN` mask, folded with the relation
    //! qualifier's de-duplication mask. Sized `SolverInput::num_rows`.
    vector<bool> filter_mask;
    //! Non-owning; the flattened inner terms live on the logical operator.
    const vector<DecideTerm> *inner_terms = nullptr;
    //! Evaluated per-row coefficient of each inner term, already scaled by its sign.
    vector<vector<double>> per_term_coefs;
    //! Flat column of this term's global auxiliary; assigned by the linearizer.
    idx_t z_idx = DConstants::INVALID_INDEX;
    //! User source text (`MAX(x)`) naming this term's global auxiliary in diagnostics.
    string label;
};

//! Encode one composed MIN/MAX *constraint*: a global auxiliary and its pinning rows per
//! MIN/MAX term, then one outer row summing the auxiliaries and the SUM/AVG terms against
//! `rhs_val`. `terms` is mutated in place — each MIN/MAX term's `z_idx` is filled in.
void LinearizeComposedMinMaxConstraint(SolverInput &input, const VarIndexer &indexer,
                                       vector<ComposedMinMaxTermData> &terms, double rhs_val,
                                       ExpressionType outer_cmp, idx_t source_clause_id,
                                       const vector<string> &var_names, NativeConstructPolicy native_min_max);

//! Encode a composed MIN/MAX *objective*: the same auxiliary layer, but the composition is
//! written into the objective — a coefficient on each auxiliary's column, and per-row
//! coefficients for the SUM/AVG terms. Replaces `input.objective_coefficients`.
void LinearizeComposedMinMaxObjective(SolverInput &input, const VarIndexer &indexer,
                                      vector<ComposedMinMaxTermData> &terms,
                                      const vector<string> &var_names, NativeConstructPolicy native_min_max);

} // namespace duckdb
