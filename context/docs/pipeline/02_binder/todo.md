# Stage 02 — Binder: open work

Language-surface work is owned by `../../language/` (the scopes:
`../../language/scopes.md`); what the language does not yet accept is listed in
`../../language/errors.md` §2, and the scope suites in
`../../testing/scope_suite.md`.

---

## 1. Bind-time refusals a later change may lift

Each is refused here, at bind time, with a message that says "not supported yet". None is a wrong answer; each is a
shape the later layers do not formulate. The two-level objective's restrictions are deck features and are on the
[roadmap](../../language/roadmap.md#nested-reducers-anr-pp-25-2832).

| Refused | Message | What lifting it needs |
|---|---|---|
| A window function in a `when`, a key or a `by` | *"DECIDE: a window function such as 'row_number() OVER ()' is not supported in when, per or by"* | a window evaluated over the rows the scope keeps, before the scope reads it |
| `norm(D: e, p)` | *"DECIDE: norm(D: ...) is not supported yet; a relation qualifier takes SUM, AVG, MIN or MAX"* | the norm lowering (stage 05) carrying the qualifier's key scope onto the reducer it builds |
| A strict comparison over a `MIN` or `MAX` whose value can be fractional (`max(x + 0.5) > 3`) | *"Comparison '>' is not supported here: 'max(x + 0.5)' has type DECIMAL(21,1) ... Compare with '>=' instead."* | moving the constant addend onto the bound in stage 04, the shape owner; done in stage 05 it leaves the canonical contract |

**Ruling (owner, 2026-10-27).** Lift `norm(D: e, p)` and the strict comparison over a fractional `MIN` or
`MAX`. The window function in a `when`, a key or a `by` stays refused: it needs a new
evaluation step over the kept rows for little gain.

**Test.** For each lifted shape, an oracle-judged test on both solvers
(`_scope_oracle.py` or `_brute_force.py`). The refusals are pinned in
`test_scope_objective.py`, `test_scope_explain_diagnose.py`, `test_scope_generation.py`,
`test_scope_reducer_inner.py` and `test_scope_reducer_by.py`; a lifted shape's pin
becomes its answer test.

**Update.** `done.md` §4a, `../../language/errors.md` §2.

## 2. Functions that behave unlike SQL

**Rule (owner, 2026-10-27).** A function inside DECIDE behaves as the same SQL function does, NULL and NaN included,
unless a strong reason that helps users says otherwise; each divergence is stated in the docs with that reason.
`COUNT(col)` on a bound already follows it: it skips a NULL and counts a NaN (`MakeReducerColumn`,
`decide_rewrite_rhs_window.cpp`; `test_s1_count_bound_skips_null_and_counts_nan_as_sql_does`).

`SUM`, `MIN`, `MAX` and `AVG` raise on a NULL argument, where SQL skips it
(`../../language/functions.md`, "Constraints", NULL). **Ruling (owner, 2026-10-27).** A NULL that multiplies
a decision (`sum(price * x)` with a NULL `price`) keeps the error: it names the column, and skipping it would change
the model silently. A reducer over data only skips the NULL, as SQL does. The deck also skips NULLs ([roadmap](../../language/roadmap.md#an-empty-reducer-group-and-a-null-value-pp-45-50-56)).

**Work.** List every other place DECIDE reads a function differently from DuckDB (NaN and infinity in each reducer,
`AVG` over no rows, integer overflow, empty groups) and bring each to the owner.

**Update.** `../../language/functions.md`, `../../language/guide.md` §3 and §7.2.
