# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved bugs are moved to `done.md`.

---

## An additive coefficient inside a MIN/MAX objective fails to load into Gurobi

**Symptom.** `Invalid Input Error: Failed to add constraint to Gurobi: Problem adding
constraints`. The query never reaches the solver.

**Reproduction** (TPC-H `nation`, 25 rows, no join, `build/release/decidb`):

```sql
-- fails
SELECT keepN FROM nation n DECIDE n.keepN(BOOL)
SUCH THAT SUM(keepN) >= 1 MAXIMIZE MIN((n.n_nationkey + 1) * keepN);

-- succeeds — same query, coefficient has no additive part
SELECT keepN FROM nation n DECIDE n.keepN(BOOL)
SUCH THAT SUM(keepN) >= 1 MAXIMIZE MIN(n.n_nationkey * keepN);
```

**Scope.** The trigger is an **additive** coefficient (`col + 1`) inside a MIN or MAX
objective. Narrowed by bisection:

- fails: `MIN((col + 1) * x)`, `MAX((col + 1) * x)` — both senses, both aggregates
- fails for row-scoped (`x(BOOL)`) and entity-scoped (`n.x(BOOL)`) variables alike
- fails with no join present, so it is not a join/multiplicity effect
- succeeds: `MIN(col * x)`, `MIN(2 * col * x)` — multiplicative scaling is fine
- succeeds: `SUM((col + 1) * x)` — only MIN/MAX is affected

**Where to look.** `(col + 1) * x` distributes into two additive terms (`col*x + 1*x`).
SUM absorbs both into one row; the MIN/MAX objective path builds a `z` auxiliary plus
per-row linking constraints (`RewriteMinMaxObjective` in
`src/optimizer/decide/decide_optimizer.cpp`, the flat/composed MIN/MAX blocks in
`src/execution/operator/decide/physical_decide.cpp`), and one of the emitted rows is
malformed — most likely duplicate column indices or a mismatched
indices/coefficients length reaching `GurobiSession::Load`
(`src/decidb/gurobi/gurobi_solver.cpp:168`), which is what `addconstr` rejects. The
Gurobi error code is swallowed; surfacing it would localize this quickly.

_Discovered 2026-08-08, while writing MIN/MAX coverage for relation-qualified reducers
(A5). Not caused by that work — both the qualified and unqualified forms fail
identically. `test_qualified_reducer.py::test_qualified_minmax` sidesteps it by using a
bare-column coefficient._
