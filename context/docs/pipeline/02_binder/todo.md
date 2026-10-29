# Stage 02 — Binder: open work

Language-surface work is owned by `../../language/` (the scopes:
`../../language/scopes.md`); what the language does not yet accept is listed in
`../../language/errors.md` §2, and the scope suites in
`../../testing/scope_suite.md`.

---

## BIN-1 — Functions that behave unlike SQL

**Rule (owner, 2026-10-27).** A function inside DECIDE behaves as the same SQL function does, NULL and NaN included,
unless a strong reason that helps users says otherwise; each divergence is stated in the docs with that reason. A
reducer over data already follows it for NULL, a NULL data value is skipped (deck pp. 50, 56), a reducer over no rows
is NULL (D68), and `COUNT` follows it for NaN (`../../language/functions.md`, "Constraints", NULL).

**Audit (2026-10-27).** Each place DECIDE still reads a value differently from DuckDB, with the reason to keep it or
the change. Each needs a ruling.

1. **NaN in a bound or in a data reducer's argument** is an error (`... contains invalid value (NaN)`); SQL carries
   the NaN through (`SUM` is NaN). Recommended: keep, and state the reason: no solver reads a NaN bound.
2. **Infinity in a coefficient** is an error (`... (NaN or Infinity)`); SQL computes `inf * x`. In a bound it is a
   value, as in SQL (`x <= 4 / 0` holds always). Recommended: keep, and state the reason: a model cannot hold an
   infinite coefficient.
3. **The NaN and infinity message** lists "common causes" in a block. Recommended: name the column and the smallest
   edit (`../../language/errors.md`).

**Ruling (owner, 2026-10-29).** All three as recommended: NaN in a bound or a data reducer's argument stays an error;
infinity in a coefficient stays an error and in a bound stays a value; the message names the column and the smallest
edit instead of the "common causes" block.

Integer arithmetic follows DuckDB: a bound is computed in DuckDB's types before the model reads it (2^60 + 1 - 2^60
is 1; a `HUGEINT` overflow raises as in SQL), and division by zero gives DuckDB's `inf`, `nan` or NULL. The model reads
every number as `DOUBLE`, so a coefficient or bound beyond 2^53 is rounded there.

**Update.** `../../language/functions.md`, `../../language/guide.md` §3 and §7.2, `../../language/roadmap.md`.
