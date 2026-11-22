//===----------------------------------------------------------------------===//
//                         DeciDB
//
// duckdb/decidb/formulation/ilp_linearization.hpp
//
// Stage 06 — linearization of the evaluated decision problem.
//
// A formulation chosen at stage 05 (a `<>` disjunction, a hard MIN/MAX, an ABS
// under MAXIMIZE, a bilinear product) reaches this stage as a *tag* on an
// `EvaluatedConstraint` (or, for an ABS, as a record of its shape): stage 05 recorded which encoding applies and
// stopped, because the rows that encode it and the constants that scale them are both
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

#include "duckdb/common/decide_numeric_policy.hpp"
#include "duckdb/decidb/formulation/ilp_model.hpp"
#include "duckdb/planner/decide/decide_tree_terms.hpp"

#include <cmath>
#include <functional>
#include <unordered_map>

namespace duckdb {

//! Worst-case absolute contribution of row `r`'s decision variables:
//! sum over terms of |coef[t][r]| * max(|lb|,|ub|). Constant terms
//! (var == INVALID_INDEX, folded into the RHS by callers) are skipped. If any
//! contributing variable lacks a finite bound, `has_unbounded` is set and that term
//! is omitted — the caller must then REFUSE, not substitute a constant. There is no
//! fixed Big-M anywhere in DeciDB: a constant the true range exceeds does not fail,
//! it silently cuts the feasible region and returns a confidently wrong answer.
double DecideRowTermRange(const vector<idx_t> &variable_indices, const vector<CoefficientColumn> &row_coefficients,
                          idx_t row, const vector<double> &lower_bounds, const vector<double> &upper_bounds,
                          bool &has_unbounded, const InstanceBox &instances);

//! Tight scalar Big-M for a per-row indicator constraint: the maximum over
//! active rows of |rhs[r]| + (worst-case row contribution), plus a 1-unit margin
//! that covers the integer-step band of the `<>` rewrite (harmless slack for the
//! MIN/MAX rewrites).
//!
//! **Throws** when a contributing variable has no finite bound. No constant dominates
//! an unbounded range, so there is no M to compute — only a guess, and a guess the
//! true range exceeds silently cuts the feasible region rather than failing. The
//! refusal names a column to bound, and it is the same refusal the lowered ABS
//! (`EmitAbsConstraints`) has always made. `var_names` supplies that name, and `construct` the text of the construct
//! that asked for the constant (`norm(e, 0)` for the links of a count of nonzeros), which the refusal quotes.
//!
//! `box` is the box the returned constant is valid for, and it is the caller's job to
//! pass the one the constant will be used against — see `FormulationBox`. Widening a
//! column after this returns does not make the constant conservative; it makes it a cap.
//! `instances` gives each row's instance its own box (InstanceBox), so an instance the default floor of 0 holds on is
//! sized from 0 and not from a lowered instance's floor.
double DecideTightPerRowBigM(const EvaluatedConstraint &ec, const FormulationBox &box, idx_t num_rows,
                             const vector<string> &var_names, const InstanceBox &instances,
                             const DecideClauseRef &written, const char *construct);

//! The instances of a decision whose default floor of 0 a constant lower bound under a `when` replaces (a constraint's
//! `lifted_floor`). A constant lower bound replaces the default floor on the instances whose rows it keeps; a decision
//! shared by several rows (keyed or query-wide) is lowered when any row its clause keeps lowers it. A decision with a
//! lower bound written (`boxes[v].lower_written`) is skipped.
//!
//! The instance box refines the decision box: `lower_bounds[v]` becomes the lowest floor of any instance (a lowered
//! instance's is its tightest kept bound, an instance no bound keeps has 0), so it stays a valid floor of every
//! column, and `instance_lower_bounds[v]` holds 0 on an instance the default floor still holds on and -infinity on one
//! that follows the decision's box. The exact floor (-5, or -3 where two bounds keep a row) still comes from the
//! clause's own row, which the model builder folds into the column, and which a diagnosis can loosen. The rigid floor
//! of a lowered decision is re-opened, as for a folded user bound. Runs after the rigid box is captured and before
//! implied bounds are derived, and needs the VarIndexer for the row to instance map.
void ResolveLiftedFloors(SolverInput &input, const VarIndexer &indexer, const vector<DecideBoundBox> &boxes);

//! Data-driven implied-bound propagation. For a non-negative `<=`/`=` row the model
//! emits, Sum_j a_j x_j (<=|=) K with a_j >= 0 and x_j >= 0, each instance satisfies
//! x_j <= K / a_j (the other non-negative terms only help), where a_j is the
//! instance's coefficient summed over the row's data rows (ClauseEmittedRows) and K
//! the row's bound less its fixed terms. A sound shared upper bound on a decision is
//! the max over its instances of the tightest such bound, when every instance has
//! one. This converts many
//! declared-unbounded variables into bounded ones, enabling a finite, correct,
//! tighter Big-M. Only provably-implied bounds are applied, so the feasible
//! region — and the optimum — are unchanged.
//!
//! Single pass (not a fixpoint): a bound derived for one variable is not fed back
//! to tighten others in the same sweep. This is sound — it only leaves some
//! tightness on the table for chained implications — and avoids iterating to
//! convergence over potentially many constraints.
//!
//! Reads `input.constraints` and tightens `input.upper_bounds`. A merged coefficient past a DOUBLE raises the error
//! that names its clause here, before the model builder, as no bound can be read from it.
void DecidePropagateImpliedBounds(SolverInput &input, const VarIndexer &indexer, idx_t num_rows);

//! Whether a construct is stated natively or lowered, and the rule that decides.
//!
//! Native is the default: where the backend declares a construct, it is used. A construct is lowered only where the
//! lowering is MEASURED faster, and each such construct cites its measurement (`lowering_measured_faster`). A construct
//! with no such measurement stays native wherever it is declared.
//!
//! MIN/MAX is lowered. A general constraint (`z = MAX(t..)`) and a Big-M family encode the same thing, so where both
//! are valid this is a pure performance question, and the answer, measured, is the lowering. Two reasons, both
//! structural:
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
//! search quality to buy back: the Big-M here is per-row tight. So for MIN/MAX native is
//! the FALLBACK, reserved for the case that has no valid Big-M at all, which is the case it
//! was built to answer and the only one where the lowering must refuse the query.
//!
//! ABS is not lowered. Measured on Gurobi on Q10 (one binary, 200K rows) the lowering ran 12.0-12.2 s against 12.2-12.3
//! s native, with the same objective: a tie, so there is no gain to give up the native construct for.
struct NativeConstructPolicy {
	//! The backend declares this construct (stage 05, read off its capabilities).
	bool available = false;
	//! Test-only `DECIDB_NATIVE_CONSTRUCTS=force`: state it natively wherever it is
	//! declared, so the A/B equivalence tests still have two arms to compare.
	bool forced = false;
	//! A measurement shows the lowering faster than the native construct (true for MIN/MAX, false for ABS).
	bool lowering_measured_faster = false;

