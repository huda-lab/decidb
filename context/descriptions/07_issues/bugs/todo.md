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

---

## Composed MIN/MAX with an entity-scoped variable crashes with an internal error

**Symptom.** `INTERNAL Error: Vector::Reference used on vector of different type`, raised
from `ExpressionExecutor::Execute` inside `PhysicalDecide::Finalize`.

**Reproduction.** Depots(depotID, opening_cost) joined to Routes(routeID, depotID, …):

```sql
-- fails
SELECT routeID FROM Depots D JOIN Routes T USING (depotID)
DECIDE D.open(BOOL) SUCH THAT SUM(open) >= 1
MINIMIZE SUM(opening_cost * open) + MAX(opening_cost * open);

-- succeeds — same shape, row-scoped variable
SELECT routeID FROM Depots D JOIN Routes T USING (depotID)
DECIDE x(INT) SUCH THAT x <= 5 AND SUM(x) >= 3
MINIMIZE SUM(unit_cost * x) + MAX(unit_cost * x);
```

**Scope.** The composed MIN/MAX shape (`SUM(...) + MIN/MAX(...)` as additive siblings in
one clause) breaks when the variable is **entity-scoped**; the same shape with a row-scoped
variable works. Affects both the objective and the constraint form. Independent of the
additive-coefficient bug above — the coefficient here is a bare column.

**Where to look.** `RewriteComposedMinMaxObjective` / `RewriteComposedMinMaxInConstraint`
(`src/optimizer/decide/decide_optimizer.cpp`) and the `composed_minmax_constraints` block
in `src/execution/operator/decide/physical_decide.cpp`. That block evaluates term
coefficients through its own `EvaluateTermCoefs` rather than the shared batched path, and
builds a fresh `DataChunk` per term from `transformed.return_type`; a BOOLEAN
entity-scoped decision column reaching a chunk typed for the coefficient's type is the
likely mismatch.

**Consequence for relation-qualified reducers.** `ComposedMinMaxTerm`
(`logical_decide.hpp`) carries no `qualifier_scope_idx`, so a qualified reducer composed
with MIN/MAX would drop its qualifier. That cannot produce a wrong answer today only
because this crash fires first — an accident, not a guard. See
`03_expressivity/decide/todo.md` → "Qualified reducer composed with MIN/MAX in the same
clause".

_Discovered 2026-08-08, while checking whether a relation-qualified reducer could silently
lose its qualifier in the composed path. Not caused by that work — the unqualified form
fails identically._
