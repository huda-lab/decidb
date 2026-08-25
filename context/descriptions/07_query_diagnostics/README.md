# Query Diagnostics

Turning failed or useless DECIDE solves into actionable diagnoses. SQL always
returns rows; a DECIDE *solve* can fail in ways a lookup can't — and an undiagnosed
failure is a dead end (a static error paragraph, a timeout, or a silently
arbitrary answer). This area replaces each dead end with a **diagnosis**: *why*
it failed and the **least-change** edit that restores a usable solution.

## The map — everything flows through the router

Every solve outcome flows through one dispatch tree (the **router**): it inspects the
solver status — plus a couple of sub-signals (is there a recession ray? an
incumbent?) — and routes to exactly one terminal. The states below are its leaves.

```
                              solve result
                                   │
        ┌──────────────────────────┼──────────────────────────┐
     solved                      failed                    time_limit
        │                          │                            │
  (success,         ┌──────────────┼──────────────┐       ┌─────┴─────┐
 no diagnosis)   unbounded     infeasible       inf/unb  incumbent   no sol
                    │              │               │         │          │
                 find ray       elastic        check ray   report     report
                    │              │               │      incum+gap    slow
                  report         report      ┌──────┴──────┐
                                          found        not found
                                            │              │
                                   "add a bound —       elastic
                                    may still be          │
                                    infeasible"         report
```

- `[router/](router/)` — the **spine**: the dispatch tree above and the inf/unb
  `check ray` disambiguation. All four terminals are wired — solved, unbounded,
  infeasible, and time_limit (the slow engine, R6) — each an engine dropped behind an
  existing classifier leaf without editing the classifier.
- `[unbounded/](unbounded/)` — terminal `failed → unbounded`: names the escaping
  variable via the ray and prescribes a bound (**tighten** a too-open region).
- `[infeasible/](infeasible/)` ★ — terminal `failed → infeasible`: elastic
  relaxation (**loosen** a too-small region). The flagship engine — fully shipped
  (I1–I5, aggregate `<>` removal, and the T3 two-mode slack-scope policy: `query`
  folds each knob to one SQL edit / turns data conflicts into virtual offsets,
  `expanded` exposes the per-row / per-group profile).
- `[slow/](slow/)` — terminal `time_limit`: **shipped**. A solve that hits the limit
  returns `SolverStatus::TIME_LIMIT`, prints a plain-language checkpoint report, and — per
  the `decide_on_timeout` pragma (ask / error / continue) — resumes the **same warm solver**
  for more wall-clock, returning the best-so-far rows when the user stops. Not a
  relax/reformulate engine; it reports and continues. (`off` is a master mute → plain
  error.)
- `[foundations/](foundations/)` — the **substrate** every terminal sits on (not
  a terminal): structured solver result, constraint + variable provenance, the
  `diagnose_decide` gate, the solver-behavior reference, the reporting relation.

Infeasible and unbounded are mirror images — loosen a too-small region vs. tighten a
too-open one; slow is a runtime event masking the other states.

## Principles

- **Least-change** — propose the smallest edit, not a rewrite.
- **On by default** — `PRAGMA diagnose_decide` is `auto` by default: a failed solve
  is diagnosed automatically wherever an engine exists. Set `off` to suppress and
  get the plain static error. We never edit the user's query; a diagnosis only ever
  *describes* the failure and prescribes a remedy.
- **Solver-agnostic** — everything works on Gurobi and HiGHS. We build the
  elastic model in our own model builder so both backends solve it natively;
  Gurobi `feasRelax` is an *accelerator*, never a dependency.
- **A native construct must still be reachable** — when a backend expresses a construct
  itself, the rows it would have produced are gone, and so is anything diagnosis could
  slacken. That is why `<>` is stated with *indicator constraints* rather than a general
  constraint: an indicator constraint still carries a row, so the remove-only dial wires
  its binary into the implied row exactly as into a matrix row and the diagnosis is
  unchanged. ABS and MIN/MAX lose only structural rows — the user's own clause row
  survives either way — so the choice only bites where the clause *is* its encoding.
- **A repair the model cannot represent is a repair the diagnosis cannot offer** — the
  cost of the bullet above, stated plainly. The rows a lowering emits are sized from the
  decision box *as the query states it*, and the elastic engine repairs by **widening**
  that box: a ceiling baked in at the old width silently rules the widened repair out,
  so the engine reports whatever else it can still see. That is worse advice, and where
  one arm bakes in nothing it is worse advice **on one host only**. The rule that closes
  it: a clause demanding more of an auxiliary than the box can supply sizes that
  auxiliary itself. It fires only when the demand exceeds the box — only when the clause
  cannot be met as written — so no query that solves has its Big-M loosened, which the
  golden corpus checks. Applied to ABS and MIN/MAX; `<>`, McCormick and `norm` have
  encodings of the same shape and are **not** covered, see
  [`infeasible/todo.md`](infeasible/todo.md).
