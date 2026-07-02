# Code Quality Issues — Open

Code-quality issues (duplication, dead code, fragile patterns, unclear naming, missing test coverage) spotted opportunistically while working on other tasks. Not bugs — nothing here produces wrong results today; these are things that make the code harder to change safely. Actual bugs go to `../bugs/todo.md`.

Each entry: short title, location (`file:line`), what's wrong, why it matters, and when/during which task it was discovered.

Resolved entries are removed; if the fix taught a generalizable lesson, record it in `.claude/lessons.md`.

---

## Infeasible headline enumerates every failing PER group (no cap)

- **Location**: infeasible headline builder, grouped-clause phrase (`src/decidb/utility/decide_diagnostic.cpp`, `QuotedGroupList` / the "grouped clause … for groups …" phrase)
- **Discovered**: 2026-07-02 while stress-testing per-group diagnostics on real TPC-H data
- **What's wrong**: when many groups fail, the stderr headline lists **all** of them inline. `SUM(x) >= 5 PER l_orderkey` over 400 orders emits a headline naming 59 group keys (`… for groups 2, 4, 5, 6, 33, … and 391.`), a multi-line wall of text. The structured `decide_diagnostics()` relation is fine — this is headline-only.
- **Why it matters**: violates the "concise, actionable" output principle (CLAUDE.md). A headline should summarize (`… for 59 groups, e.g. 2, 4, 5 — see decide_diagnostics()`), not enumerate. The full per-group detail already lives in the relation.
- **Where to look next**: cap the enumerated group list (first K + "and N more") in the headline path only; leave the relation untouched.

---

## Infeasible `drop` edit is duplicated once per per-row `<>` expansion

- **Location**: elastic-engine removal/`drop` edit emission (`src/decidb/utility/decide_diagnostic_engines.cpp`, `<>` removal rows keyed by `indicator_col`)
- **Discovered**: 2026-07-02 while stress-testing diagnostics on real TPC-H data
- **What's wrong**: a single `<>` constraint that expands per row emits one identical `drop` EAV row per input row. `x <> 1` over an order with 6 line items produces 6 identical `clause=x <> 1 / edit_kind=drop` rows in `decide_diagnostics()`.
- **Reproduction**:
  ```sql
  PRAGMA diagnose_decide='auto';
  SELECT l_orderkey, x FROM lineitem WHERE l_orderkey = 1
  DECIDE x IS BOOLEAN SUCH THAT x <> 0 AND x <> 1 MINIMIZE SUM(x);
  SELECT * FROM decide_diagnostics();   -- x <> 1 / drop appears 6× (order 1 has 6 lines)
  ```
- **Why it matters**: the relation should carry one edit per user clause; the duplication is noise that scales with row count and makes the drop suggestion look like many distinct edits.
- **Where to look next**: dedupe `drop` edits by `(clause_id, indicator label)` before emitting EAV rows, the way per-group loosens are keyed by `(clause_id, group_key)`.
