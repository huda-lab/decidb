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
//! Constraints are matched to their indicator variables via `minmax_clause_idx`
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

//! Encode every MIN/MAX *constraint* stage 05 tagged, routing each clause between the
//! two formulations `NativeConstructPolicy` describes. Both arms make the same bound
//! classification first — a vacuous or unreachable bound is settled by the direction it
//! points, not by an encoding — and then:
//!
//!   - **Lowered** (`MAX(e) >= K` -> `e_r - M*y_r >= K - M` per row, plus `SUM(y) >= 1`)
//!     rewrites the clause in place, in the per-row representation it arrived in.
//!   - **Native** states `z = MAX(t..)` for the backend: a column per member expression
//!     (or the member's own column, where the expression is nothing but one), one
//!     extremum column per group, and the user's bound as a single row over it. No Big-M,
//!     so no contributing variable needs a finite bound.
//!
//! Untagged constraints pass through unchanged.
void LinearizeMinMaxConstraints(SolverInput &input, const VarIndexer &indexer,
                                const vector<string> &var_names, NativeConstructPolicy policy);

//! Encode every constraint stage 05 tagged with a `<>` indicator as the disjunction it
//! is: `z == 0 => LHS <= K-1` and `z == 1 => LHS >= K+1`, a pair of CONDITIONAL ROWS.
//! That is the only spelling this emits. Whether the chosen backend states a condition
//! itself or needs it encoded with a Big-M is settled once, afterwards, by
//! `LowerDecideConstructs` — so no bound is asked for here and no backend is consulted.
//!
//! Both spellings of the clause are finished here. A per-row `<>` expands against the
//! row-scoped indicator stage 05 allocated; an aggregate one allocates a *global* binary
//! per group, because a group's rows sum onto one row and one row needs one binary.
//! `aux_var_expressions` supplies the clause text stage 05 recorded for the indicator, so
//! a dropped aggregate `<>` can be named in a repair.
//!
//! Both halves carry the clause's `indicator_col` on the row, so the infeasible removal
//! dial groups them into one droppable `<>`. That is why `<>` is stated as a conditional
//! ROW rather than as a general constraint, which carries no row for diagnosis to reach —
//! and dropping the clause is the only repair a `<>` has.
//!
//! Refuses a left-hand side that is not integer-valued — the ±1 band is only exact
//! on the integer lattice — and silently drops a comparison whose bound no integer
//! can equal, since every assignment already satisfies it.
void LinearizeNotEqual(SolverInput &input, const VarIndexer &indexer,
                       const vector<string> &var_names);

//! The one place a construct is lowered, and the last pass of stage 06.
//!
//! Every construct site above emits the SEMANTIC form and nothing else: a `<>` becomes a
//! pair of conditional rows whether or not any backend can state one. This pass reads
//! what the chosen backend declared — the answer stage 05 recorded on the plan, not a
//! fresh question to a backend — and rewrites whatever it cannot state into ordinary
//! rows.
//!
//! Lowering a conditional row is one rewrite: give it a Big-M term on its own binary,
//! sized so the row is exactly slack when the condition is off. Everything a Big-M needs
//! — the row, and the box of every column in it — is already here, which is why this can
//! be one pass over the model rather than a second emitter inside every construct.
//!
//! Refuses, naming a column to bound, where no finite Big-M exists. That refusal belongs
//! HERE and only here: a construct the backend states needs no constant to dominate it,
//! so whether a contributing variable is bounded is a question about the lowering and not
//! about the query.
void LowerDecideConstructs(SolverInput &input, const VarIndexer &indexer,
                           const vector<string> &var_names,
                           const SolverConstructSupport &constructs);

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
//! and a `GeneralConstraintSpec` saying `aux = |t|`. Emitted in flat columns, which is
//! why every construct site takes the `VarIndexer`.
//!
//! Each `t` is boxed by ITS OWN row's reach, keeping each end it can derive
//! independently, exactly as `LinearizeMinMaxConstraints` boxes its argument columns.
//! A row whose contributors are all bounded gets a real box even when another row's are
//! not; a column is left free only where nothing is derivable at all, which is the query
//! this arm exists to answer.
//!
//! Called only when the chosen backend declared `SolverConstructSupport::abs`. The routing
//! is the gate's; this only translates.
void EmitNativeAbs(SolverInput &input, const VarIndexer &indexer);

//! The reachable range of a family of row expressions: the box every auxiliary over
//! that family is declared with, and — as `Span()` — the only Big-M an extremum link
//! over it may use. One object, because both come from the same walk over the data.
//!
//! Every continuous auxiliary stage 05 introduces stands for an extremum over such a
//! family, so its bounds are always derivable at the moment it is created. Returning
//! the range rather than a bare Big-M constant is what stops those endpoints from
//! being computed and then discarded, which used to leave every continuous auxiliary
//! declared `[-1e30, 1e30]` and the root LP with no box to work in.
//!
//! `lo`/`hi` INCLUDE constant terms, and there is deliberately no constant-free
//! counterpart. One existed until 2026-08-23 and was what extremum links scaled off,
//! on the reasoning that a constant cancels in the `(aux - expr)` difference a link row
//! slackens. It cancels within one row, not between two, so it was invalid wherever
//! rows carried different constants — see `Span()`. Both readings cannot coexist in one
//! object without the wrong one eventually being picked, so only the sound one is
//! representable.
//!
//! Both endpoints seed at 0. That only ever widens the box — it never cuts off a
//! reachable value — and it keeps the box consistent with the sites that pin an
//! empty auxiliary to exactly 0.
struct AuxRange {
    double lo = 0.0;
    double hi = 0.0;
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
    //! ends finite — every Big-M, which has to dominate the whole span. Boxing a
    //! column is the one caller entitled to read the per-side flags instead, because a
    //! half-open box is still a valid box.
    bool Unbounded() const { return lo_unbounded || hi_unbounded; }

