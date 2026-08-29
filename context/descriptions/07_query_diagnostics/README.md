# Query Diagnostics

Turning failed DECIDE solves into actionable diagnoses. SQL always returns rows; a
DECIDE *solve* can fail in ways a lookup can't — and an undiagnosed failure is a dead
end (a static error paragraph, or a silently arbitrary answer). This area replaces each
dead end with a **diagnosis**: *why* it failed and the **least-change** edit that
restores a usable solution.

Nothing here runs on its own. A user asks for it by name, by prefixing the query with
`DIAGNOSE`, and gets the findings back as a relation. See
`00_project_overview/syntax_reference.md` §8 for the syntax and the output schema.

## The map — everything flows through the router

A solve made under `DIAGNOSE` flows through one dispatch tree (the **router**): it
inspects the solver status — plus one sub-signal (is there a recession ray?) — and
routes to exactly one terminal. The states below are its leaves.

```
                       solve result (under DIAGNOSE)
                                   │
        ┌──────────────────────────┴──────────────────────────┐
     solved                                                failed
        │                          ┌─────────────────────────┼──────────────┐
  (one `feasible`              unbounded                 infeasible        inf/unb
       finding)                    │                          │              │
                                find ray                   elastic      check ray
                                   │                          │              │
                                 report                     report    ┌──────┴──────┐
                                                                   found       not found
                                                                      │             │
                                                             "add a bound —      elastic
                                                              may still be          │
                                                              infeasible"        report
```

- `[router/](router/)` — the **spine**: the dispatch tree above and the inf/unb
  `check ray` disambiguation. Three terminals — solved, unbounded, infeasible — each an
  engine dropped behind an existing classifier leaf without editing the classifier. A
  time limit is deliberately *not* a leaf: see below.
- `[unbounded/](unbounded/)` — terminal `failed → unbounded`: names the escaping
  variable via the ray and prescribes a bound (**tighten** a too-open region).
- `[infeasible/](infeasible/)` ★ — terminal `failed → infeasible`: elastic
  relaxation (**loosen** a too-small region). The flagship engine — fully shipped
  (I1–I5, aggregate `<>` removal, and the T3 two-mode slack-scope policy: `query`
  folds each knob to one SQL edit / turns data conflicts into virtual offsets,
  `expanded` exposes the per-row / per-group profile).
- **A slow solve is not a diagnosis.** A solve that hits the wall-clock limit — or that
  a user stops with Ctrl-C — is ordinary execution behaviour: it happens with or without
  the prefix, no engine runs, and nothing is diagnosed. It lives in
  `01_pipeline/08_execution/slow_solves.md`.
- `[foundations/](foundations/)` — the **substrate** every terminal sits on (not
  a terminal): structured solver result, constraint + variable provenance, the
  solver-behavior reference, the reporting relation.

Infeasible and unbounded are mirror images — loosen a too-small region vs. tighten a
too-open one.

## Principles

- **Least-change** — propose the smallest edit, not a rewrite.
- **Opt-in, by name** — `DIAGNOSE <query>` is the only thing that starts the engine.
  There is no automatic path and no setting: a query that is never prefixed never pays
  for a diagnostic solve, and a bare failure reports its state and stops. Naming the
  clause *is* the elastic solve, so it happens only when asked. We never edit the user's
  query; a diagnosis only ever *describes* the failure and prescribes a remedy.
- **Solver-agnostic** — everything works on Gurobi and HiGHS. We build the
  elastic model in our own model builder so both backends solve it natively;
  Gurobi `feasRelax` is an *accelerator*, never a dependency.