	//! The rule. `big_m_underivable` is this SITE's question, not the query's: one
	//! clause can have a derivable range while another in the same query does not.
	bool Use(bool big_m_underivable) const {
		return available && (forced || big_m_underivable || !lowering_measured_faster);
	}
};

//! Encode every constraint stage 05 tagged with a `<>` indicator as the disjunction it
//! is: `z == 0 => LHS <= K-1` and `z == 1 => LHS >= K+1`, a pair of CONDITIONAL ROWS.
//! That is the only spelling this emits. Whether the chosen backend states a condition
//! itself or needs it encoded with a Big-M is settled once, afterwards, by
//! `LowerDecideConstructs` — so no bound is asked for here and no backend is consulted.
//!
//! Both forms of the clause are finished here. A per-row `<>` expands against the
//! row-scoped indicator stage 05 allocated; an aggregate one allocates a *global* binary
//! per group, because a group's rows sum onto one row and one row needs one binary.
//! `aux_var_expressions` supplies the clause text stage 05 recorded for the indicator, so
//! a dropped aggregate `<>` can be named in a repair.
//!
//! Both halves carry the clause's `indicator_col` as formulation provenance, while the
//! independent `removal_group_id` groups every descendant into one droppable `<>`.
//! Dropping the source clause is the only repair a `<>` has.
//!
//! Refuses a left-hand side that is not integer-valued — the ±1 band is only exact
//! on the integer lattice — and silently drops a comparison whose bound no integer
//! can equal, since every assignment already satisfies it.
void LinearizeNotEqual(SolverInput &input, const VarIndexer &indexer, const vector<string> &var_names);

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
//! `box` is the box the emitted Big-Ms are valid for — the same explicit-box contract
//! `DecideTightPerRowBigM` carries, in the flat column coordinates this pass works in.
void LowerDecideConstructs(SolverInput &input, const VarIndexer &indexer, const FormulationBox &box,
                           const vector<string> &var_names, const SolverConstructSupport &constructs);

//! Floor each product's column at its range's, min(0, its non-Boolean factor's floor), raised by a floor written on
//! it (`b * x >= -4`): a product has no default floor of 0 (AddBooleanProduct). Stage 08 runs it before implied-bound
//! propagation, which then reads `b * x` over x >= 0 as a non-negative term.
void FloorBilinearAuxiliaries(SolverInput &input);

//! Emit the McCormick envelope for every `w = b * x` link. `w` has no default floor of 0 (AddBooleanProduct): its
//! column is floored at the product's range, min(0, the lowest L of the instances of `x` in `box`), raised by a floor
//! written on it (`b * x >= -4`). Where that floor is 0 or more it implies the lower corner
//! and the upper corner collapses to the plain structural `w <= x`, so three rows suffice; where it is negative all
//! four corners are emitted, so the product reaches the negative value. Requires a finite upper bound on `x` and
//! names it if missing.
//!
//! The rows are emitted per INSTANCE of `w`, in flat columns, each from the box of the instance of `x` it reads: a
//! query-wide product is one block, a keyed one a block per key, a row-scoped one a block per row.
//!
//! The envelope is an EXACT linearization of the product over `box`, not a dominating
//! constant: on a box wider than `box` it cuts valid points, and on a narrower one it is
//! slack. So unlike a Big-M it is wrong in both directions once the box moves, which is
//! why the box is named rather than read from `input`. `input` is still written (the
//! auxiliary's own column is floored at the product's range), and that write is a column
//! declaration rather than a derived constant.
void LinearizeBilinear(SolverInput &input, const VarIndexer &indexer, const FormulationBox &box);

//! Declare each McCormick auxiliary's box before ABS/MIN/MAX descendants inspect it. A factor with no
//! finite ceiling is refused, unless `refuse_when_unbounded` is false: a helper decision of the value rewrite
//! (`SolverInput::reducer_value_links`) is boxed after this runs, so the run that settles the helpers' boxes
//! leaves such an auxiliary open and a final refusing run decides.
void DeriveBilinearAuxiliaryBounds(SolverInput &input, const FormulationBox &box, const vector<string> &var_names,
                                   bool refuse_when_unbounded = true);

//! The lower envelope of every ABS (SolverInput::abs_links): its two evaluated rows, `aux >= inner` (`operand`) and
//! `aux >= -inner` (`negated`), become rows of `input.constraints` like any clause; the link keeps `operand`, which the
//! rows of an exact ABS are read from. Called once, as the evaluated model is handed over, before any pass reads the
//! rows: implied-bound propagation reads `ABS(x) <= 3` as `x <= 3` through them. The pair pins `aux` from below, and to
//! exactly |inner| where what pushes on the ABS holds it down; an exact ABS is pinned from above by EmitAbsConstraints.
//! Refuses an envelope row that holds an aggregate as an internal error: stage 05 gives every reducer a function reads
//! a helper decision, so the rows are read per row.
void EmitAbsEnvelopes(SolverInput &input);

//! Phase 1 of the ABS auxiliary formulation, and it runs before EVERY other
//! linearizer. For every ABS (`abs_links`), exact or not, it derives the signed interval of the
//! expression inside ABS over every row (the two sign-row constants of an exact ABS come from its ends)
//! and narrows the auxiliary's column box to the largest |inner| it reaches — the only place that
//! box is ever derived, since `aux >= inner` / `aux >= -inner` bound it from below
//! only. Every other linearizer computes its Big-M from column boxes, so an auxiliary
//! they read has to be boxed by the time they run. An ABS that is not exact is pushed down by every reader, so its
//! value is |inner| and the box holds it.
//!
//! `refuse_when_unbounded` is the gate's answer for this construct, and it refuses only an exact ABS. Where the backend
//! declares no native ABS it is true: no finite Big-M exists over an unbounded contributor, and the
//! query is refused naming a column to bound. Where it does declare one it is false — a general
//! constraint needs no Big-M, so the auxiliary is simply left unboxed (`AbsLinkSpec::range_unbounded`) and the
//! query answers. That divergence is the capability's whole payoff. An ABS that is not exact has no Big-M, so an open
//! reach leaves its box open whatever the flag says; a reader that needs the box (a product, a MIN or MAX, a `<>`)
//! refuses it through its own check.
void DeriveAbsAuxiliaryBounds(SolverInput &input, const VarIndexer &indexer, const vector<string> &var_names,
                              bool refuse_when_unbounded);

//! Phase 2: the rows that pin each exact ABS from above, emitted in flat columns, which is why this takes the
//! `VarIndexer`. The ABS is a pseudo-operation that stage 05 only recorded, and this is where it is legalized or
//! expanded (as LLVM's legalization decides per operation): `policy.Use(range_unbounded)` is the one place native
//! against lowered is decided for ABS, by the rule MIN and MAX read too.
//!   - Native: one column `t` per active row, an equality row `t = inner`, and a `GeneralConstraintSpec` saying
//!     `aux = |t|`. Each `t` is boxed by ITS OWN row's reach, keeping each end it can derive independently, exactly as
//!     `EmitExtremumLink` boxes its argument columns: a row whose contributors are all bounded gets a real box even
//!     when another row's are not; a column is left free only where nothing is derivable at all, which is the query
//!     this arm exists to answer.
//!   - Lowered: a sign indicator per instance of the auxiliary and the two Big-M rows it switches, each with its own
//!     constant from the signed interval DeriveAbsAuxiliaryBounds derived.
void EmitAbsConstraints(SolverInput &input, const VarIndexer &indexer, NativeConstructPolicy policy);

//! The value rewrite's helper decisions (SolverInput::reducer_value_links), phase 0, at the end of assembly, once the
//! rows and the objective are final: inline each helper whose every reader can read its members in place of its
//! column -- linear rows of plain clauses and the linear objective, or the members of one outer helper -- where doing
//! so adds no nonzero to the model (D89), counted on the data: an instance of `m` terms read `k` times costs `m * k`
//! written in, against `m + 1` for its definition and `k` for its reads. `held` lists the decisions read where only a
//! column can stand and SolverInput does not show it: a bound folded into its column's box. One read by an outer
//! helper's members -- a nested reducer -- is written into them, each term reading an instance becoming that instance's
//! terms; one read by rows and the objective is marked `inlined`, for the model builder to write its members where they
//! read it. Either way its variable becomes DecideVarScope::INLINED, with no column. A MIN or MAX group of one member
//! is that member, written in group by group (SolverInput::written_in); an outer MIN or MAX that may close natively
//! takes only instances read once. `minmax_values` are G/O MIN/MAX outputs whose non-singleton formulas stay explicit.
//! SUM/AVG values remain eligible for recursive expansion into their readers. A singleton MIN/MAX group may be written
//! directly at its readers, since its reducer value is that one member (deck p. 112). Returns whether a helper was
//! inlined or written through: the column space must then be laid out again.
bool InlineReducerValueLinks(SolverInput &input, const vector<idx_t> &held, const vector<idx_t> &minmax_values);

//! The value rewrite's helper decisions (SolverInput::reducer_value_links), phase 1: box
//! each helper instance by the reach of its own group's members -- the sum of their brackets
//! for SUM/AVG, their cover for MIN/MAX -- so a Big-M over it has a span and the root LP
//! a box. The reach goes to `instance_reach_lower` / `instance_reach_upper`, written afresh
//! on each call (an instance is mapped to its group through VarIndexer::InstanceOf); the
//! decision's box is the cover of every instance's reach and only ever tightens, like
//! DeriveAbsAuxiliaryBounds: a member may be an ABS auxiliary and an ABS may hold a helper,
//! so FormulateModel runs the two to a fixed point. A helper may be negative, so its floor
//! is derived like its ceiling, never defaulted to 0; an end no member bounds is left open.
void DeriveReducerValueBounds(SolverInput &input, const VarIndexer &indexer);

//! Its backward half (language/guide.md §5.5): a helper's box flows to the helpers it is defined from --
//! through the definition of a SUM or AVG, each term is at most the helper's top less every
//! other term's floor where it states `value >= members`, and at least its floor less every
//! other's top where it states `value <= members`; through a MAX's
//! envelope every member is at or under its top, a MIN's at or over its floor. So a nested
//! MIN/MAX whose closing side needs a Big-M finds one in a bound written only at the root
//! (`sum(max(x) by (g, h)) by (g) = 10`). One box covers every instance of a helper, so it
//! is tightened only when every instance it defines is bounded, by the loosest bound.
//! Only tightens; FormulateModel runs it in the same fixed point.
void DeriveReducerMemberBounds(SolverInput &input, const VarIndexer &indexer);

//! Phase 2: emit each helper's definition, one row per group of its members. SUM/AVG:
//! the flat equality `value - members = constants` (an AVG's members carry their 1/n).
//! MIN/MAX: an extremum link (EmitExtremumLink) on the sides the clause needs, its
//! closing Big-M sized off the helper's box against the family's far end, and refused
//! naming a column to bound where no such constant exists on the lowering arm; an extremum group of one member is that
//! member, an equality (deck p. 112). The evaluated member rows are consumed here; none reaches `input.constraints`.
//! An inlined helper has no definition: its members move to SolverInput::inlined_helpers.
void LinearizeReducerValueLinks(SolverInput &input, const VarIndexer &indexer, const vector<string> &var_names,
                                NativeConstructPolicy policy);

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
//! Each end is seeded by the first row that reaches it, so the range is the cover of the
//! family and nothing else; a family holding only positive values does not reach down to 0.
//! A family that covers no row at all stays [0, 0], which is what the sites that pin an
//! empty auxiliary to exactly 0 rely on.
struct AuxRange {
	double lo = 0.0;
	double hi = 0.0;
	//! Whether `lo` / `hi` hold a covered value yet, as opposed to the empty family's 0.
	bool lo_seeded = false;
	bool hi_seeded = false;
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
	bool Unbounded() const {
		return lo_unbounded || hi_unbounded;
	}

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
		if (other.lo_seeded) {
			FoldLo(other.lo);
		}
		if (other.hi_seeded) {
			FoldHi(other.hi);
		}
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

