# O1 (Monge assignment) — open work

O1 answers a one-to-one matching of two ordered sides by sorting both and pairing equal ranks. The class is in
`definition.md`. How to work is in `../../framework/todo.md` § How to work any task. A task runs only the focused
tests its **Done when** names; the full gate runs once at combine. The standard tests are in
`../../architecture/rules.md` § The standard tests every rule ships.

## Scope rulings (owner, 2026-11-10)

The first version admits only the shape below and declines everything else. A decline is a slower correct answer; a
wrong admit is a wrong answer.

- **Source.** DECIDE's child is a `CROSS_PRODUCT`. Filter pushdown runs before the DECIDE optimizer, so a one-side
  `WHERE` sits inside its side and the pairs stay complete; a filter across both sides makes a join, which declines.
- **Constraints.** Exactly two, each `per K: sum(x) by (K) = 1`, where K is a relation key (`per l`, NULL compared as a
  value) whose columns all come from one side, and the two keys come from different sides. No `when`, no other
  constraint, one row-scoped `BOOL` decision.
- **Objective.** One term: `MINIMIZE k*|A - B|` or `MINIMIZE k*(A - B)^2` with constant `k >= 0`, or `MAXIMIZE` or
  `MINIMIZE k*A*B` with constant `k`. A is a bare numeric column of the left key, B of the right key (either order). No
  `when`.
- **Positions (ruling, owner, 2026-11-10).** Both position columns are declared `NOT NULL` (a `PRIMARY KEY` counts) on
  a table their side scans directly; otherwise the rule declines. `SUM` drops a NULL term (guide § NULL; deck p. 56),
  so an item with a NULL position pairs with anyone at no cost, and rank pairing is no longer optimal. A declared
  constraint holds for every row the plan will read, which column statistics do not promise.
- **Lean.** No new shared helper until a second rule needs it; no case outside the shapes above.

## O1-7 — `per l.id` on a key of its table

- **Problem.** `per l.id` declines, because a column key may hold duplicates or NULLs and need not determine the
  position. Users write this spelling naturally. When `id` is a key of the table `l` scans, `per l.id` gives the same
  classes as `per l`, but only under that table's constraints: a fact of the catalog, which the canonicalizer may not
  use (`../../../pipeline/04_canonicalizer/done.md` § What a rule must be).
- **Change.** The direct IR records the keys of each directly scanned table on the same walk as `not_null`: every
  UNIQUE constraint, the PRIMARY KEY included, whose columns are all declared NOT NULL and read by the scan. O1 admits
  a column key when it determines the side's position: the key holds the position, or holds one of those keys of the
  position's table. Every other column key declines. **Ruling (owner, 2026-11-10).** A UNIQUE key counts only when
  all its columns are declared NOT NULL: UNIQUE lets NULLs repeat, and a column key puts a row with a NULL key in no
  class. DuckDB declares a PRIMARY KEY's columns NOT NULL, so it needs no case of its own.
- **Done when.** Oracle cases: a PRIMARY KEY column and a UNIQUE NOT NULL column answer under `require`; the same query
  without the key, and on a nullable UNIQUE column, declines; the IR's keys are checked in `test_decidb_direct_ir.cpp`;
  `./test/decide/run_tests.sh -n 2 -k o1_monge` and `build/release/test/unittest "[decidb]"` pass.