- **A native construct must still be reachable** — every matrix, quadratic, indicator,
  and native-general descendant of a drop-only source clause carries the same
  `removal_group_id`. Diagnosis removes that complete group before solving a candidate,
  so native and lowered formulations have the same repair semantics.
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
- **A clause is quoted as the user wrote it** — never as canonicalization built it.
  Stage 04 puts decisions left and the bound right, which for most constraints is
  invisible: `SUM(x) >= 100` is already canonical, so re-rendering the canonical tree
  reproduces what was typed. One shape does not survive it. When **both sides carry
  decisions** the canonicalizer must merge them, and the paper's own Figure 1 line 7 —
  `ship BETWEEN 0 AND capacity * open` — becomes `ship - capacity * open <= 0`: a clause
  the query does not contain, against a literal bound that is not in it either. The
  repair offered against it, `ship - capacity * open <= 25`, named nothing the user
  could edit.

  `ConstraintSourceInfo` now carries the written spelling, captured before
  canonicalization runs, and the diagnosis quotes that. The offset is valid against
  either form — moving a term across a comparison does not change what adding a constant
  to the bound means — so the repair reads `ship <= capacity * open + 25` off the same
  `virtual_offset` machinery that already served column bounds. `EXPLAIN` reads the same
  registry, so it stopped printing the algebra too.

  It stays narrow deliberately. Where only **one** side carries decisions the rewrite is
  a clean move and the leftover bound folds into something *better* than what was
  written: `(SELECT 7) >= x + 2` becomes `x <= 5`, turning an opaque subquery into a
  number the user can edit. Quoting the written form there would be a regression, so it
  does not fire. Tests: `test/decide/tests/test_diagnosis_written_clause.py`.
- **Differential testing** — every phase tests against `oracle_solver` on
  constructed cases, never hand-computed answers.

## Invocation — the `DIAGNOSE` prefix

`DIAGNOSE <select>` runs the query and returns its findings as a relation instead of its
rows. It is a property of the *statement*, carried parser → binder → logical plan →
stage 08 as `LogicalDecide::diagnose`; nothing reads it back out of a session setting.
The full syntax and output schema are in `00_project_overview/syntax_reference.md` §8.

The engine's *tuning* knobs remain sticky session settings — they configure how it works
once it is running, never whether it runs. The one that changes what you see most is
`diagnose_decide_infeasible_slack_scope` (`query` default / `expanded`): one folded
SQL-level edit per knob, vs. the per-row / per-group profile. See `infeasible/done.md`.

---

# What users can expect

A DECIDE query can end four ways: it **solves**, it is **unbounded** (something can
grow forever), it is **infeasible** (the constraints contradict each other), or it is
**slow** (the time limit expires first). This tour shows what DeciDB says in each case.
The first three are what `DIAGNOSE` reports on: it names the offending part of *your*
query and states the smallest edit that fixes it, one row per finding. The fourth is
execution behaviour and prints its own report, prefix or no prefix.

All outputs below are real captured runs against the TPC-H sample database
(`decidb.db`, scale 0.01 — `part` 2000 rows, `lineitem` 60k, `supplier` 100), Gurobi
backend, default settings unless a `PRAGMA`/`SET` line is shown. The slow examples set
the time limit to 1 second (`DECIDB_TIME_LIMIT=1`) so they reproduce in seconds.
For readability, finding tables project the columns relevant to that state; the
authoritative fixed schema is in the syntax reference linked above.

## When it just works