	//! Extend by one more row expression bracketed by [`row_lo`, `row_hi`], each end folded in or marked open on its
	//! own (SignedRowRange), so a row open on one side still contributes its closed side to the box. An open end's
	//! value is ignored; an end that is not open is folded in as it is, so a value that overflowed a DOUBLE reaches
	//! the box as an infinity and the hand-off names the clause. `var` names the variable to blame if a Big-M is later
	//! asked for over this family.
	void CoverRowEnds(double row_lo, double row_hi, bool lo_open, bool hi_open, idx_t var) {
		if (lo_open) {
			MarkLoUnbounded(var);
		} else {
			FoldLo(row_lo);
		}
		if (hi_open) {
			MarkHiUnbounded(var);
		} else {
			FoldHi(row_hi);
		}
	}

private:
	void FoldLo(double value) {
		lo = lo_seeded ? MinOf(lo, value) : value;
		lo_seeded = true;
	}
	void FoldHi(double value) {
		hi = hi_seeded ? MaxOf(hi, value) : value;
		hi_seeded = true;
	}
	static double MinOf(double a, double b) {
		return a < b ? a : b;
	}
	static double MaxOf(double a, double b) {
		return a > b ? a : b;
	}
};

//! The row of a MIN/MAX link (`z - expr op bound`), once its terms are summed per column.
//!
//! DecideTerm arrays are indexed by term, not by variable, so the same solver column
//! reaches one row more than once in two situations: `(c + 1) * x` distributes
//! into `c*x + 1*x`, and a keyed or scalar variable resolves to a
//! single column across every row it spans. A repeated column index is rejected
//! outright by both Gurobi and HiGHS, so a row's columns are summed before they land here, by the
//! SparseCoeffAccumulator the family's rows share (the sum the model builder takes for a clause's rows): a family
//! has a row per data row, and a hash map per row cost an allocation per member. Constant terms carry no column at
//! all (`variable_index == INVALID_INDEX`) and collect into `constant`, which the caller folds into the bound:
//! `z <= expr + k` is `z - expr <= k`.
//!
//! The accumulator hands columns over in first-appearance order, or in column order where it sorts instead (past its
//! dense cap), so the solver is handed the same matrix ordering every run.
//!
//! The constant grows through AddCoefficient: `written` names the clause (or the objective) the row comes from.
struct MinMaxLinkRow {
	vector<int> indices;
	vector<double> coefficients;
	double constant = 0.0;

