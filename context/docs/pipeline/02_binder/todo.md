# Stage 02 — Binder: open work

Language-surface work is owned by `../../language/` (the scopes:
`../../language/scopes.md`); what the language does not yet accept is listed in
`../../language/errors.md` §2, and the scope suites in
`../../testing/scope_suite.md`.

---

## 1. Functions that behave unlike SQL

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