A successful solve returns the decided rows like any SQL query. No diagnosis runs, and
none is paid for. (Under `DIAGNOSE` the same query returns exactly one row saying
`feasible`.)

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
┌───────────┬────────┬──────────────────┬────────┬───────┬───────┬──────────────┬───────┬──────┐
│   state   │ clause │ suggested_change │ amount │ total │ scope │ edit_source  │ group │ row  │
├───────────┼────────┼──────────────────┼────────┼───────┼───────┼──────────────┼───────┼──────┤
│ unbounded │ buy    │ buy <= <cap>     │ 8.0    │ 8     │ row   │ runaway_+inf │ NULL  │ NULL │
└───────────┴────────┴──────────────────┴────────┴───────┴───────┴──────────────┴───────┴──────┘
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
┌───────────┬────────┬──────────────────┬────────┬───────┬───────┬──────────────┬───────────────────────────┬──────┐
│   state   │ clause │ suggested_change │ amount │ total │ scope │ edit_source  │           group           │ row  │
├───────────┼────────┼──────────────────┼────────┼───────┼───────┼──────────────┼───────────────────────────┼──────┤
│ unbounded │ buy    │ buy <= <cap>     │ 29.0   │ 29    │ row   │ runaway_+inf │ p_mfgr = 'Manufacturer#1' │ NULL │
└───────────┴────────┴──────────────────┴────────┴───────┴───────┴──────────────┴───────────────────────────┴──────┘
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
┌───────────┬──────────┬───────────────────┬────────┬───────┬────────┬──────────────┬───────────────────┬──────┐
│   state   │  clause  │ suggested_change  │ amount │ total │ scope  │ edit_source  │       group       │ row  │
├───────────┼──────────┼───────────────────┼────────┼───────┼────────┼──────────────┼───────────────────┼──────┤
│ unbounded │ capacity │ capacity <= <cap> │ 20.0   │ 20    │ entity │ runaway_+inf │ r_name = 'EUROPE' │ NULL │
└───────────┴──────────┴───────────────────┴────────┴───────┴────────┴──────────────┴───────────────────┴──────┘
```

### U4 — when the variable can't be named

A non-linear objective (here a `POWER` term) prevents identifying the runaway
variable. `DIAGNOSE` says so honestly in one `undiagnosed` finding rather than
returning an empty relation or raising another error.

```sql
SELECT p_partkey, buy
FROM part
WHERE p_partkey <= 8
DECIDE buy(REAL)
SUCH THAT buy >= 0
MAXIMIZE SUM(buy * p_retailprice) + SUM(POWER(buy, 2));
```

```
┌───────────┬────────┬─────────────────────────────────────────────────┬────────┬───────┬───────┬─────────────┬───────┬──────┐
│   state   │ clause │                suggested_change                 │ amount │ total │ scope │ edit_source │ group │ row  │
├───────────┼────────┼─────────────────────────────────────────────────┼────────┼───────┼───────┼─────────────┼───────┼──────┤
│ unbounded │ NULL   │ a non-linear term prevents naming the variable. │ NULL   │ NULL  │ NULL  │ undiagnosed │ NULL  │ NULL │
└───────────┴────────┴─────────────────────────────────────────────────┴────────┴───────┴───────┴─────────────┴───────┴──────┘
```

---

## Infeasible — the constraints can't all hold at once

There is no assignment that satisfies every constraint. DeciDB finds the **smallest
edit** that restores a solution and quotes it back in your own syntax: which clause,
loosened to what, by how much — plus one `achievable_objective` row, the objective value
you would get after the edit. When more than one clause must give, each gets its own
row.

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
┌────────────┬───────────┬──────────────────┬────────┬──────────────────────┬───────┬──────┐
│   state    │  clause   │ suggested_change │ amount │     edit_source      │ group │ row  │
├────────────┼───────────┼──────────────────┼────────┼──────────────────────┼───────┼──────┤
│ infeasible │ make <= 5 │ make <= 10       │ 5.0    │ source_literal       │ NULL  │ NULL │
│ infeasible │ NULL      │ NULL             │ 80.0   │ achievable_objective │ NULL  │ NULL │
└────────────┴───────────┴──────────────────┴────────┴──────────────────────┴───────┴──────┘
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
┌────────────┬──────────────────────────────────┬───────────────────────────────────┬────────┬──────────────────────┬───────┬──────┐
│   state    │              clause              │         suggested_change          │ amount │     edit_source      │ group │ row  │
├────────────┼──────────────────────────────────┼───────────────────────────────────┼────────┼──────────────────────┼───────┼──────┤
│ infeasible │ AVG(buy * p_retailprice) >= 5000 │ AVG(buy * p_retailprice) >= 904.5 │ 4095.5 │ source_literal       │ NULL  │ NULL │
│ infeasible │ NULL                             │ NULL                              │ 8.0    │ achievable_objective │ NULL  │ NULL │
└────────────┴──────────────────────────────────┴───────────────────────────────────┴────────┴──────────────────────┴───────┴──────┘
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
┌────────────┬─────────────────────────────────┬──────────────────────────────────┬────────┬──────────────────────┬───────┬──────┐
│   state    │             clause              │         suggested_change         │ amount │     edit_source      │ group │ row  │
├────────────┼─────────────────────────────────┼──────────────────────────────────┼────────┼──────────────────────┼───────┼──────┤
│ infeasible │ SUM(buy) >= 20                  │ SUM(buy) >= 4                    │ 16.0   │ source_literal       │ NULL  │ NULL │
│ infeasible │ SUM(buy * p_retailprice) <= 900 │ SUM(buy * p_retailprice) <= 3610 │ 2710.0 │ source_literal       │ NULL  │ NULL │
│ infeasible │ NULL                            │ NULL                             │ 4.0    │ achievable_objective │ NULL  │ NULL │
└────────────┴─────────────────────────────────┴──────────────────────────────────┴────────┴──────────────────────┴───────┴──────┘
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
┌────────────┬────────────┬────────────────────┬────────┬──────────────────────┬───────┬──────┐
│   state    │   clause   │  suggested_change  │ amount │     edit_source      │ group │ row  │
├────────────┼────────────┼────────────────────┼────────┼──────────────────────┼───────┼──────┤
│ infeasible │ promo <> 0 │ remove this clause │ NULL   │ remove_only          │ NULL  │ NULL │
│ infeasible │ NULL       │ NULL               │ 0.0    │ achievable_objective │ NULL  │ NULL │
└────────────┴────────────┴────────────────────┴────────┴──────────────────────┴───────┴──────┘
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
┌────────────┬─────────────────────┬──────────────────────────┬─────────┬──────────────────────┬───────┬──────┐
│   state    │       clause        │     suggested_change     │ amount  │     edit_source      │ group │ row  │
├────────────┼─────────────────────┼──────────────────────────┼─────────┼──────────────────────┼───────┼──────┤
│ infeasible │ ship <= ps_availqty │ ship <= ps_availqty + 18 │ 18.0    │ virtual_offset       │ NULL  │ NULL │
│ infeasible │ NULL                │ NULL                     │ 35064.0 │ achievable_objective │ NULL  │ NULL │
└────────────┴─────────────────────┴──────────────────────────┴─────────┴──────────────────────┴───────┴──────┘
```

