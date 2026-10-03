# Current-syntax baseline corpus (2026-09-30)

This is the pre-feature solver baseline on `direct-solve-prototype` at
`721d49ba43d10fd4e9e15572508f10a873970c3b`, built with `make release
BUILD_JOBS=4`. Run each complete SQL string with `build/release/decidb -csv -c
"SQL"`; `DECIDB_FORCE_SOLVER=highs` or `gurobi` pins a backend. The default,
HiGHS, and Gurobi all ran on this host. These are historical solver-only
observations; the permanent direct path tests now live in
`test/decide/tests/test_direct_solve.py`.

## Small common fixture

In the tables below, `R` stands for this **literal SQL text** (substitute it in
each query). Declaring `x(BOOL)` creates one row-scoped decision per input row:

```sql
(VALUES (1, 9.0::DOUBLE), (2, 10.0::DOUBLE)) t(id, p)
```

The common suffix `DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 MAXIMIZE SUM(p*x)`
returns `id INTEGER, x INTEGER` rows `(1,0), (2,1)` when selecting `id,x`
and ordering by `id`. The first rule should be selected only for a full proved
shape; `off` and every miss use the solver. Keep the SQL fixture exact when
promoting these to executable tests.

| ID | SQL after `FROM R` (unless a full query is shown) | Current solver result | Direct `auto` expectation |
|---|---|---|---|
| P1 | `DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(p*x) ORDER BY id` | `(1,0),(2,1)` | S1 hit |
| P2 | `DECIDE x(BOOL) SUCH THAT SUM(x)<=0 MAXIMIZE SUM(p*x) ORDER BY id` | `(1,0),(2,0)` | S1 hit only after zero-capacity guard/assignment test |
| P3 | `DECIDE x(BOOL) SUCH THAT SUM(x)<=3 MAXIMIZE SUM(p*x) ORDER BY id` | `(1,1),(2,1)` | S1 hit |
| P4 | `DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MINIMIZE SUM(p*x) ORDER BY id` | `(1,0),(2,0)` | S1 hit; positive scores cannot improve a minimum |
| P5 | `DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(p*x) ORDER BY id` on two identical `p=9` rows | One row selected; observed `(1,1),(2,0)` | S1 hit; only objective and cardinality are fixed at a tie |
| P6 | `DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(p*x)` on an empty input | Zero rows; outer `COUNT(*)` is `0` | S1 hit |
| P7 | `DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MINIMIZE SUM(p*x) ORDER BY id` on `p=(-9,-10)` | `(1,0),(2,1)` | S1 hit |
| P8 | Write capacity `9007199254740992` (`2^53`) | `(1,1),(2,1)` | S1 hit if the bound constant is exactly represented |

P1–P3, P7, and P8 were repeated with both forced backends. P4 uses the same `R`
fixture. P5 uses `(VALUES (1,9.0::DOUBLE),(2,9.0::DOUBLE)) t(id,p)`. For P6,
append `WHERE id<0` before `DECIDE` and put `SELECT COUNT(*) AS n` above it;
the exact query run was:

```sql
SELECT COUNT(*) AS n
FROM (VALUES (1,9.0::DOUBLE),(2,10.0::DOUBLE)) t(id,p)
WHERE id<0
DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(p*x);
```

### One-condition-away cases

These are valid DECIDE queries recorded before the direct rule. The table
preserves the initial narrow rule's admission decisions; several shapes were
subsequently proved and promoted. The current near-miss tests are in
`test/decide/tests/test_direct_solve.py`. All unspecified parts use P1.

