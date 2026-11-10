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

## O1-5 — O1's docs leave Prototype

- **Problem.** `definition.md` lists O1 as Prototype for two reasons the rule settles: complete pair support and
  numeric ordering.
- **Change.**
  - `definition.md` is marked Exact, its example uses `per l`, and each condition in the scope rulings gets its proof.
    Complete support follows from the cross product. Duplicate rows share a cost, so one copy of each pair suffices.
    The ordering is exact on the input values and rounding stays far inside the solver's tolerance. A declared
    `NOT NULL` position rules out the free item a dropped NULL term makes.
  - `done.md` in the done format, from the code (`o1_monge_assignment_rule.cpp`) and its tests: what it admits
    (including the casts it looks through and the constant factor in the part's scale), what it declines (a maximized
    distance, `pow`/`**`/`^`, `TRY_CAST`, a nullable position), the plan (three source reads, the hidden coefficient
    column), and where it differs from the solver.
  - O1's status row in `../../README.md` reads Works, and its links table points to O1's `done.md` and `todo.md`.
- **Done when.** Every claim in `done.md` names the test that holds it. Doc-only: no tests.

## O1-6 — O1's Performance is measured

- **Needs.** O1-5. A quiet machine; the owner starts the run.
- **Problem.** `done.md` has no Performance section, which every working class has.
- **Change.** A hand plan `benchmark/decide/direct_solve/hand/o1.sql` and an O1 query in the benchmark, measured as
  `../../../performance/benchmarking/direct_solve.md` § Comparing with a hand-written plan describes, against Gurobi and
  HiGHS.
- **Done when.** `done.md` § Performance gives CPU against the hand plan and speed against both solvers.

## O1-7 — `per l.id` on a primary key

- **Needs.** Optional.
- **Problem.** `per l.id` declines, because a column key may have duplicates or NULLs and its position need not be a
  function of it. Users write this spelling naturally.
- **Change.** Admit a column key when its columns are the declared `PRIMARY KEY` of a table its side scans directly,
  so each class is one row and its position is fixed. `DirectIR::not_null` already walks to those scans; the front end
  records the primary-key columns on the same walk. Every other column key declines.
- **Done when.** An oracle case with a primary key answers under `require`; the same query without the key declines;
  `./test/decide/run_tests.sh -n 2 -k o1_monge` passes.

## O1-8 — The number-range warning on O1's plan

- **Decision needed.**
- **Problem.** With `INT` positions near 2e9, the solver path prints "a very large or very small number may make this
  answer inaccurate"; O1's plan prints nothing. The warning describes the solver's floating-point model. Rank pairing
  compares the positions themselves, so its answer has no such error.
- **Options.**
  1. No warning on O1's plan; `done.md` lists it as a difference from the solver. No code.
  2. Warn as the solver does, from the same coefficient range (R1's plan does this). A range check on every pair row.
- **Recommendation.** Option 1: the warning would claim an inaccuracy the plan does not have.
- **Done when.** Option 1: the difference is in `done.md`. Option 2: a test compares the warning under `require` and
  `off`, and `./test/decide/run_tests.sh -n 2 -k o1_monge` passes.