To see exactly which rows conflict instead, set
`PRAGMA diagnose_decide_infeasible_slack_scope='expanded'` — one edit per conflicting
row, at its own row's values:

```
┌────────────┬────────────┬──────────────────┬─────────┬──────────────────────┬───────┬──────┐
│   state    │   clause   │ suggested_change │ amount  │     edit_source      │ group │ row  │
├────────────┼────────────┼──────────────────┼─────────┼──────────────────────┼───────┼──────┤
│ infeasible │ ship <= 27 │ ship <= 32       │ 5.0     │ expanded_row         │ NULL  │ NULL │
│ infeasible │ ship <= 11 │ ship <= 29       │ 18.0    │ expanded_row         │ NULL  │ NULL │
│ infeasible │ NULL       │ NULL             │ 34943.0 │ achievable_objective │ NULL  │ NULL │
└────────────┴────────────┴──────────────────┴─────────┴──────────────────────┴───────┴──────┘
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
┌────────────┬──────────────────────────┬──────────────────────────┬────────┬──────────────────────┬───────┬──────┐
│   state    │          clause          │     suggested_change     │ amount │     edit_source      │ group │ row  │
├────────────┼──────────────────────────┼──────────────────────────┼────────┼──────────────────────┼───────┼──────┤
│ infeasible │ SUM(buy) >= 3 PER p_mfgr │ SUM(buy) >= 1 PER p_mfgr │ 2.0    │ source_literal       │ NULL  │ NULL │
│ infeasible │ NULL                     │ NULL                     │ 12.0   │ achievable_objective │ NULL  │ NULL │
└────────────┴──────────────────────────┴──────────────────────────┴────────┴──────────────────────┴───────┴──────┘
```

