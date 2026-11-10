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
  value) or a column key that determines the side's position (done.md § What it admits), whose columns all come from
  one side, and the two keys come from different sides. No `when`, no other
  constraint, one row-scoped `BOOL` decision.
- **Objective.** One term: `MINIMIZE k*|A - B|` or `MINIMIZE k*(A - B)^2` with constant `k >= 0`, or `MAXIMIZE` or
  `MINIMIZE k*A*B` with constant `k`. A is a bare numeric column of the left key, B of the right key (either order). No
  `when`.
- **Positions (ruling, owner, 2026-11-10).** Both position columns are declared `NOT NULL` (a `PRIMARY KEY` counts) on
  a table their side scans directly; otherwise the rule declines. `SUM` drops a NULL term (guide § NULL; deck p. 56),
  so an item with a NULL position pairs with anyone at no cost, and rank pairing is no longer optimal. A declared
  constraint holds for every row the plan will read, which column statistics do not promise.
- **Lean.** No new shared helper until a second rule needs it; no case outside the shapes above.