- **Diagnosed on the solver that failed** — every re-solve (the elastic model, the
  `INF_OR_UNBD` probe, the unbounded-ray fallback) runs on the backend the *primary*
  solve ran on, resolved from `PhysicalDecide::solver_backend_name` through
  `PlannedSolverBackend()`. That choice was made once,
  at plan time, before any rewrite. Diagnosis never re-selects: a second, independently
  answered selection would diagnose a failure on a solver that did not produce it, and
  once selection depends on the model the two answers can genuinely differ.
- **Differential testing** — every phase tests against `oracle_solver` on
  constructed cases, never hand-computed answers.

## Invocation — `PRAGMA diagnose_decide`

Sticky session pragma with two modes: `auto` (default — diagnose whichever failed
state the solve lands in, wherever an engine exists) and `off` (suppress diagnosis;
reproduce the plain static solver error). Diagnosis only ever runs when the solve
*actually* fails, so leaving `auto` on costs nothing on a successful solve.

A second sticky pragma, `diagnose_decide_infeasible_slack_scope` (`query` default /
`expanded`), selects how the infeasible engine reports a knob that fans out — one
folded SQL-level edit vs. the per-row / per-group profile. See `infeasible/done.md`.

---

# What users can expect

A DECIDE query can end four ways: it **solves**, it is **unbounded** (something can
grow forever), it is **infeasible** (the constraints contradict each other), or it is
**slow** (the time limit expires first). This tour shows what DeciDB says in each case.
Every failure explains itself in one line, names the offending part of *your* query,
and states the smallest edit that fixes it; the structured detail is always one
`SELECT * FROM decide_diagnostics();` away.

All outputs below are real captured runs against the TPC-H sample database
(`decidb.db`, scale 0.01 — `part` 2000 rows, `lineitem` 60k, `supplier` 100), Gurobi
backend, default settings unless a `PRAGMA`/`SET` line is shown. The slow examples set
the time limit to 1 second (`DECIDB_TIME_LIMIT=1`) so they reproduce in seconds.

## When it just works

A successful solve returns the decided rows like any SQL query. Diagnosis is on by
default but costs nothing here — it only runs when a solve fails — and
`decide_diagnostics()` stays empty.

```sql
SELECT p_partkey, p_retailprice, buy
FROM part
WHERE p_partkey <= 8
DECIDE buy(BOOL)
SUCH THAT SUM(buy * p_retailprice) <= 3000
MAXIMIZE SUM(buy * p_retailprice);
```

```
┌───────────┬───────────────┬─────┐
│ p_partkey │ p_retailprice │ buy │
├───────────┼───────────────┼─────┤
│ 1         │ 901.00        │ 0   │
│ 2         │ 902.00        │ 0   │
│ 3         │ 903.00        │ 0   │
│ 4         │ 904.00        │ 0   │
│ 5         │ 905.00        │ 0   │
│ 6         │ 906.00        │ 1   │
│ 7         │ 907.00        │ 1   │
│ 8         │ 908.00        │ 1   │
└───────────┴───────────────┴─────┘
```

---

## Unbounded — something can grow forever

The objective can improve without limit because nothing caps a variable. The fix is
always the same shape — add a bound — so the diagnosis focuses on telling you **which
variable** escapes and **which rows** it happens in. DeciDB never invents the cap
value; that number is yours to choose.

### U1 — a variable with no upper bound

Nothing stops `buy` from growing, so revenue grows with it.

```sql
SELECT p_partkey, buy
FROM part
WHERE p_partkey <= 8
DECIDE buy(REAL)
SUCH THAT buy >= 0
MAXIMIZE SUM(buy * p_retailprice);
```

```
Invalid Input Error: DECIDE optimization is unbounded: variable buy can grow without bound. Add an upper bound, e.g. SUCH THAT buy <= <cap>.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬───────────┬──────────────┬─────────┬───────────────┬────────────┐
│ diagnosis_id │   state   │ subject_kind │ subject │   attribute   │   value    │
├──────────────┼───────────┼──────────────┼─────────┼───────────────┼────────────┤
│ 1            │ unbounded │ variable     │ buy     │ grows_toward  │ +inf       │
│ 1            │ unbounded │ variable     │ buy     │ affected_rows │ all 8 rows │
└──────────────┴───────────┴──────────────┴─────────┴───────────────┴────────────┘
```

When several variables escape, each gets its own rows in the relation and the
headline lists them all. An INTEGER variable reports identically.

### U2 — a cap that skipped a slice of the data

