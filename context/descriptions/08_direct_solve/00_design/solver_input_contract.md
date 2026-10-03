# Solver Input Contract

What every direct-solve rule must reproduce of the solver path, whatever its
mathematics. A rule that proves a problem replaces the solver, so a user must not be
able to tell from errors, types, or row counts which path ran. The rule-specific
admission lives in each rule's spec ([S1](../02_first_rule/spec.md)); this page is the
part they share. The shared builders that implement it are in `direct_builder.cpp`.

## Every input row is read

The solver reads all of its input before it returns. A bad value on the last of 5,000
rows therefore raises even under an outer `LIMIT 1`, `COUNT(*)`, or a parent filter
that keeps one row. Only `LIMIT 0`, which reads nothing, may skip it. A rule whose plan
reads every row before releasing one (S1's rank window) declares that column as its
validation slot; a rule whose plan could stream wraps its checks in
`DirectValidationBarrier`. A standalone filter is not enough: it was shown to skip a
late row under `LIMIT 1`.

## Errors and their order

When several problems exist, the solver path raises the first in this order, and a
rule must raise the same one:

1. **Empty scoped aggregate.** A reducer under `WHEN` or `PER` over a nonempty input
   with no eligible row has no value: `DECIDE empty row set for aggregate in
   constraint. An empty aggregate has no well-defined value; check your WHEN clause.`
   (`DirectGuardEmptyAggregate`).
2. **Invalid data-valued bounds and pins, in source-clause order.** A NULL (or, for
   floating point, NaN) bound raises on every row, including rows a `WHEN` or a NULL
   `PER` key exclude from the clause, because the solver evaluates the bound before it
   applies the scope. A bound that is one column names it: `DECIDE: column "cap" is
   NULL. Impute it with COALESCE(cap, 0) or filter those rows out with a WHERE clause.`;
   a computed bound says `the bound expression is NULL`. An equality bound whose value
   varies within a group raises `DECIDE source-valued equality bound varies within a
   group`. A NULL source value in a per-row Boolean pin raises `DECIDE per-row Boolean
   bound contains NULL`. (`DirectValidateBounds`, `DirectInvalidBoundMessage`.)
3. **Invalid objective coefficients.** A NULL coefficient names its column when the
   score is exactly one column (`DECIDE: column "score" is NULL. ...`) and otherwise
   says `a value used in the optimization is NULL`; a NaN or infinite one raises
   `DECIDE objective coefficient contains invalid value (NaN or Infinity)`. The solver
   also reports the failing row number; a direct plan has no row to name.
   (`DirectValidScorePredicate`.)
4. **Infeasibility.** `DECIDE optimization is infeasible. Prefix the query with
   DIAGNOSE to see which clause to change.`

Errors DuckDB raises while evaluating a data expression (a failing cast, an overflow)
keep their own type and text. A rule that cannot prove it raises them in the solver's
order declines the expression; that is why throwing bounds and multi-term throwing
coefficients are solver cases today.

## One numeric domain

Every value the solver reads is a DOUBLE. Data expressions are evaluated with DuckDB's
own semantics, casts included, and only the result is converted. Values must be finite;
whole-number limits are exact only up to `2^53`, and a rule refuses anything it cannot
represent exactly rather than rounding it. Strict and fractional count limits normalize
as the solver does: `< U` to `ceil(U) - 1`, `<= U` to `floor(U)`, `> L` to
`floor(L) + 1`, `>= L` to `ceil(L)`.

## Results

The result keeps the DECIDE node's bindings: every source column in order, then one
column per decision, typed `INTEGER` 0/1 for `BOOL`, `BIGINT` for `INT` and `DOUBLE` for
`REAL`. Duplicate source rows are separate rows with separate decisions. When several
assignments are optimal, a rule may return a different one than the solver; tests
compare row count and primary objective, never tied assignments.

## Wording

Messages follow the project's rule for user-facing text: name the object the user
wrote and the smallest edit that fixes it, with no solver vocabulary. Internal detail
belongs in `require` reasons and `EXPLAIN`, not in errors a query raises.
