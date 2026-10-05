# Shared engine — open work

The engine is everything that is not one problem class: facts, coordinator, builders, result boundary, policy. Cross-cutting
tasks live here. When a task ships, its result moves to `done.md` and the task is deleted from this file.
Old IDs are in parentheses so earlier notes still make sense.

## H-01 — A second rule proves the engine is reusable (was HAR-02 and NEXT-01)

- **Why.** The engine is built for many rules and has only ever run one. Until a materially different rule registers
  without touching the engine, reusability is a design bet.
- **Chosen rule.** A1, independent bounded decisions (`../a1/todo.md`), picked 2026-10-04. It brings `INT`/`REAL`
  decisions, per-row bounds, a quadratic objective, and a plan that streams, which exercises `DirectValidationBarrier`.
- **Code.** `src/optimizer/decide/direct/`: the shared parts are `direct_coordinator.cpp`, `direct_result_boundary.cpp`,
  `direct_builder.cpp`; `s1_rule.cpp` is the model. Checklist: `../../architecture/rules.md`.
- **Done when.** The rule is added to `RegisteredDirectRules()` with no edit to the coordinator, builder or result
  boundary beyond that line, and it has its own proof, a hit test, a miss test that falls back to the solver, and the
  `require` message. Moves to `done.md`.

## H-02 — ANR adapter for the new language (was NEXT-04)

- **Goal.** Produce the same facts from the new language, so the rules work for both syntaxes.
- **Depends.** A stable language branch.
- **Code.** `direct_problem.cpp` is the current-tree adapter; `../../architecture/facts.md`.
- **Done when.** The same facts contract and direct/solver tests run on both syntaxes where comparable. Extend the facts
  when semantics change; do not teach a rule parser spelling. Compatibility is not assumed until this passes.

## H-03 — Cost choice among competing direct plans (was NEXT-05)

- **Goal.** Choose between two proved direct plans by cost. The coordinator already picks the cheapest proved
  candidate, but only one rule exists.
- **Depends.** H-01.
- **Done when.** Two proved plans compete on a real workload and cost decides. Cost never repairs a missing proof.