Here a budget cap exists — but its `WHEN` exempts one manufacturer, and exactly that
slice escapes. The relation pinpoints it, turning "something is unbounded" into
"your cap misses Manufacturer#1".

```sql
SELECT p_partkey, p_mfgr, buy
FROM part
WHERE p_size <= 5
DECIDE buy(REAL)
SUCH THAT buy >= 0
     AND SUM(buy * p_retailprice) <= 100000 WHEN p_mfgr <> 'Manufacturer#1'
MAXIMIZE SUM(buy * p_retailprice * 0.10);
```

```
Invalid Input Error: DECIDE optimization is unbounded: variable buy can grow without bound. Add an upper bound, e.g. SUCH THAT buy <= <cap>.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬───────────┬──────────────┬─────────┬───────────────┬───────────────────────────────────────────────┐
│ diagnosis_id │   state   │ subject_kind │ subject │   attribute   │                     value                     │
├──────────────┼───────────┼──────────────┼─────────┼───────────────┼───────────────────────────────────────────────┤
│ 1            │ unbounded │ variable     │ buy     │ grows_toward  │ +inf                                          │
│ 1            │ unbounded │ variable     │ buy     │ affected_rows │ 29 of 29 rows where p_mfgr = 'Manufacturer#1' │
└──────────────┴───────────┴──────────────┴─────────┴───────────────┴───────────────────────────────────────────────┘
```

### U3 — an entity-level variable, characterized by a joined column

A per-supplier variable (`s.capacity`) escapes only for European suppliers — the
exempted region. The description works even though `r_name` arrives through two joins.

```sql
SELECT s.s_suppkey, r_name, capacity
FROM partsupp ps
JOIN supplier s ON ps.ps_suppkey = s.s_suppkey
JOIN nation n ON s.s_nationkey = n.n_nationkey
JOIN region r ON n.n_regionkey = r.r_regionkey
DECIDE s.capacity(REAL)
SUCH THAT capacity <= 1000 WHEN r_name <> 'EUROPE'
MAXIMIZE SUM(capacity * ps_supplycost);
```

```
Invalid Input Error: DECIDE optimization is unbounded: variable capacity can grow without bound. Add an upper bound, e.g. SUCH THAT capacity <= <cap>.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬───────────┬──────────────┬──────────┬───────────────────┬───────────────────────────────────────────┐
│ diagnosis_id │   state   │ subject_kind │ subject  │     attribute     │                   value                   │
├──────────────┼───────────┼──────────────┼──────────┼───────────────────┼───────────────────────────────────────────┤
│ 1            │ unbounded │ variable     │ capacity │ grows_toward      │ +inf                                      │
│ 1            │ unbounded │ variable     │ capacity │ affected_entities │ 20 of 20 entities where r_name = 'EUROPE' │
└──────────────┴───────────┴──────────────┴──────────┴───────────────────┴───────────────────────────────────────────┘
```

### U4 — when the variable can't be named

A non-linear objective (here a `POWER` term) prevents identifying the runaway
variable. The error says so honestly — the remedy is unchanged — and no detail
relation is stashed.

```sql
SELECT p_partkey, buy
FROM part
WHERE p_partkey <= 8
DECIDE buy(REAL)
SUCH THAT buy >= 0
MAXIMIZE SUM(buy * p_retailprice) + SUM(POWER(buy, 2));
```

```
Invalid Input Error: DECIDE optimization is unbounded: a non-linear term prevents naming the variable. Add an upper bound, e.g. SUCH THAT x <= <cap>.
```

---

## Infeasible — the constraints can't all hold at once

There is no assignment that satisfies every constraint. DeciDB finds the **smallest
edit** that restores a solution and quotes it back in your own syntax: which clause,
loosened to what, by how much — plus `achievable_objective`, the objective value you
would get after the edit. When more than one clause must give, each edit is listed
under the same `diagnosis_id`.

### I1 — two bounds that contradict

`make` must be at least 10 but at most 5. The diagnosis picks one side and states the
exact loosening.

```sql
SELECT p_partkey, make
FROM part
WHERE p_partkey <= 8
DECIDE make(INT)
SUCH THAT make >= 10 AND make <= 5
MAXIMIZE SUM(make);
```

