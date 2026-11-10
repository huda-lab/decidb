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
- **Plan.** Group-then-join over the source reader: the distinct classes of each side, ranked by position, joined back
  to the pair rows. A hand plan of this shape ran in 64 ms at 600 × 600, where Gurobi took 1.47 s.
- **Lean.** No new shared helper until a second rule needs it; no case outside the shapes above.

## O1-1 — The IR records the two sides of a cross product

- **Problem.** A rule reads only the IR, and the IR does not say whether the source is a complete cross product or which
  columns come from which side. O1's first fact (every left row meets every right row) depends on it.
- **Change.** `DirectIR::Read` records, when DECIDE's child is a `LogicalCrossProduct`, the source bindings of each side;
  empty otherwise. Nothing else looks through operators: a projection, filter or join above the cross product leaves it
  empty.
- **Done when.** `test_decidb_direct_ir.cpp` has a case for a cross product (one-side filter included), a cross-side
  filter (empty) and a single table (empty), and `build/release/test/unittest` passes on those cases by name.

## O1-2 — O1's recognition

- **Needs.** O1-1.
- **Problem.** No rule recognizes the matching shape in the scope rulings.
- **Change.** `o1_monge_assignment_rule.cpp` with `CheckCondition` for the rulings' source, constraint and objective
  conditions, returning the two key sides, the position columns and the rank direction (equal ranks, or opposite for
  minimizing `k*A*B` with `k > 0` and maximizing it with `k < 0`). Registered in `direct_registry.cpp`; `Apply` may raise
  an internal error until O1-3. Disjoint from S1, which admits one grouping only.
- **Done when.** `test_decidb_direct_decline.cpp` has one case per decline branch, and each fails if its branch is
  removed; `build/release/test/unittest` passes on them by name, and `./test/decide/run_tests.sh -n 2 -k
  "direct_rule_conformance or direct_s1"` raises no rule overlap.

## O1-3 — O1's plan

- **Needs.** O1-2.
- **Problem.** A recognized query has no plan.
- **Change.** `Apply` builds the group-then-join plan:
  - each side's distinct key classes, numbered by position (`ROW_NUMBER`), joined back to the pair rows;
  - `sel` is 1 on matched ranks, on one copy of each matched class pair;
  - when the two class counts differ, the solver's infeasible error; an empty source gives an empty result;
  - the written coefficient is evaluated on every pair row through the shared value checks, so NULL, NaN, infinity and
    overflow raise in the solver's words.
- **Done when.** The worked example in `definition.md` answers under `require` as under `off`, and
  `./test/decide/run_tests.sh -n 2 -k o1_monge` passes once O1-4 is combined.

## O1-4 — O1's standard tests

- **Needs.** O1-1. Written against the scope rulings; its `require` runs pass once O1-3 is combined.
- **Problem.** O1 has none of the standard tests.
- **Change.**
  - A `RuleFixture` in `_direct_rule_fixtures.py`, with near misses: a cross-side filter, `per l.id`, a computed
    position, an extra constraint, a `when`.
  - `test_direct_o1_monge_assignment.py` with an oracle case table: each objective shape and direction, duplicate rows
    on one side and on both, tied positions, negative positions, unequal sizes (infeasible), an empty side.
  - An error table (NULL and NaN positions) and a parent-query case.
  - A `test_direct_user_facing.py` case comparing the infeasible text of `require` with `off`.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k o1_monge` passes (the fixture ids carry the rule name): every
  case agrees with the oracle and with `off`, and each near miss declines under `require`. Before O1-3 is combined,
  only the `off` runs pass.

## O1-5 — O1's docs leave Prototype

- **Needs.** O1-4.
- **Problem.** `definition.md` lists O1 as Prototype for two reasons the rule settles: complete pair support and
  numeric ordering.
- **Change.**
  - `definition.md` is marked Exact, its example uses `per l`, and each condition in the scope rulings gets its proof.
    Complete support follows from the cross product. Duplicate rows share a cost, so one copy of each pair suffices.
    The ordering is exact on the input values and rounding stays far inside the solver's tolerance.
  - `done.md` in the done format.
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

- **Needs.** O1-4. Optional.
- **Problem.** `per l.id` declines, because a column key may have duplicates or NULLs and its position need not be a
  function of it. Users write this spelling naturally.
- **Change.** Admit a column key when its columns are the declared `PRIMARY KEY` of a table its side scans directly
  (unique and NOT NULL), so each class is one row and its position is fixed. Every other column key declines.
- **Done when.** An oracle case with a primary key answers under `require`; the same query without the key declines;
  `./test/decide/run_tests.sh -n 2 -k o1_monge` passes.
