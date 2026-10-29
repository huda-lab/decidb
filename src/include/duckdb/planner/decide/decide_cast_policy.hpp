//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/planner/decide/decide_cast_policy.hpp
//
// The single answer to "what does a cast mean over a decision variable?"
//===----------------------------------------------------------------------===//
//
// WHY THIS EXISTS. A DECIDE constraint's left side is not a value the executor
// computes -- it is a shape handed to a solver, taken apart into (variable,
// coefficient) pairs while `x` still has no value. A cast is a function, and a
// function cannot be applied to a value that does not exist yet, so every walker
// that takes an expression apart reaches a BoundCastExpression and has to decide
// whether to look through it.
//
// Historically each walker decided for itself: a census found 45 unwrap sites,
// 42 of them independent, including three byte-identical helpers in different
// files. Only 12 carried any guard. Every unguarded one was a place a
// value-changing cast could be dropped silently, which is why cast bugs kept
// recurring in different shapes rather than staying fixed.
//
// THE POLICY. DECIDE carries ONE numeric domain: DOUBLE. That is not a choice
// made here -- `solver_input.hpp` hands the backend `vector<double>` for every
// bound and every coefficient, and Gurobi and HiGHS take doubles, so every value
// in a model lands there regardless of the types the expression tree carried.
// A model therefore holds about 15 significant digits, and a conversion INTO
// that domain is the edge of what DECIDE represents, not a loss inside it.
//
// THE POLICY HAS TWO BOUNDARIES.
//
//   PARSED: every user-written cast over a decision is rejected, including a
//   cast to DOUBLE. This is the only point where authorship is observable.
//
//   BOUND: any cast whose subtree still contains a decision was inserted by
//   DuckDB while reconciling types and is transparent to the solver's DOUBLE
//   algebra. A data-only cast is a real DuckDB computation and is never peeled;
//   the complete expression is evaluated first and only its result becomes DOUBLE.
//
// This split is what makes a new walker safe: shape walkers unwrap only casts
// proven decision-bearing, while value walkers never look beneath a data cast.
//
// THE ONE ASSUMPTION THIS ALL RESTS ON. The BOUND rule above is sound only
// because the PARSED rule caught every user-written cast first -- and the parsed
// rule tells a user cast from a parser-synthesized one by a single signal: the
// parser stamps a query location on nodes that came from typed characters
// (`Transformer::TransformTypeCast` calls `SetQueryLocation`), and nodes it
// invents have none to stamp. `ValidateDecideNoExplicitDecisionCasts` reads
// exactly that signal.
//
// That signal is upstream bookkeeping DuckDB maintains for its own error
// messages, not a contract owed to us, and it is one easily-omitted line --
// `TransformBooleanTest` already builds a CastExpression without it, stamping
// the sibling nodes on either side. Harmless there, because that cast is
// genuinely parser-internal, which is the answer we want anyway.
//
// The failure directions are NOT symmetric, which is why this note exists:
//
//   a synthesized cast that gains a location -> a valid query is rejected.
//       Loud, a user reports it, someone fixes it.
//
//   a user cast that loses its location      -> read as reconciliation noise and
//       peeled. `CAST(x AS INTEGER)` silently models `x` instead of `round(x)`:
//       no error, plausible output, WRONG ANSWER. That is precisely the bug
//       class this policy exists to end, re-entering through its own front door.
//
// `test_all_explicit_decision_cast_syntax_is_rejected` pins CAST, TRY_CAST and
// `::` across constraint and objective, so a regression in any cast syntax in use
// today fails the suite. The residual exposure is a cast syntax DuckDB adds
// later that omits the stamp. If a decision cast is ever reported as slipping
// through, check the location stamp on its parsed node BEFORE looking anywhere
// downstream -- every unwrap site below is unguarded by design and will look
// innocent, because under this invariant it is.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class ClientContext;
class BoundColumnRefExpression;

//! True when `expr` references a variable from the LogicalDecide binding.
bool BoundExpressionReferencesDecide(const Expression &expr, idx_t decide_index);

//! Look through outer cast wrappers only while the wrapper belongs to
//! decision-bearing solver algebra. These casts were inserted by DuckDB after the
//! parsed boundary rejected every user-written decision cast, so they are type
//! reconciliation noise. A data-only cast is a real SQL computation and stops the
//! walk.
const Expression *UnwrapDecideCasts(const Expression &expr, idx_t decide_index);
Expression *UnwrapDecideCasts(Expression &expr, idx_t decide_index);

//! The single decision variable `expr` names, or nullptr when it names anything else.
//!
//! "Anything else" includes a product, a sum, a data column, and a decision buried
//! under arithmetic: this asks whether the WHOLE expression is one bare variable, so
//! a caller can treat it as a coefficient-free term. Only the cast wrappers
//! UnwrapDecideCasts admits are looked through, which is what keeps the answer a
//! cast-policy decision rather than each walker's own.
const BoundColumnRefExpression *GetBareDecideColumnRef(const Expression &expr, idx_t decide_index);

//! Strip EVERY outer cast, numeric or not, to reach the node's IDENTITY.
//!
//! Correct only where the caller never reads the value: rendering a label (`grp = 'a'`
//! should read as `grp = 'a'`, not `grp = CAST('a' AS VARCHAR)`) and reading a
//! binding (a flattened subquery is the same column whatever wraps it). A cast cannot
//! change either, so peeling past a value-changing one is safe here and only here.
//!
//! Kept separate from UnwrapDecideCasts so the choice is visible at every call site:
//! if the result feeds arithmetic, a bound, or a shape decision, this is the wrong
//! function.
const Expression *StripCastsForIdentity(const Expression &expr);