```
Invalid Input Error: DECIDE optimization is infeasible: the constraints cannot all be satisfied at once; diagnosis points to clause `make <= 5`.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬────────────┬──────────────┬───────────┬──────────────────────┬────────────────┐
│ diagnosis_id │   state    │ subject_kind │  subject  │      attribute       │     value      │
├──────────────┼────────────┼──────────────┼───────────┼──────────────────────┼────────────────┤
│ 1            │ infeasible │ clause       │ make <= 5 │ edit_kind            │ loosen         │
│ 1            │ infeasible │ clause       │ make <= 5 │ suggested_change     │ make <= 10     │
│ 1            │ infeasible │ clause       │ make <= 5 │ amount               │ 5              │
│ 1            │ infeasible │ clause       │ make <= 5 │ edit_source          │ source_literal │
│ 1            │ infeasible │ clause       │ make <= 5 │ offset_scope         │ clause         │
│ 1            │ infeasible │ model        │ NULL      │ achievable_objective │ 80             │
└──────────────┴────────────┴──────────────┴───────────┴──────────────────────┴────────────────┘
```

### I2 — an impossible average target, suggested in your own units

Even buying every part, the average spend tops out at 904.5 — the target 5000 is out
of reach. The suggestion is quoted as an `AVG` bound, in the same units you wrote.

```sql
SELECT p_partkey, buy
FROM part
WHERE p_partkey <= 8
DECIDE buy(BOOL)
SUCH THAT AVG(buy * p_retailprice) >= 5000
MAXIMIZE SUM(buy);
```

```
Invalid Input Error: DECIDE optimization is infeasible: the constraints cannot all be satisfied at once; diagnosis points to clause `AVG(buy * p_retailprice) >= 5000`.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬────────────┬──────────────┬──────────────────────────────────┬──────────────────────┬───────────────────────────────────┐
│ diagnosis_id │   state    │ subject_kind │             subject              │      attribute       │               value               │
├──────────────┼────────────┼──────────────┼──────────────────────────────────┼──────────────────────┼───────────────────────────────────┤
│ 1            │ infeasible │ clause       │ AVG(buy * p_retailprice) >= 5000 │ edit_kind            │ loosen                            │
│ 1            │ infeasible │ clause       │ AVG(buy * p_retailprice) >= 5000 │ suggested_change     │ AVG(buy * p_retailprice) >= 904.5 │
│ 1            │ infeasible │ clause       │ AVG(buy * p_retailprice) >= 5000 │ amount               │ 4095.5                            │
│ 1            │ infeasible │ clause       │ AVG(buy * p_retailprice) >= 5000 │ edit_source          │ source_literal                    │
│ 1            │ infeasible │ clause       │ AVG(buy * p_retailprice) >= 5000 │ offset_scope         │ clause                            │
│ 1            │ infeasible │ model        │ NULL                             │ achievable_objective │ 8                                 │
└──────────────┴────────────┴──────────────┴──────────────────────────────────┴──────────────────────┴───────────────────────────────────┘
```

### I3 — when two constraints must both give

Buy at least 20 parts (only 8 exist) on a 900 budget (the cheapest part costs 901).
Neither edit alone restores a solution, so the diagnosis proposes a balanced pair.

```sql
SELECT p_partkey, buy
FROM part
WHERE p_partkey <= 8
DECIDE buy(BOOL)
SUCH THAT SUM(buy) >= 20 AND SUM(buy * p_retailprice) <= 900
MAXIMIZE SUM(buy);
```

```
Invalid Input Error: DECIDE optimization is infeasible: the constraints cannot all be satisfied at once; diagnosis points to clause `SUM(buy) >= 20` and clause `SUM(buy * p_retailprice) <= 900`.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬────────────┬──────────────┬─────────────────────────────────┬──────────────────────┬──────────────────────────────────┐
│ diagnosis_id │   state    │ subject_kind │             subject             │      attribute       │              value               │
├──────────────┼────────────┼──────────────┼─────────────────────────────────┼──────────────────────┼──────────────────────────────────┤
│ 1            │ infeasible │ clause       │ SUM(buy) >= 20                  │ edit_kind            │ loosen                           │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 20                  │ suggested_change     │ SUM(buy) >= 4                    │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 20                  │ amount               │ 16                               │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 20                  │ edit_source          │ source_literal                   │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 20                  │ offset_scope         │ clause                           │
│ 1            │ infeasible │ clause       │ SUM(buy * p_retailprice) <= 900 │ edit_kind            │ loosen                           │
│ 1            │ infeasible │ clause       │ SUM(buy * p_retailprice) <= 900 │ suggested_change     │ SUM(buy * p_retailprice) <= 3610 │
│ 1            │ infeasible │ clause       │ SUM(buy * p_retailprice) <= 900 │ amount               │ 2710                             │
│ 1            │ infeasible │ clause       │ SUM(buy * p_retailprice) <= 900 │ edit_source          │ source_literal                   │
│ 1            │ infeasible │ clause       │ SUM(buy * p_retailprice) <= 900 │ offset_scope         │ clause                           │
│ 1            │ infeasible │ model        │ NULL                            │ achievable_objective │ 4                                │
└──────────────┴────────────┴──────────────┴─────────────────────────────────┴──────────────────────┴──────────────────────────────────┘
```

