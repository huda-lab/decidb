# Code Quality Issues — Open

Code-quality issues (duplication, dead code, fragile patterns, unclear naming, missing test coverage) spotted opportunistically while working on other tasks. Not bugs — nothing here produces wrong results today; these are things that make the code harder to change safely. Actual bugs go to `../bugs/todo.md`.

Each entry: short title, location (`file:line`), what's wrong, why it matters, and when/during which task it was discovered.

Resolved entries are removed; if the fix taught a generalizable lesson, record it in `.claude/lessons.md`.

---

## Stale diagnosis lingers after a later successful DECIDE

- **Location:** `src/decidb/utility/decide_diagnostic.cpp` (stash never cleared); read path `DecideDiagnosticsInit` / `DecideDiagnosticsFunction`; producer in `src/execution/operator/decide/physical_decide.cpp::Finalize`.
- **What's wrong:** the per-connection `DecideDiagnosticState.latest` stash is only ever *written* (on a diagnosed failure) — never reset. A successful DECIDE on the same connection does not clear it, so `SELECT * FROM decide_diagnostics()` keeps returning the previous failure's diagnosis after the user has fixed the query and re-run it successfully. Verified manually: fail (unbounded, stash) → succeed → `decide_diagnostics()` still shows the unbounded scaffold row.
- **Why it matters:** misleading UX — a stale diagnosis reads as if it describes the most recent solve. Technically within the documented "most recent diagnosis" contract (F5), so it is not a wrong-results bug, but the contract itself is arguably the wrong call. Candidate fix: clear/invalidate the stash on an OPTIMAL DECIDE finalize (and/or stamp each diagnosis with a statement id). Design decision for the user — do not pick unilaterally.
- **Discovered:** 2026-06-16, verifying F2/F4/F5 (query diagnostics).