//! How `TryEvaluateFoldableDouble` judges an expression and its value. Each flag is one difference between the callers
//! that evaluate a constant to a DOUBLE; a caller names the ones it needs and gets nothing else.
enum FoldableDoubleFlag : uint32_t {
	//! Foldable, non-NULL and numeric, with the result converted as DuckDB casts it. The result may be infinite or NaN,
	//! and a conversion error propagates to the caller's own error policy.
	FOLD_PLAIN = 0,
	//! Also demand a constant no row, parameter or subquery can change: consistent, and free of parameters,
	//! subqueries, aggregates and windows.
	FOLD_STABLE = 1,
	//! Also demand a finite result that survives a round trip back to the expression's own type.
	FOLD_EXACT_FINITE = 4,
	//! Report a DuckDB error raised by the evaluation or the conversion as false.
	FOLD_CATCH_DUCKDB_ERRORS = 8,
	//! Report any exception whatever as false (implies `FOLD_CATCH_DUCKDB_ERRORS`).
	FOLD_CATCH_ALL = 8 | 16
};

//! Evaluate a complete foldable SQL expression and convert its result to DOUBLE. Data-only casts remain part of the
//! computation. Returns false for a non-foldable, NULL or non-numeric expression, and for
//! whatever else `flags` (a combination of `FoldableDoubleFlag`) rules out. `out` is written once the value is
//! converted, before the finite and round-trip check, so it is meaningful only when the call returns true.
bool TryEvaluateFoldableDouble(ClientContext &context, const Expression &expr, double &out, uint32_t flags);

//! What is known of a factor's sign before the query runs. Read structurally, not only
//! by folding: a product is ZERO when any multiplier is, whatever the others
//! (`0 * w` with a data column `w`), and NEGATIVE or POSITIVE when every factor's sign is
//! known; a negation flips; a quotient follows its numerator unless the divisor is zero
//! or unknown. A data column, a subquery, or arithmetic that only the rows decide is
//! UNKNOWN. Every consumer that asks "which way does this factor push, if at all" --
//! the MIN/MAX rewrites and the ABS classifier -- reads the one answer from here.
enum class PlanTimeSign : uint8_t { ZERO, POSITIVE, NEGATIVE, UNKNOWN };

//! The decisions declared BOOL (LogicalDecide::is_boolean_var, by decide-column index). A BOOL is in {0, 1}, so as a
//! factor it is never negative: it keeps the direction of the term it scales (its sign reads POSITIVE; where it is 0
//! the term vanishes and any direction is right for it). Without it a decision's sign is UNKNOWN.
struct BooleanDecisions {
	idx_t decide_index;
	const vector<bool> &is_boolean;
};

PlanTimeSign PlanTimeSignOf(ClientContext &context, const Expression &expr,
                            const BooleanDecisions *booleans = nullptr);

//! The sign a factor on a reducer contributes: +1, -1, or 0 when it is not known until
//! the query runs (PlanTimeSignOf). A factor that is exactly zero reads as +1: the term
//! vanishes and any direction is right for it. The canonicalizer and the MIN/MAX
//! rewrite read it alike: a MIN or MAX under a factor of unknown sign takes the
//! composed formulation.
//!
//! Nothing depends on this for correctness -- it selects between an exact cheap
//! linearization and an exact expensive one. 0 must therefore be treated as "assume
//! the expensive one", never as an error.
int ScaleSignAtPlanTime(ClientContext &context, const Expression *scale, bool divides,
                        const BooleanDecisions *booleans = nullptr);

//! True when a factor on a reducer is exactly zero before the query runs
//! (PlanTimeSignOf), so the term vanishes: nothing pushes its extremum anywhere, and the
//! cheap formulation is right in every direction. A divisor is never a vanishing factor.
bool ScaleVanishesAtPlanTime(ClientContext &context, const Expression *scale, bool divides);

//! The way a comparison drives a term on its decision side, read before the query runs.
//! `<=` and `<` push a positive term DOWN and a negative one UP; `>=` and `>` the mirror.
//! NONE when nothing decides it: `=`, `<>`, or a sign (0) that is not known until the
//! query runs. A MIN or MAX pushed its cheap way -- MAX down, MIN up -- is held by its
//! envelope alone (every member bounds it); pushed the other way it needs the closing
//! side (some member must reach it); under NONE it needs both. The single-reducer, the
//! composed and the value rewrites all read the direction from here.
enum class ExtremumPush : uint8_t { DOWN, UP, NONE };

//! The push of a comparison's whole decision side (a term of sign +1).
ExtremumPush ExtremumPushOf(ExpressionType comparison);

//! The push of a term of `sign` under `outer`, the push of the side it stands in.
ExtremumPush ExtremumPushOf(ExtremumPush outer, int sign);

//! True when `push` is the cheap direction of a MIN (`is_max` false) or MAX; NONE never is.
inline bool IsEasyExtremum(bool is_max, ExtremumPush push) {
	return push == (is_max ? ExtremumPush::DOWN : ExtremumPush::UP);
}

} // namespace duckdb