### I4 — when only removing a clause helps

A BOOLEAN forbidden from being 0 *and* from being 1 has no legal value left. No
loosening fixes that; the diagnosis says which clause to drop (`edit_kind = drop`).

```sql
SELECT promo FROM part WHERE p_partkey = 1
DECIDE promo(BOOL)
SUCH THAT promo <> 0 AND promo <> 1
MINIMIZE SUM(promo);
```

```
Invalid Input Error: DECIDE optimization is infeasible: the constraints cannot all be satisfied at once; diagnosis points to clause `promo <> 0`.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬────────────┬──────────────┬────────────┬──────────────────────┬───────┐
│ diagnosis_id │   state    │ subject_kind │  subject   │      attribute       │ value │
├──────────────┼────────────┼──────────────┼────────────┼──────────────────────┼───────┤
│ 1            │ infeasible │ clause       │ promo <> 0 │ edit_kind            │ drop  │
│ 1            │ infeasible │ model        │ NULL       │ achievable_objective │ 0     │
└──────────────┴────────────┴──────────────┴────────────┴──────────────────────┴───────┘
```

### I5 — when the conflict is in your data

Ship at least the ordered quantity, but no more than the supplier has available — and
for two order lines, availability falls short of the order. There is no literal in the
query to edit, so the default (`query`) mode reports one **virtual offset** on the
clause — `+ 18` covers the worst shortfall (`edit_source = virtual_offset`).

```sql
SELECT l_orderkey, l_linenumber, ship
FROM lineitem
JOIN partsupp ON l_partkey = ps_partkey AND l_suppkey = ps_suppkey
WHERE l_orderkey IN (33, 295)
DECIDE ship(INT)
SUCH THAT ship >= l_quantity AND ship <= ps_availqty
MAXIMIZE SUM(ship);
```

```
Invalid Input Error: DECIDE optimization is infeasible: the constraints cannot all be satisfied at once; diagnosis points to clause `ship <= ps_availqty`.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬────────────┬──────────────┬─────────────────────┬──────────────────────┬──────────────────────────┐
│ diagnosis_id │   state    │ subject_kind │       subject       │      attribute       │          value           │
├──────────────┼────────────┼──────────────┼─────────────────────┼──────────────────────┼──────────────────────────┤
│ 1            │ infeasible │ clause       │ ship <= ps_availqty │ edit_kind            │ loosen                   │
│ 1            │ infeasible │ clause       │ ship <= ps_availqty │ suggested_change     │ ship <= ps_availqty + 18 │
│ 1            │ infeasible │ clause       │ ship <= ps_availqty │ amount               │ 18                       │
│ 1            │ infeasible │ clause       │ ship <= ps_availqty │ edit_source          │ virtual_offset           │
│ 1            │ infeasible │ clause       │ ship <= ps_availqty │ offset_scope         │ clause                   │
│ 1            │ infeasible │ model        │ NULL                │ achievable_objective │ 35064                    │
└──────────────┴────────────┴──────────────┴─────────────────────┴──────────────────────┴──────────────────────────┘
```

To see exactly which rows conflict instead, set
`PRAGMA diagnose_decide_infeasible_slack_scope='expanded'` — one edit per conflicting
row, at its own row's values:

```
Invalid Input Error: DECIDE optimization is infeasible: the constraints cannot all be satisfied at once; diagnosis points to clause `ship <= 27` and clause `ship <= 11`.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬────────────┬──────────────┬────────────┬──────────────────────┬──────────────┐
│ diagnosis_id │   state    │ subject_kind │  subject   │      attribute       │    value     │
├──────────────┼────────────┼──────────────┼────────────┼──────────────────────┼──────────────┤
│ 1            │ infeasible │ clause       │ ship <= 27 │ edit_kind            │ loosen       │
│ 1            │ infeasible │ clause       │ ship <= 27 │ suggested_change     │ ship <= 32   │
│ 1            │ infeasible │ clause       │ ship <= 27 │ amount               │ 5            │
│ 1            │ infeasible │ clause       │ ship <= 27 │ edit_source          │ expanded_row │
│ 1            │ infeasible │ clause       │ ship <= 27 │ offset_scope         │ row          │
│ 1            │ infeasible │ clause       │ ship <= 11 │ edit_kind            │ loosen       │
│ 1            │ infeasible │ clause       │ ship <= 11 │ suggested_change     │ ship <= 29   │
│ 1            │ infeasible │ clause       │ ship <= 11 │ amount               │ 18           │
│ 1            │ infeasible │ clause       │ ship <= 11 │ edit_source          │ expanded_row │
│ 1            │ infeasible │ clause       │ ship <= 11 │ offset_scope         │ row          │
│ 1            │ infeasible │ model        │ NULL       │ achievable_objective │ 34943        │
└──────────────┴────────────┴──────────────┴────────────┴──────────────────────┴──────────────┘
```

