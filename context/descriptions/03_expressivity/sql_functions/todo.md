# SQL Functions & Expressions — open work

No open work. Supported forms and intentional rejections are recorded in
[`done.md`](done.md) and the syntax reference.

## Functions that behave unlike SQL

- **Owner's rule (2026-10-27):** a function inside DECIDE behaves as the same SQL function does, NULL and NaN included,
  unless a strong reason that helps users says otherwise; each divergence is stated in the docs with that reason.
- **Done so far:** `COUNT(col)` on a bound skips a NULL and counts a NaN
  (`decide_rewrite_rhs_window.cpp`, `MakeReducerColumn`; `test_s1_count_bound_skips_null_and_counts_nan_as_sql_does`).
- **Decision needed:** `SUM`, `MIN`, `MAX` and `AVG` raise on a NULL argument (syntax reference §3, "never a zero"),
  where SQL skips it. Keep the error (it names the column; a skipped NULL can hide missing data), or follow SQL?
- **Change:** list every other place DECIDE reads a function differently from DuckDB (NaN and infinity in each
  reducer, `AVG` over no rows, integer overflow, empty groups) and bring each to the owner.
