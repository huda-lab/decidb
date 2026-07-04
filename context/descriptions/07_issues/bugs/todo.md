# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved bugs are moved to `done.md`.

---

## Infeasible diagnosis renders `=` clauses as `x == 5` (non-SQL spelling)

- **Location:** `src/decidb/utility/decide_diagnostic_engines.cpp:296` (`SenseStr` returns `"=="` for `'='`).
- **Symptom:** `DECIDE x IS REAL SUCH THAT x = 10 AND x = 5 MAXIMIZE SUM(x)` → relation subject `x == 5`,
  suggestion `x == 10`. The user wrote `=`; `==` is a DuckDB-accepted alias but not the SQL spelling the
  user typed, violating the "output is for SQL users" principle.
- **Why it matters:** the suggested edit doesn't match the query text, so it can't be located by
  copy/paste; the E1 apply-the-fix test harness needs a `subject_to_sql` override to find the clause.
- **Ruled out:** nothing — likely a one-token fix, but check for tests asserting `==` before changing.
- **Found:** 2026-07-04, while wiring the E1 apply-the-fix harness (Batch 1: A1/C3/E1/E3).

## Quadratic infeasible diagnosis emits a zero-amount virtual-offset edit (and leaks implicit CAST)

- **Location:** infeasible engine readback (`ReadElasticEdits` path) for the QCQP case; repro:
  `SELECT id, x FROM (VALUES (1,10)) t(id, lo) DECIDE x IS REAL SUCH THAT POWER(x,2) <= 4 AND x >= lo MAXIMIZE SUM(x)` (Gurobi).
- **Symptom:** besides the correct `POWER(x, 2) <= 4` → `<= 100` edit, the relation reports a second edit
  `x >= CAST(lo AS DOUBLE)` → `x >= CAST(lo AS DOUBLE) - 0` with `amount = 0` — a no-op edit row. Two warts:
  (1) an amount-0 edit should not be reported at all (likely a tiny slack that survives the
  `DIAGNOSTIC_RAY_EPSILON` filter on the QCQP re-solve but rounds to 0 in `FormatNum`);
  (2) the subject leaks the binder's implicit `CAST(lo AS DOUBLE)` instead of rendering `x >= lo`.
- **Why it matters:** a no-op suggestion tells the user to change nothing, and the suggested text is not
  even appliable SQL — `SUCH THAT` rejects explicit CAST (`Binder Error: SUCH THAT clause does not support
  'CAST(lo AS DOUBLE)'`), so a user pasting the suggestion back gets a binder error. The E1 harness
  quarantines this edit in `test_infeasible_quadratic_loosens_linear_rhs` (drop that filter when fixing).
- **Found:** 2026-07-04, while wiring the E1 apply-the-fix harness (Batch 1: A1/C3/E1/E3).