### I6 — a PER group that can't keep up

"Pick at least 3 parts per manufacturer" — but in this subset Manufacturer#2 has only
2 parts and Manufacturer#5 only 1. The clause is one SQL literal, so the default
(`query`) mode folds all groups into the one edit that fixes the worst of them:

```sql
SELECT p_partkey, p_mfgr, buy
FROM part
WHERE p_partkey <= 12
DECIDE buy(BOOL)
SUCH THAT SUM(buy) >= 3 PER p_mfgr
MAXIMIZE SUM(buy);
```

```
Invalid Input Error: DECIDE optimization is infeasible: the constraints cannot all be satisfied at once; diagnosis points to clause `SUM(buy) >= 3 PER p_mfgr`.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬────────────┬──────────────┬──────────────────────────┬──────────────────────┬──────────────────────────┐
│ diagnosis_id │   state    │ subject_kind │         subject          │      attribute       │          value           │
├──────────────┼────────────┼──────────────┼──────────────────────────┼──────────────────────┼──────────────────────────┤
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr │ edit_kind            │ loosen                   │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr │ suggested_change     │ SUM(buy) >= 1 PER p_mfgr │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr │ amount               │ 2                        │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr │ edit_source          │ source_literal           │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr │ offset_scope         │ clause                   │
│ 1            │ infeasible │ model        │ NULL                     │ achievable_objective │ 12                       │
└──────────────┴────────────┴──────────────┴──────────────────────────┴──────────────────────┴──────────────────────────┘
```

In `expanded` mode the headline names the failing groups and each gets its own edit,
sized to its own shortfall:

