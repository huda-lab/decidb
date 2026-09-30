# Design experiments (2026-09-30)

These observations were made on branch `direct-solve-prototype` from
`721d49ba43d10fd4e9e15572508f10a873970c3b` after `make release
BUILD_JOBS=4`. They establish a starting design, not a shipped direct path.
The temporary bound-plan test code was removed after the experiment; the
implementation must turn its checks into permanent regressions.

## Bound-plan boundary spike

A test-only pre-optimizer extension replaced a bound `LogicalDecide` with a
`LogicalWindow` (`ROW_NUMBER`), a result projection, and a logical-only boundary
that advertised the original child-plus-decision bindings and SQL types. It
then ran the normal optimizer, serializer verifier, physical planner, and
executor with both installed backends available. This injection occurred
*before* the built-in optimizer passes, earlier than the intended direct
attempt; the production seam and its remaining passes still need dedicated
tests.

The first version **failed**. `RemoveUnusedColumns` saw no references to the
generated projection's own bindings and replaced it with one `42` constant.
The boundary still advertised three external outputs; a parent read physical
column 2 from a one-column child and raised an internal out-of-range error.
Merely returning the old bindings from `GetColumnBindings()` is therefore
unsound.

The revised probe gave the boundary an explicit bound-column dependency for
each generated output. The pruner then kept the required columns. The focused
run passed **28 assertions in two C++ cases** (`build/release/test/unittest
"[direct_design_probe]"`), covering output values and `INTEGER` decision type,
an outer filter, `PRAGMA verify_serializer`, and physical lowering. This is a
safe initial *retain-all* mapping; it does not establish a selective pruning
implementation. The probe temporarily exposed the already-resolved physical
child-lowering overload; production code needs an owned lowering entry point
that does not run binding resolution a second time.

## Runtime validation probe

The bound probe placed a truth-or-error `isfinite(score)` filter below a global
window. It reported an invalid coefficient when `x` was not projected and
the SQL capacity was zero; it also reported a NULL in the last of 5,000 rows
under an outer `LIMIT 1`. NaN and both infinities raised the probe error;
a row-dependent bad `CAST` raised DuckDB's conversion error. A source `WHERE`
that removed the bad row succeeded. The probe used a fixed rank threshold of
one even in the capacity-zero query: **it validated execution at zero capacity,
not the zero-capacity assignment**. The production rule must use the proved
capacity and test both assignment and guard survival there.

An ordinary SQL truth-or-error filter **without** a blocking window returned
the first row under `LIMIT 1` and skipped a bad row at position 4,999. With a
global window between the filter and limit, it raised the error. That explains
the proposed placement. It is not a general guarantee from SQL evaluation
order: an optimizer can remove a window whose result becomes dead. The first
implementation must retain the rank/validation dependency even when `x` is
unused, or use an explicitly verified blocking validator.

The existing solver path raised `Invalid Input Error` for NULL, NaN, and
infinity with unused `x` and capacity zero; a bad cast raised `Conversion
Error`. A late NULL was checked under outer `LIMIT 1`, `COUNT(*)`, and an outer
filter, for both the host default and forced HiGHS. A source filter removing
it succeeded. Outer `LIMIT 0` did not execute the DECIDE operator and did not
raise a value error. The direct path should preserve that execution boundary.
The initial guard need not reproduce the solver's row-numbered text, but must
preserve invalid/NULL categories and ordinary expression errors; any message
difference should be explicit in tests.

## Numeric and class checks

The current solver returned the expected 0/1 assignments for capacities 0,
1, and larger than the row count; a fractional `1.5` acted like capacity 1,
and negative capacity was infeasible. The proposed first rule deliberately
admits only non-negative integral capacities. `2^53` and `2^53+1` both solved
the two-row fixture, which cannot prove large-bound equivalence; the syntax
reference already identifies values beyond `2^53` as outside the reliable
DOUBLE domain. Keep the admission bound conservative.

Solver assignment is not a suitable numeric oracle near zero. For two rows
with scores `(1e-12, 0)` and capacity one, both forced HiGHS and Gurobi returned
`(0, 0)`. At `1e-9`, HiGHS still returned `(0, 0)` while Gurobi returned
`(1, 0)`; at `1e-6`, both returned `(1, 0)`. A one-row `1e-12` fixture returned
`1` on both. These are observed solver choices, not a mathematical zero
threshold. The direct rule should select by the finite DOUBLE score's sign;
the independent mathematical oracle checks optimality, while backend
differentials compare objectives within a stated tolerance.

Current-syntax positive and one-condition-away solver outputs are recorded in
the [baseline corpus](../03_correctness/baseline.md).
