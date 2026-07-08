# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved bugs are moved to `done.md`.

---

## Infeasible diagnosis renders a data-weighted AVG clause as `SUM(...)`

**Location**: `FormatLhs` / `FormatSumLhs` fold in the infeasible engine's clause reconstruction (see `08_query_diagnostics/infeasible/done.md` § I2.d), likely the `provenance.avg_scaled` collapse path vs. the data-varying-coefficient `FormatSumLhs` path.

**Symptom**: on an infeasible `SUCH THAT AVG(buy * p_retailprice) >= 5000` (BOOLEAN `buy`, TPC-H `part`, 8 rows), the diagnosis names the clause `SUM(buy * p_retailprice) >= 5000` and suggests `SUM(buy * p_retailprice) >= 904.5`. The **amount is in AVG units** (904.5 is the achievable AVG; the achievable SUM is 7236), so the suggestion as displayed is a *different, wrong* constraint — a user who applies the literal edit `SUM(...) >= 904.5` writes a constraint that does not correspond to the diagnosis. A pure `AVG(buy) >= 5` renders correctly as `AVG(buy) >= 1`, so the break is specific to **data-varying coefficients** inside AVG: the avg-scaled row appears to fall through to the `FormatSumLhs` data-varying rendering (`SUM(var * col)`) instead of the AVG collapse.

**Why it matters**: the suggested_change is the product's core promise (the smallest edit, quoted in the user's own units and syntax); here it's mislabeled and numerically inconsistent with the shown aggregate.

**Repro**: `SELECT p_partkey, buy FROM part WHERE p_partkey <= 8 DECIDE buy IS BOOLEAN SUCH THAT AVG(buy * p_retailprice) >= 5000 MAXIMIZE SUM(buy);` on the repo `decidb.db`, Gurobi, `diagnose_decide='auto'`.

**Discovered**: 2026-07-08, while generating the query-diagnostics README example catalog (mini-report task).

---

## Data-only `AVG(col)` as a coefficient multiplier is silently miscomputed

**Location**: `src/planner/expression_binder/decide_binder.cpp` (SUM-argument validation, `avg` branch ~line 378) + AVG→SUM optimizer rewrite.

**Symptom**: `SUCH THAT SUM(avg(p) * x) <= K` where `avg(p)` references only table columns (no DECIDE variable) is accepted and solved, but the coefficient applied is *not* `avg(p)`. On `(VALUES (1,10.0),(2,20.0))`, `avg(p) = 15`, so `SUM(avg(p)*x) <= 3` should force `x = [0,0]`; instead it returns `x = [1,1]` (effective coefficient ≈ 1, as if the AVG collapsed to a per-row identity). With bound `<= 0` it does return `[0,0]`, so the constraint is not fully dropped — the coefficient value is simply wrong.

**Asymmetry**: the analogous `SUM(sum(p) * x)` is *correctly rejected* at bind time (`Nested SUM() inside DECIDE expression must reference a DECIDE variable`). Only the AVG form slips through — almost certainly because the AVG→SUM algebraic rewrite mishandles a data-only nested AVG. MIN/MAX data-only coefficients should be checked too.

**Why it matters**: wrong results, silently. A user who writes a data-only AVG as a coefficient gets a plausible-looking but incorrect optimum with no error.

**Ruled out**: not introduced by the data-only scalar-function fold (2026-07-06) — that change adds fallthrough branches strictly *after* the aggregate-handling branches (binder 378-388 and symbolic `avg` branch always return before reaching the new code), so the AVG path is untouched. Verified the same wrong behavior is reachable independently of that feature.

**Next**: decide whether data-only aggregate *coefficients* (as opposed to the supported data-only aggregate *RHS*, see `03_expressivity/sql_functions/done.md`) should be rejected uniformly (like SUM) or given well-defined broadcast semantics; fix the AVG-rewrite path accordingly. Add negative/positive tests mirroring the SUM rejection.

**Discovered**: 2026-07-06, while landing the data-only scalar-function fold (negative-control probing).