    //! The full reach of this family, constants INCLUDED — and the only Big-M any
    //! extremum link may use. Defined ONLY when `!Unbounded()`: no constant dominates a
    //! range open at either end, so a caller must check `Unbounded()` and refuse before
    //! asking. There is no fallback value to return — that is the point. A half-open
    //! range is enough to box a column but not to scale a row, which is why boxing reads
    //! the per-side flags and this does not.
    //!
    //! Why constants are included: a link row deactivated on row r must stay slack up
    //! to `hi - exprmin_r`, and `hi` is whichever row in the family reaches highest — so
    //! row r's constant is measured against a DIFFERENT row's and does not cancel. It
    //! cancels only within one row's own `(aux - expr)`. Scaling off a constant-free
    //! reach instead, which is what this did until 2026-08-23, hands back a Big-M too
    //! small to be valid wherever rows carry different constants (`MAX(x + c)` with `c`
    //! a data column) — and a too-small Big-M cuts off legal answers rather than merely
    //! loosening the relaxation.
    double Span() const {
        D_ASSERT(!Unbounded());
        return hi - lo;
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

    //! Extend by one more row expression bracketed by [`row_lo`, `row_hi`].
    void CoverRow(double row_lo, double row_hi) {
        D_ASSERT(!std::isinf(row_lo) && !std::isinf(row_hi));
        lo = MinOf(lo, row_lo);
        hi = MaxOf(hi, row_hi);
    }

    //! `CoverRow` for a bracket that may be infinite on either end, as the signed walk
    //! over a row's terms returns. Each end is folded in or marked open on its own, so
    //! a row open on one side still contributes its closed side to the box. `var`
    //! names the variable to blame if a Big-M is later asked for over this family.
    void CoverRowSided(double row_lo, double row_hi, idx_t var) {
        if (std::isinf(row_lo)) {
            MarkLoUnbounded(var);
        } else {
            lo = MinOf(lo, row_lo);
        }
        if (std::isinf(row_hi)) {
            MarkHiUnbounded(var);
        } else {
            hi = MaxOf(hi, row_hi);
        }
    }

private:
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

//! One member of an extremum family: `(result - expr)` as a half-row — `MinMaxLinkRow`
//! already stores the expression negated, with its constant part separate — plus the
//! reach of `expr` on its own, which is the box a column pinned to it deserves.
struct ExtremumMember {
    MinMaxLinkRow link;
    AuxRange range;
};

//! `result = MIN/MAX(members)`, in flat columns. Every MIN/MAX auxiliary in the model is
//! one of these: the flat objective's `z`, a `PER` group's `z_g`, the outer `w` over
//! those or over group sums, and each composed term's `z_k`.
//!
//! Two independent sides, because which of them the surrounding model already supplies
//! differs by site and is not a property of the construct:
//!
//!   - The **envelope** (`result >= member` for MAX) holds `result` at or above every
//!     member. An objective that minimizes `result` supplies it for free.
//!   - The **closing** side pins `result` down onto some member: one Big-M row per
//!     member and a `SUM(y) >= 1` that makes one bind. An objective that maximizes
//!     `result` supplies it for free.
//!
//! An objective-side link needs exactly one of them, and which one is the easy/hard
//! classification stage 05 makes. A composed link sits in a *constraint*, where no
//! optimization pressure exists at all, so its hard direction needs both.
struct ExtremumLinkSpec {
    idx_t result_column = DConstants::INVALID_INDEX;
    bool is_max = false;
    bool need_envelope = false;
    bool need_closing = false;
    //! The closing Big-M, and whether one exists at all. Resolved by the CALLER, because
    //! the family a link reduces over is not always the family of its own members: an
    //! outer MIN/MAX over group SUMS spans the per-row reach times a group's row count,
    //! not the per-row reach. `blame_var` names the variable that left it open.
    double closing_big_m = 0.0;
    bool closing_underivable = false;
    idx_t blame_var = DConstants::INVALID_INDEX;
    //! Names every column and binary this emits, through the global label channel, so a
    //! diagnosis renders `MAX(x)` rather than an internal column.
    string label;
    //! The two sides can carry different provenance. A composed clause's closing rows
    //! ARE the user's clause and are loosenable; its envelope is mechanism, and an
    //! objective's linking rows are structural throughout.
    ConstraintKind envelope_kind = ConstraintKind::STRUCTURAL;
    ConstraintKind closing_kind = ConstraintKind::STRUCTURAL;
};

//! Emit one extremum link, in whichever form the policy and the spec call for. This is
//! the ONLY place a MIN/MAX auxiliary is pinned, and the only place that three-way
//! choice is made: it used to be written out at each of the five sites above.
//!
//! The envelope, where the spec asks for it, is emitted either way — it costs one row
//! per member, needs no constant to dominate anything, and is implied by the general
//! constraint on the native arm. The closing side is where the two formulations part:
//! `result = MIN/MAX(cols)` stated for the backend, or the Big-M family that encodes it.
//! `members` is consumed.
void EmitExtremumLink(SolverInput &input, const VarIndexer &indexer,
                      const ExtremumLinkSpec &spec, vector<ExtremumMember> &members,
                      const vector<string> &var_names, NativeConstructPolicy policy);

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