```
Invalid Input Error: DECIDE optimization is infeasible: the constraints cannot all be satisfied at once; diagnosis points to grouped clause `SUM(buy) >= 3 PER p_mfgr` for groups `Manufacturer#2` and `Manufacturer#5`.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬────────────┬──────────────┬──────────────────────────────────────────────────┬──────────────────────┬──────────────────────────┐
│ diagnosis_id │   state    │ subject_kind │                     subject                      │      attribute       │          value           │
├──────────────┼────────────┼──────────────┼──────────────────────────────────────────────────┼──────────────────────┼──────────────────────────┤
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#2] │ edit_kind            │ loosen                   │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#2] │ suggested_change     │ SUM(buy) >= 2 PER p_mfgr │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#2] │ amount               │ 1                        │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#2] │ group                │ Manufacturer#2           │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#2] │ edit_source          │ expanded_group           │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#2] │ offset_scope         │ group                    │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#5] │ edit_kind            │ loosen                   │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#5] │ suggested_change     │ SUM(buy) >= 1 PER p_mfgr │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#5] │ amount               │ 2                        │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#5] │ group                │ Manufacturer#5           │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#5] │ edit_source          │ expanded_group           │
│ 1            │ infeasible │ clause       │ SUM(buy) >= 3 PER p_mfgr [group: Manufacturer#5] │ offset_scope         │ group                    │
│ 1            │ infeasible │ model        │ NULL                                             │ achievable_objective │ 12                       │
└──────────────┴────────────┴──────────────┴──────────────────────────────────────────────────┴──────────────────────┴──────────────────────────┘
```

Two edge behaviors worth knowing: if the proposed repair would leave the objective
able to grow forever, `achievable_objective` reports `unbounded` instead of a number;
and if working out the diagnosis itself runs out of time, the error says so in one
line rather than guessing.

### I7 — a bound nothing can reach

Every diagnosis above ends in an edit, because every conflict above can be loosened
into feasibility. Some cannot. A bound of `inf` is not a large number, it is a target
no value reaches, so there is no smaller number to suggest and no amount to quote.
DeciDB says that instead of inventing an edit:

```sql
SELECT id, x
FROM (VALUES (1), (2), (3)) t(id)
DECIDE x(INT)
SUCH THAT x >= 0 AND x <= 6 AND SUM(x) >= 1e1000::DOUBLE
MAXIMIZE SUM(x);
```

```
Invalid Input Error: DECIDE optimization is infeasible: the constraints cannot all be satisfied at once; clause `SUM(x) >= inf` sets a bound no value can reach.
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬────────────┬──────────────┬───────────────┬───────────────────┬───────┐
│ diagnosis_id │   state    │ subject_kind │    subject    │     attribute     │ value │
├──────────────┼────────────┼──────────────┼───────────────┼───────────────────┼───────┤
│ 1            │ infeasible │ clause       │ SUM(x) >= inf │ unreachable_bound │ true  │
└──────────────┴────────────┴──────────────┴───────────────┴───────────────────┴───────┘
```

`unreachable_bound` appears instead of `edit_kind` / `suggested_change` / `amount`, and
it is the one infeasible shape with no edit rows at all. The fix is yours to choose:
write a finite bound, or drop the clause. The direction matters — `SUM(x) <= inf` points
the other way, constrains nothing, and is never reported.

---

## When the solver can't tell (rare)

Occasionally a solver's first answer is the ambiguous "infeasible *or* unbounded".
DeciDB settles it with a quick internal check before reporting, so in practice you
see one of the two definitive diagnoses above. In the rare case the evidence stays
ambiguous, you get whichever diagnosis the evidence supports: the unbounded report
with the caveat `It may instead be infeasible.` appended, or the infeasible
diagnosis.

---

## Slow — the time limit expires first

A solve that hits the wall-clock limit (default 300s; set the `DECIDB_TIME_LIMIT`
environment variable to change it) is not thrown away: DeciDB prints a checkpoint
report of what the solver has so far, and the `decide_on_timeout` setting decides
what happens next — `ask` (default: at a terminal, Enter keeps solving and `s` stops;
in scripts it behaves like `error`), `error` (report, then stop), or `continue`
(keep solving until done or interrupted). Ctrl-C works on any solve and behaves like
reaching the limit: you get the best answer found so far instead of a dead query.

### S1 — the limit hits, but a usable solution exists

A hard portfolio pick over 400 parts and six capacity constraints (weights derived
from the part keys), with a 1-second limit. The solution in hand is quantified —
within 0.11% of the best possible — so you can decide whether it's already good
enough.

```sql
SELECT p_partkey, buy
FROM (
  SELECT p_partkey, p_retailprice,
         ((p_partkey*7)%97)+1  AS w0, ((p_partkey*9)%97)+1  AS w1,
         ((p_partkey*11)%97)+1 AS w2, ((p_partkey*13)%97)+1 AS w3,
         ((p_partkey*15)%97)+1 AS w4, ((p_partkey*17)%97)+1 AS w5
  FROM part WHERE p_partkey <= 400
)
DECIDE buy(BOOL)
SUCH THAT SUM(w0*buy) <= 9785 AND SUM(w1*buy) <= 9766
      AND SUM(w2*buy) <= 9747 AND SUM(w3*buy) <= 9776
      AND SUM(w4*buy) <= 9806 AND SUM(w5*buy) <= 9787
MAXIMIZE SUM(p_retailprice * buy);
```

```
DECIDE hit the 1s time limit with a usable solution (not proven best).
  best objective so far: 261144  (within 0.11% of the best possible)
  elapsed 1s · peak memory 108 MB
Invalid Input Error: DECIDE optimization is slow: the solve hit the time limit with a usable but unproven solution — reduce the input size to prove it, or keep solving with SET decide_on_timeout='continue'
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬───────┬──────────────┬─────────┬─────────────────────────┬────────────────┐
│ diagnosis_id │ state │ subject_kind │ subject │        attribute        │     value      │
├──────────────┼───────┼──────────────┼─────────┼─────────────────────────┼────────────────┤
│ 1            │ slow  │ model        │ NULL    │ stopped_by              │ time_limit     │
│ 1            │ slow  │ model        │ NULL    │ status                  │ solution_found │
│ 1            │ slow  │ model        │ NULL    │ best_objective          │ 261144         │
│ 1            │ slow  │ model        │ NULL    │ within_percent_of_best  │ 0.11%          │
│ 1            │ slow  │ model        │ NULL    │ best_possible_objective │ 261439         │
│ 1            │ slow  │ model        │ NULL    │ elapsed                 │ 1s             │
│ 1            │ slow  │ model        │ NULL    │ peak_memory             │ 108 MB         │
└──────────────┴───────┴──────────────┴─────────┴─────────────────────────┴────────────────┘
```

The "within X%" line appears only when the solver has actually proven how close the
solution is to the best possible; when it hasn't (some purely continuous problems at
very small limits), the report simply omits the claim rather than fabricate one.

### S2 — the limit hits with no solution at all

A pick that must hit six exact totals simultaneously (coefficients hashed from the
part keys) — hard enough that 1 second finds nothing either way. The report says so
plainly; nothing is returned.

```sql
SELECT p_partkey, pick
FROM (
  SELECT p_partkey,
         hash(p_partkey*6+0)%100 AS c0, hash(p_partkey*6+1)%100 AS c1,
         hash(p_partkey*6+2)%100 AS c2, hash(p_partkey*6+3)%100 AS c3,
         hash(p_partkey*6+4)%100 AS c4, hash(p_partkey*6+5)%100 AS c5
  FROM part WHERE p_partkey <= 60
)
DECIDE pick(BOOL)
SUCH THAT SUM(c0*pick) = 1509 AND SUM(c1*pick) = 1554
      AND SUM(c2*pick) = 1495 AND SUM(c3*pick) = 1535
      AND SUM(c4*pick) = 1473 AND SUM(c5*pick) = 1650;
```

```
DECIDE hit the 1s time limit without finding a solution yet.
  elapsed 1s · peak memory 103 MB
Invalid Input Error: DECIDE optimization is slow: the solve hit the time limit before finding a solution — reduce the input size or loosen the constraints, or keep searching with SET decide_on_timeout='continue'
Details: SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬───────┬──────────────┬─────────┬─────────────┬─────────────┐
│ diagnosis_id │ state │ subject_kind │ subject │  attribute  │    value    │
├──────────────┼───────┼──────────────┼─────────┼─────────────┼─────────────┤
│ 1            │ slow  │ model        │ NULL    │ stopped_by  │ time_limit  │
│ 1            │ slow  │ model        │ NULL    │ status      │ no_solution │
│ 1            │ slow  │ model        │ NULL    │ elapsed     │ 1s          │
│ 1            │ slow  │ model        │ NULL    │ peak_memory │ 103 MB      │
└──────────────┴───────┴──────────────┴─────────┴─────────────┴─────────────┘
```

### S3 — keep solving, stop when satisfied

Same query as S1 with `SET decide_on_timeout='continue';` — the solve resumes past
each limit (the report repeats with rising elapsed time) until you press Ctrl-C.
The stop is graceful: the query **succeeds**, returning the best solution found,
with a one-line caveat on how to read it.

```
DECIDE hit the 1s time limit with a usable solution (not proven best).
  best objective so far: 261144  (within 0.11% of the best possible)
  elapsed 1s · peak memory 107 MB
DECIDE hit the 2s time limit with a usable solution (not proven best).
  best objective so far: 261144  (within 0.11% of the best possible)
  elapsed 2s · peak memory 149 MB
DECIDE stopped at your request with a usable solution (not proven best).
  best objective so far: 261144  (within 0.11% of the best possible)
  elapsed 2.5s · peak memory 165 MB
DECIDE is returning the best solution found so far — it is NOT proven the best possible.
```

```
┌───────────┬───────┐
│ p_partkey │  buy  │
│   int64   │ int32 │
├───────────┼───────┤
│         1 │     1 │
│         2 │     1 │
│         3 │     1 │
│         4 │     0 │
│         · │     · │
│         · │     · │
│       399 │     1 │
│       400 │     1 │
├───────────┴───────┤
│     400 rows      │
│    (40 shown)     │
└───────────────────┘
```

And because the answer you took is unproven, its quality stays queryable *after* the
rows return:

```sql
SELECT * FROM decide_diagnostics();
```

```
┌──────────────┬─────────┬──────────────┬─────────┬─────────────────────────┬────────────────┐
│ diagnosis_id │  state  │ subject_kind │ subject │        attribute        │     value      │
├──────────────┼─────────┼──────────────┼─────────┼─────────────────────────┼────────────────┤
│            1 │ slow    │ model        │ NULL    │ stopped_by              │ user_interrupt │
│            1 │ slow    │ model        │ NULL    │ status                  │ solution_found │
│            1 │ slow    │ model        │ NULL    │ best_objective          │ 261144         │
│            1 │ slow    │ model        │ NULL    │ within_percent_of_best  │ 0.11%          │
│            1 │ slow    │ model        │ NULL    │ best_possible_objective │ 261429         │
│            1 │ slow    │ model        │ NULL    │ elapsed                 │ 2.5s           │
│            1 │ slow    │ model        │ NULL    │ peak_memory             │ 165 MB         │
└──────────────┴─────────┴──────────────┴─────────┴─────────────────────────┴────────────────┘
```

(On the bundled HiGHS backend, Ctrl-C takes effect at the next report rather than
instantly, and the stop reads as a time-limit stop; the best-so-far rows are still
returned.)

---

## Turning it off

`PRAGMA diagnose_decide='off'` suppresses every diagnosis: no reports, no
continuation, an empty `decide_diagnostics()`, and the plain static error — which
reminds you how to get the detail back.

```sql
PRAGMA diagnose_decide='off';
SELECT p_partkey, buy
FROM part
WHERE p_partkey <= 8
DECIDE buy(REAL)
SUCH THAT buy >= 0
MAXIMIZE SUM(buy * p_retailprice);
```

```
Invalid Input Error: DECIDE optimization is unbounded: a decision variable can grow without bound. Add an upper bound, e.g. SUCH THAT x <= <cap>. For the variable, set PRAGMA diagnose_decide='auto' and re-run.
```
