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
- **Plan.** Group-then-join over the source reader: the distinct classes of each side, ranked by position, joined back
  to the pair rows. A hand plan of this shape ran in 64 ms at 600 × 600, where Gurobi took 1.47 s.
- **Lean.** No new shared helper until a second rule needs it; no case outside the shapes above.

## O1-7 — `per l.id` on a primary key

- **Needs.** Optional.
- **Problem.** `per l.id` declines, because a column key may have duplicates or NULLs and its position need not be a
  function of it. Users write this spelling naturally.
- **Change.** Admit a column key when its columns are the declared `PRIMARY KEY` of a table its side scans directly,
  so each class is one row and its position is fixed. `DirectIR::not_null` already walks to those scans; the front end
  records the primary-key columns on the same walk. Every other column key declines.
- **Done when.** An oracle case with a primary key answers under `require`; the same query without the key declines;
  `./test/decide/run_tests.sh -n 2 -k o1_monge` passes.
