# S2 (laminar-matroid optimization) — what works today

Verified on 2026-11-13 at `86ccc227ec`: `test/decide/run_direct_matrix.sh`, each of its four suites (auto, off,
serializer, highs) 7,037 run; C++ `build/release/test/unittest "[decidb]"` 9,547 assertions in 105 cases.

Code: `src/optimizer/decide/direct/s2_laminar_matroid_rule.cpp`, on the ranked-prefix builder with one level per quota
scope (`../../framework/done.md`). Class definition: [definition.md](definition.md).

## What it admits

One row-scoped `BOOL` decision `x` (output `INTEGER` 0/1), a linear objective `MAXIMIZE` or `MINIMIZE SUM(score*x)`
(the shared linear score, as S1 reads it; no objective at all is admitted), and two or more upper count quotas of
different scope:

- **Quotas.** Each coupling clause is `SUM(x) <= k` or `< k` (a body whose constant coefficients add up to 1 or -1,
  so `SUM(-x) >= -k` is one too), read by the shared count-limit reader and rounded to a whole number of rows. `k` is
  a constant of magnitude below 1e20, a value of the source row (the plan reads the tightest one per group), or an
  aggregate of the group's kept rows (`<= MAX(cap)`, `<= COUNT(*) / 2`, under a `when` of its own), at any level. A
  scope is a clause's `by` key, `when` and `per`; clauses of the same scope form one level, whose cap per group is the
  least of theirs. A NULL data cap is no limit, as the solver skips the instance; a NaN one raises the solver's error,
  and a group with a negative cap can never be met; both raise before the first row leaves.
- **Nesting proved from the text.** The levels must form a chain, each inside the next: the coarser level's `by` key
  is a subset of the finer one's (compared as sets, `DirectKeyWithin`), and the coarser has no `when` or the same one.
  The IR carries no functional dependencies and the rule is chosen once, when the plan is built, so the data is never
  consulted: `by (department)` beside `by (division)` declines even when every department lies in one division.
- **`per` on any level.** The levels need not share a `per`, so the definition's example runs as written (`per` on
  the two finer clauses, none on the global one). A `per` does not group the sum (D6); it only decides which groups
  have an instance (those a class reaches), and each level reads that flag on its own. A level with no instance in a
  group leaves a chain that still nests.
- **Fixed rows.** Clauses that fix rows (`x = 1`, `x = 0`, under a `when`) are read as S1 reads them; a row fixed to
  1 takes a place in every level's group it belongs to.
- **NULL keys and `when`.** A row with a NULL key, or one a level's `when` drops, is in no group of that level, so
  that level does not limit it (D5).

## What it does not admit (falls back to the solver)

A lower or `=` quota; a single scope (S1's); two scopes that do not nest or differ in `when`; a constant cap of
magnitude 1e20 or more; any other decision type or objective.

## How it works

The levels are listed finest first. Level 0 ranks the free rows of each of its groups by score (`ROW_NUMBER`, a fixed
row's NULL key last); level k ranks, in its own groups, only the rows level k-1 keeps (`ROW_NUMBER` partitioned by the
group and the keep flag of level k-1, which gives a kept row its place among the kept rows). A row is kept at level k
when level k-1 keeps it and it is outside level k's groups or its rank is within the group's remaining cap (the cap
less the group's rows fixed to 1). A free row is chosen when the last level keeps it and its score improves the
objective; a fixed row keeps its value. Where a level can fail (fixed rows, or a negative cap), the group's verdict is
raised in the held rows before the first row leaves, so an infeasible group raises the solver's error even under
`LIMIT 1`.

This is the greedy for a laminar matroid: a row rejected by a finer quota takes no place in a coarser one, which ranking
each level over all its rows would get wrong (a division of cap 2 holding scores 9, 8 and 7, where 9 and 8 share a
department of cap 1, selects 9 and 7). Ties may break differently at each level; the exchange argument of the definition
allows it. A data cap is a group bound of its level, registered with the builder: its aggregates are computed in the
level's rank window and its value per group, rounded to whole rows, by the shared group-count-limit fragments S1 reads
too. Where a cap's input fails in two ways at once (a NaN `cap` under `MAX(cap)`), which of the solver's two messages is
raised depends on the thread (error order is not owed). `EXPLAIN` shows `S2_LAMINAR_MATROID` and `min/max SUM(s*x), x in
{0,1}, nested SUM(x) <= k quotas`.

## Verified by

`test/decide/tests/test_direct_s2_laminar_matroid.py` compares S2 with the independent oracle on Gurobi and HiGHS:
the definition's example, the counter-example above, two to four levels written in any order, zero caps, caps above a
group's size, fractional and strict caps, merged clauses, negative caps (infeasible on both paths), fixed rows at each
level, MINIMIZE, negative scores, ties, `when` on one or every level, NULL keys, and caps read from the data at each
level (per-row values, MAX, COUNT and SUM of the group, NULL, NaN and negative caps, a data cap beside a constant
one). The standard tests
(`_direct_rule_fixtures.py`) cover S2 as every rule. `test/common/test_decidb_direct_decline.cpp` holds the forms S2
takes, one decline per branch of its proof, and its row in the overlap test against every other rule.

## Performance

Measured on 2026-11-12 at `a7d2e21de5`, on an 11-core Apple M3 Pro at a load of 6 to 10 (other work ran beside it).

**Against a hand-written plan.** `run.py --compare --sf 1 2 --queries s2` over TPC-H `customer` (score `c_acctbal`;
at most 2 per nation and market segment, 5 per nation, 100 overall), best of 5 in one process. The hand plan is a chain
of `ROW_NUMBER` stages, each ordering the survivors of the stage before first. Both sides gave the same answer.

| Rows | Direct CPU | Hand CPU | CPU ratio | Direct latency | Hand latency | DECIDE planning |
|---:|---:|---:|---:|---:|---:|---:|
| 150,000 | 160.0 ms | 113.2 ms | 1.41 | 22.4 ms | 16.0 ms | 0.13 ms |
| 300,000 | 317.6 ms | 199.8 ms | 1.59 | 39.0 ms | 25.4 ms | 0.13 ms |

**Against Gurobi.** `run.py --sf 0.125 0.25 0.5 1 --queries s2 --modes direct gurobi`, a fresh process per run,
median of 3; the objectives agreed at every size.

| Rows | Direct | Gurobi | Speedup |
|---:|---:|---:|---:|
| 18,750 | 0.032 s | 0.067 s | 2.1x |
| 37,500 | 0.036 s | 0.108 s | 3.0x |
| 75,000 | 0.041 s | 0.196 s | 4.7x |
| 150,000 | 0.052 s | 0.425 s | 8.2x |

The quotas of a laminar matroid make a model whose linear relaxation is integral, so Gurobi solves it at its root
node; the gap is the model's construction, which grows with the rows. At 300,000 rows the harness's 32 threads on 11
cores make direct solve's fresh-process time vary from 0.07 to 0.47 s under load; with 11 threads it takes 0.05 s.