| ID | Change to P1 | Solver result | Initial miss reason |
|---|---|---|---|
| M1 | Add `y(BOOL)` to the declaration; select `id,x,y` | Observed `(1,0,0),(2,1,0)`; `y` is unconstrained by the objective | Two decisions |
| M2 | Add `AND x<=1` | `(1,0),(2,1)` | Extra constraint, even if redundant |
| M3 | Write `SUM(x)<=1 WHEN id=1` | `(1,1),(2,1)` | `WHEN` changes relevant rows |
| M4 | Write `SUM(x)<=1 PER id` | `(1,1),(2,1)` | Per-key capacity |
| M5 | Write `SUM(x+1)<=3` | `(1,0),(2,1)` | Data-only term needs row-count evaluation |
| M6 | Write `SUM(id*x)<=2` | `(1,0),(2,1)` | Weighted capacity |
| M7 | Use three-column input `(id,p,cap)` with `cap=1` on both rows; write `SUM(x)<=cap` | `(1,0),(2,1)` | Data-valued bound |
| M8 | Write capacity `1.5` | `(1,0),(2,1)` | Fractional bound outside initial admission |
| M9 | Declare `t.x(BOOL)` with the rest of P1 unchanged | `(1,0),(2,1)` | Entity-scoped decision |
| M10 | From M9, use `SUM(t: x)<=1` in the constraint | `(1,0),(2,1)` | Qualified reducer |
| M11 | Add objective offset `+1` to P1 | `(1,0),(2,1)` | Admit only after a complete plan-time-offset proof |
| M12 | Write capacity `-1` | Infeasible error | Negative bound |
| M13 | Use `SUM((p+random())*x)` as the objective | Observed `(1,0),(2,1)`; the fixture's score intervals do not overlap | Volatile score |
| M14 | Use `SUM(MAX(p*x)) PER id` as the objective | `(1,0),(2,1)` | Nested reducer |
| M15 | Add `+SUM(x)` to the objective | `(1,0),(2,1)` | Additional objective factor; now admitted |
| M16 | Omit the objective | Observed `(1,0),(2,0)`; any feasible vector is allowed | Feasibility problem, not S1 optimization |
| M17 | Write capacity `9007199254740993` (`2^53+1`) | `(1,1),(2,1)` on two rows | Beyond the first rule's reliable DOUBLE admission |

M1–M17 ran on the default backend. M8, M12, and M17 also ran with forced HiGHS and
Gurobi. M10 is one additional change from M9; the other rows are one change
from P1. The rule tests matching on the **complete canonical trees**. Shapes
such as M11 and M15 were admitted only after their whole objective was proved;
a familiar `SUM(x)` or `SUM(p*x)` subtree is insufficient. M10 requires the table-scoped
declaration; `DECIDE x(BOOL)` with `SUM(t: x)` is a binder error, not a valid
near miss.

## Parent and invalid-value outcomes

| Case | Complete SQL or source change | Current outcome |
|---|---|---|
| Outer filter | `SELECT id,x FROM (SELECT id,x FROM R DECIDE x(BOOL) SUCH THAT SUM(x)<=1 MAXIMIZE SUM(p*x)) d WHERE id=1` | `(1,0)`; the parent did not change the two-row optimization input |
| Unused `x`, zero capacity | `SELECT id FROM (VALUES (1,1.0::DOUBLE),(2,NULL::DOUBLE)) t(id,p) DECIDE x(BOOL) SUCH THAT SUM(x)<=0 MAXIMIZE SUM(p*x)` | `Invalid Input Error`, naming NULL `p` |
| Non-finite score | Replace the second `p` above by `'NaN'::DOUBLE`, `'Infinity'::DOUBLE`, or `'-Infinity'::DOUBLE` | `Invalid Input Error`, invalid objective coefficient |
| Bad cast | Use `CAST(p AS DOUBLE)` for two strings `'1.0'` and `'bad'`, with unused `x` and capacity zero | DuckDB `Conversion Error` |
| Late NULL | 5,000 input rows with only `id=4999` having NULL score; outer `LIMIT 1`, `COUNT(*)`, or `WHERE id=0` | Error from invalid score; confirmed default and HiGHS under `LIMIT 1` |
| Source filter | Same 5,000 rows, with input `WHERE id<4999` | Succeeds; removed row has no DECIDE obligation |
| Unused NULL source column | Add `extra=NULL::INTEGER` to the first row while keeping finite `p`; select `id,extra,x` | `(1,NULL,0),(2,7,1)`; unrelated source NULL is allowed |
| Outer `LIMIT 0` | Append `LIMIT 0` to the unused-`x` NULL query | No execution or error |

The first-rule `BOOL` result type is SQL `INTEGER`, not `BOOLEAN`. Every input
row remains in the output, including unselected rows and duplicate-valued
rows. Parent filters and joins apply to the completed assignment.

## Numeric comparison rule for the coming tests

The independent oracle decides the mathematically optimal finite-DOUBLE
assignment for the admitted cardinality problem. Test exact row count, output
types, Boolean domain, and `SUM(x)<=K`; allow either assignment at a boundary
tie. For ordinary, well-separated scores, compare the primary objective
against both backends. Near zero, backend assignment can differ: on `(1e-12,
0)` both backends returned `(0,0)` here, although selecting the tiny positive
score is mathematically better; at `1e-9` Gurobi selected it and HiGHS did not.
Record absolute and relative objective gaps and solver status rather than
asserting vector equality. Set the final backend-differential tolerance from
measured backend behavior on the permanent fixture set, not from this small
sample or an arbitrary global constant.