In `expanded` mode the headline names the failing groups and each gets its own edit,
sized to its own shortfall:

```
┌────────────┬──────────────────────────┬──────────────────────────┬────────┬──────────────────────┬────────────────┬──────┐
│   state    │          clause          │     suggested_change     │ amount │     edit_source      │     group      │ row  │
├────────────┼──────────────────────────┼──────────────────────────┼────────┼──────────────────────┼────────────────┼──────┤
│ infeasible │ SUM(buy) >= 3 PER p_mfgr │ SUM(buy) >= 2 PER p_mfgr │ 1.0    │ expanded_group       │ Manufacturer#2 │ NULL │
│ infeasible │ SUM(buy) >= 3 PER p_mfgr │ SUM(buy) >= 1 PER p_mfgr │ 2.0    │ expanded_group       │ Manufacturer#5 │ NULL │
│ infeasible │ NULL                     │ NULL                     │ 12.0   │ achievable_objective │ NULL           │ NULL │
└────────────┴──────────────────────────┴──────────────────────────┴────────┴──────────────────────┴────────────────┴──────┘
```

Two edge behaviors worth knowing: if the proposed repair would leave the objective
able to grow forever, a separate `unbounded_after_fix` finding says so instead of
inventing a number; and if working out the diagnosis itself runs out of time, one
`undiagnosed` finding says so rather than guessing.

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
┌────────────┬───────────────┬───────────────────────────────────────────────┬────────┬───────────────────┬───────┬──────┐
│   state    │    clause     │               suggested_change                │ amount │    edit_source    │ group │ row  │
├────────────┼───────────────┼───────────────────────────────────────────────┼────────┼───────────────────┼───────┼──────┤
│ infeasible │ SUM(x) >= inf │ lower this bound — no assignment can reach it │ NULL   │ unreachable_bound │ NULL  │ NULL │
└────────────┴───────────────┴───────────────────────────────────────────────┴────────┴───────────────────┴───────┴──────┘
```

`unreachable_bound` is the finding kind in `edit_source`; it carries an explanation in
`suggested_change` but no numeric `amount`. The fix is yours to choose: write a finite
bound, or drop the clause. The direction matters — `SUM(x) <= inf` points the other way,
constrains nothing, and is never reported.

---

## When the solver can't tell (rare)

Occasionally a solver's first answer is the ambiguous "infeasible *or* unbounded".
DeciDB settles it with a quick internal check before reporting, so in practice you
see one of the two definitive diagnoses above. In the rare case the evidence stays
ambiguous, the ray still chooses the engine. If the unbounded engine cannot name a
variable, its `undiagnosed` finding keeps `state='infeasible or unbounded'` rather than
pretending the status was resolved; no ray routes to the infeasible diagnosis.

---

## Slow — the time limit expires first

Not a diagnosis. A solve that runs out of wall-clock, or that a user stops with Ctrl-C,
is ordinary execution behaviour — it happens with or without the `DIAGNOSE` prefix and
no engine runs. Its report, its continuation offer, and its worked examples live with
the layer that owns it: `01_pipeline/08_execution/slow_solves.md`.

## Without the prefix

Nothing starts the diagnostics engine except `DIAGNOSE`. The same query unprefixed
reports its state and stops — no clause, no repair, no second statement — and points at
the prefix that would answer the question.

```sql
SELECT p_partkey, buy
FROM part
WHERE p_partkey <= 8
DECIDE buy(REAL)
SUCH THAT buy >= 0
MAXIMIZE SUM(buy * p_retailprice);
```

```
Invalid Input Error: DECIDE optimization is unbounded. Prefix the query with DIAGNOSE to see which decision needs a bound.
```

That is not a mode you can turn off, and there is nothing to turn on: a query that is
never prefixed never runs a diagnostic solve, so it never pays for one.