	void AddConstant(double value, const DecideClauseRef &written) {
		AddCoefficient(constant, value, written);
	}

	//! The left-hand side `1 * column + (this row)` of a solver row, sized for `extra` more columns so that nothing
	//! reallocates as the caller adds them.
	SolverInput::RawConstraint LeftHandSide(int column, idx_t extra = 0) const {
		SolverInput::RawConstraint rc;
		rc.indices.reserve(1 + indices.size() + extra);
		rc.coefficients.reserve(1 + indices.size() + extra);
		rc.indices.push_back(column);
		rc.coefficients.push_back(1.0);
		for (idx_t i = 0; i < indices.size(); i++) {
			if (std::abs(coefficients[i]) < 1e-15) {
				continue;
			}
			rc.indices.push_back(indices[i]);
			rc.coefficients.push_back(coefficients[i]);
		}
		return rc;
	}
};

//! The members of an extremum family, produced one at a time (late materialization). A member is `(result - expr)` as
//! a half-row — `MinMaxLinkRow` already stores the expression negated, with its constant part separate — and no member
//! is kept: `fill_link` writes member `i` into one scratch row the emitter reuses, so a family of n members costs one
//! row of storage rather than n. `range` is the reach of `expr` on its own, which is the box a column pinned to it
//! deserves; it is asked only by the arm that pins one, so no other arm pays for it.
struct ExtremumMemberSource {
	idx_t count = 0;
	//! Fills `link`, which is empty, with member `i` of `count`. Called once per member and pass over the family.
	std::function<void(idx_t, MinMaxLinkRow &)> fill_link;
	std::function<AuxRange(idx_t)> range;
};

//! `result = MIN/MAX(members)`, in flat columns. Every MIN/MAX auxiliary in the model is
//! one of these: a helper decision's definition, per group of its path key.
//!
//! Two independent sides, because which of them the readers of the helper already supply
//! differs and is not a property of the construct:
//!
//!   - The **envelope** (`result >= member` for MAX) holds `result` at or above every
//!     member. An objective that minimizes `result` supplies it for free.
//!   - The **closing** side pins `result` down onto some member: one Big-M row per
//!     member and a `SUM(y) >= 1` that makes one bind. An objective that maximizes
//!     `result` supplies it for free.
//!
//! A helper needs the side its readers push it against, and both when they push it nowhere (`=`, `<>`, a
//! factor of unknown sign); which one is the polarity stage 05 records (DecideHelperSides).
struct ExtremumLinkSpec {
	idx_t result_column = DConstants::INVALID_INDEX;
	bool is_max = false;
	bool need_envelope = false;
	bool need_closing = false;
	//! The closing Big-M, and whether one exists at all. Resolved by the CALLER, because
	//! the family a link reduces over is not always the family of its own members: an
	//! outer MIN/MAX over group SUMS spans the per-row reach times a group's row count,
	//! not the per-row reach. `blame_var` names the variable that left it open, INVALID_INDEX
	//! when none did: the reach overflowed a DOUBLE, and the refusal names the clause
	//! `source_clause_id` stands for (ThrowUnboundedBigMNaming).
	double closing_big_m = 0.0;
	bool closing_underivable = false;
	idx_t blame_var = DConstants::INVALID_INDEX;
	//! The clause that first reads the helper (INVALID_INDEX: the objective). Every row the link writes takes it as its
	//! origin, so a refusal of one names that clause.
	idx_t source_clause_id = DConstants::INVALID_INDEX;
	//! Names every column and binary this emits, through the global label channel, so a
	//! diagnosis renders `MAX(x)` rather than an internal column.
	string label;
};

//! Emit one extremum link, in whichever form the policy and the spec call for. This is
//! the ONLY place a MIN/MAX auxiliary is pinned, and the only place that three-way
//! choice is made: it used to be written out at each of the five sites above.
//!
//! The envelope, where the spec asks for it, is emitted either way — it costs one row
//! per member, needs no constant to dominate anything, and is implied by the general
//! constraint on the native arm. The closing side is where the two formulations part:
//! `result = MIN/MAX(cols)` stated for the backend, or the Big-M family that encodes it.
//! The envelope rows come first and the closing rows after, so a link that needs both draws on `members` twice.
void EmitExtremumLink(SolverInput &input, const VarIndexer &indexer, const ExtremumLinkSpec &spec,
                      const ExtremumMemberSource &members, const vector<string> &var_names,
                      NativeConstructPolicy policy);

} // namespace duckdb
