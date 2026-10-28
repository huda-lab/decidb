# Stage 08 — Execution: open work

## 2. A NULL key is not a class (reverses D5)

**Ruling (owner, 2026-10-27).** "NULL should not be a key. That group should skip." Today D5 makes a NULL key one
more class (`../../language/guide.md` §7.2): with `sum(x) by (g) <= 2`, the rows whose `g` is NULL get a limit of 2 of
their own. Under the ruling those rows leave that clause, as `when g is not null` would, and keep every other clause
and the objective. The keyed direct plans already skip NULL-key rows (`../../direct_solve/framework/todo.md` H-05).

**Scope** (owner, 2026-10-27):

1. `by (g)` on a reducer: rows with a NULL `g` are out of that reducer.
2. `per g:` in front of a constraint: NULL-`g` rows get no constraint from that clause.
3. `per` inside a reducer (`sum(per cap: cap)`): NULL-`cap` rows are left out of the sum.
4. A keyed decision (`per K:` declaration): a NULL-key row has no decision (owner, 2026-10-28, following deck p. 6).
   Its value is absent: a reducer skips it and an unreduced use skips that constraint instance (deck p. 50).
6. A NULL key is no group of that key only: the clause's other reducers still read the row, so this is not a fold of
   `g IS NOT NULL` into the `when` (`sum(x) by (g) <= 0.5 * sum(x)`: the NULL-`g` row has no limit and counts in the
   total). A composite key is NULL when any written column is; a relation key (`per D:`) is unchanged, its columns
   comparing NULL as a value (owner, 2026-10-28). Each key element records which (`DecideKeyNullMode`).
7. A group left empty, by NULL keys, a reducer's own `when` or a clause `when` that keeps no row, skips its
   constraint (owner, 2026-10-28): D4 and D8 retire in this batch. Rulings in full: `../../todo.md` B1.
5. SQL's `GROUP BY` keeps NULL as one group. The docs say DECIDE departs from it because a NULL key is an unknown
   group, and a limit for an unknown group means nothing.

Once this lands no finding names a NULL group, which closes `../../diagnostics/infeasible/todo.md` §2; delete that
section in the same change.

**Work.** The window partitions and class numbering in `RewriteRhsAggregates` (`decide_rewrite_rhs_window.cpp`) and
the grouping here (§1); the scope checks and messages that quote `g = NULL` (`PhysicalDecide::RunScopeChecks`); the
removed-spelling NULL note (D62), which becomes unnecessary.

**Test.** Every D5 test turns over (for example `empty_all_keys_null` and the NULL-key cases of
`test_direct_s1_uniform_matroid.py`, `test_direct_r1_continuous_knapsack.py`, `test_scope_*.py`), and the golden
baselines with NULL keys. The keyed-plan C++ test ("S1 and R1 keyed plans answer as the solver does",
`test/common/test_decidb_direct_decline.cpp`) can take NULL keys again (`KEYED_ROWS`). Flip each D5 test in the same
change as the code, not before.

**Update.** `../../language/guide.md` §2.3, §3 and §7.2; `../../language/errors.md` §3; `../../language/decisions.md`
D5 and D62.
