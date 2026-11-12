# S2 (laminar-matroid optimization) — open work

S2 selects the best `BOOL` items under upper count quotas whose groups nest (departments inside divisions inside one
global cap). The class is in `definition.md`. How to work is in `../../framework/todo.md` § How to work any task; the
standard tests are in `../../architecture/rules.md` § The standard tests every rule ships. The exit gate of
`../s1_uniform_matroid/todo.md` holds here too.

## S2-1 — Recognition: nested upper count quotas over one decision

- **Decision needed.**
- **Problem.** No rule takes a query with two or more upper count quotas of different scope, such as
  `SUM(x) by (division, department) <= 1 AND SUM(x) by (division) <= 1 AND SUM(x) <= 2 MAXIMIZE SUM(score*x)`; the
  solver path answers it. The IR holds each clause with its own key, `when` and `per` classes (`DirectIRConstraint`),
  so the shape is readable. Nesting cannot be read from the data: the IR carries no functional dependencies, and the
  driver picks a rule once, when the plan is built, so a run-time check could only raise an error the solver would not
  raise. S2 proves nesting from the query text and declines anything else.
- **Options.**
  1. Syntax only: level C contains level F when C's key columns are a subset of F's (compared without order), C has
     no `when` or the same `when` as F, and the two have the same `per` gating. Provable families are chains, so the
     plan is a straight cascade. `by (department)` beside `by (division)` declines, though the data may nest them.
  2. Option 1 plus a finer `when` that implies the coarser one. Needs an implication proof between predicates.
- **Recommendation.** Option 1.
- **Change.** Rule `S2_LAMINAR_MATROID`, registered beside S1 (`../../architecture/rules.md` § Adding a rule). It
  requires Boolean decisions, the linear objective S1 reads (FRM-23), every coupling clause an upper count limit read
  by the shared reader (FRM-24, FRM-25), and at least two scopes forming a chain under the containment the ruling
  names; `Contains` stays in S2's file. S1 requires one scope and S2 two or more, so the rules are disjoint by one IR
  property. Two clauses with the same key and `when` and a different `per` decline. Lower and equality limits decline
  (`definition.md` § Supported variants).
- **Needs.** FRM-23, FRM-24, FRM-25, FRM-27.
- **Done when.** `build/release/test/unittest "S2*"` (forms that apply, and one decline per proof branch: two scopes
  that do not nest, a different `when`, a different `per`, a lower limit, a non-Boolean decision); the S2 row of
  `build/release/test/unittest "Every rule and every other rule never apply to the same query"`.

## S2-2 — Plan: a chain of rankings, one per level, on the levels builder

- **Problem.** S2-1 recognizes the class and has no plan. Ranking each level over all its rows is wrong: with a
  division of cap 2 holding a:9, b:8 and c:7, where a and b share a department of cap 1, the greedy answer is {a, c},
  and independent ranks give {a}, since b takes a place in the division although the department rejects it.
- **Change.** Build on the levels builder (FRM-26), finest level first: a row is kept at level k when level k-1 keeps
  it and its rank among the rows level k-1 keeps, in its level-k group, is within the group's remaining cap (the cap
  less the group's fixed rows); a row outside level k's groups (a `when` drops it, a NULL key) passes the level. A
  row is chosen when the last level keeps it and its score improves the objective; a fixed row keeps its value. A
  negative remaining cap raises the infeasible error before the first row leaves. Ties may break differently at each
  level, which the exchange argument allows. Correct step 1 of `definition.md` § Relational construction: the plan
  proves nesting when it is built, not in SQL.
- **Needs.** S2-1, FRM-26.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "direct_s2 or every_input_row_is_read or direct_rule_conformance"`
  with the standard tests and oracle tests for: the example above, a three-level chain, a zero cap, a cap above the
  group's size, fixed rows, MINIMIZE, negative scores, ties, a `when` and a NULL key;
  `DECIDB_FORCE_SOLVER=highs ./test/decide/run_tests.sh -n 2 -k "direct_s2"`.

## S2-3 — Measure S2 against the hand plan and Gurobi

- **Problem.** S2 has no benchmark query, no hand plan and no measurement.
- **Change.** A query over TPC-H `customer` (score `c_acctbal`; quotas `by (c_nationkey, c_mktsegment)`, inside
  `by (c_nationkey)`, inside a global cap), a hand plan in `benchmark/decide/direct_solve/hand/`, and the
  Performance section of `done.md` in the shape of S1's. Time a filtered cascade (each level reads only the rows the
  level below keeps, joined back once) against the one sort over all rows per level; a cascade that wins by more than
  the noise (§ Small gains) becomes a task of its own.
- **Needs.** S2-2.
- **Done when.** `python3 benchmark/decide/direct_solve/run.py --compare --sf 1 --queries s2`; `python3
  benchmark/decide/direct_solve/run.py --sf 0.125 0.25 0.5 1 --queries s2 --modes direct gurobi`.
