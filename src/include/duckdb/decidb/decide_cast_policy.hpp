//===----------------------------------------------------------------------===//
//                         DecidB
//
// duckdb/decidb/decide_cast_policy.hpp
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
// So a cast over a decision is one of exactly two things:
//
//   REPRESENTATION -- the same number in a different container. Everything the
//   binder inserts to reconcile the two sides of a comparison is this. It means
//   nothing, and peeling it is always correct.
//
//   COMPUTATION -- a different number. `CAST(x AS INTEGER)` is `round(x)`, a
//   step function; a narrowing DECIMAL drops decimal places. These are rejected
//   at the canonicalization boundary, because they are nonlinear operations over
//   a decision (the same family as ABS and MIN/MAX) wearing a cast's syntax, and
//   silently peeling one answers a different question than the user asked.
//
// WHAT THAT BUYS. Because DecideCanonicalizer::ValidateDecisionCasts rejects the
// second kind before a query leaves planning, every cast any downstream walker
// can meet is of the first kind. That is what lets those walkers peel
// unconditionally and still be correct -- not care taken at each site, but an
// invariant established once. Add a new walker and it is right by default.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! True when `to` can still draw every distinction `from` could, so a cast
//! between them is REPRESENTATION rather than COMPUTATION (see the file header).
bool DecidePreservesResolution(const LogicalType &from, const LogicalType &to);

//! True when `type` can name values between whole numbers. Integral types and
//! DECIMAL with scale 0 cannot; that is what makes a cast into them round.
bool DecideHasFractionalResolution(const LogicalType &type);

//! Look through the casts wrapping `expr`, stopping at the first one that would
//! change the value. Replaces the three identical hand-rolled loops that used to
//! live in physical_decide.cpp and decide_optimizer.cpp.
//!
//! On the decision path the stop condition never fires -- the canonicalizer already
//! rejected anything that would trip it -- so this reads as "peel freely". It earns
//! its keep on DATA: a bound like `x <= CAST(1.6 AS INTEGER)` is a rounding request
//! the executor must actually perform, and peeling to the `1.6` underneath would cap
//! `x` at 1 where SQL says 2. One helper is therefore correct on both paths, and a
//! new caller does not have to know which path it is on.
const Expression *UnwrapDecideCasts(const Expression &expr);
Expression *UnwrapDecideCasts(Expression &expr);

//! Strip EVERY cast, numeric or not, to reach the node's IDENTITY.
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

//! Render `expr` as the user would recognise it: every cast removed, at every depth.
//!
//! StripCastsForIdentity only reaches the casts wrapping the OUTSIDE of a node, which
//! is enough for a bare column but not for an expression -- `x * w` comes back as
//! `x * CAST(w AS DOUBLE)` because the binder's cast sits on a child. An error that
//! quotes that is naming an object the user never typed, so anything appearing in a
//! message or a suggested edit must come through here.
string DecideDisplayString(const Expression &expr);

} // namespace duckdb
