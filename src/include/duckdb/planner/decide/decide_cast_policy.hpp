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

//! Evaluate a complete foldable SQL expression and convert its result to DOUBLE.
//! Data-only casts remain part of the computation. Returns false for a non-foldable,
//! NULL, non-numeric, or otherwise non-evaluable expression; conversion exceptions
//! are deliberately left to the caller's layer-specific error policy.
bool TryEvaluateFoldableDouble(ClientContext &context, const Expression &expr, double &out);

//! True for a literal under any number of cast wrappers.
//!
//! This is an ELIGIBILITY test, not a folder: it asks whether an expression is a
//! constant the user wrote, so bound absorption stays a shape optimization rather
//! than becoming a general constant folder. Pair it with TryEvaluateFoldableDouble,
//! which evaluates the whole expression so a data cast keeps its SQL semantics.
bool IsCastWrappedConstant(const Expression &expr);

} // namespace duckdb
