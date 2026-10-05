# Follow-on Work — open work

These items are **not** part of the first S1 vertical slice. Each promotion
requires a credible workload, exact admission proof, permanent differential
tests, and end-to-end benefit evidence. The Word catalogue owns class details.

Each task below can be picked up on its own. When a task is done, its result moves
to [done.md](done.md) and the task is deleted from this file.

**Shared rule for NEXT-06 through NEXT-08.** The solver raises a NULL source-valued
bound even on a row whose `WHEN` is false or whose `PER` key is NULL, and an empty
scoped aggregate can raise before that bound error. Every wider admission keeps that
error order. Anything not proved stays on the solver path.

- [ ] **NEXT-01 — Second rule that proves harness reuse.**
  - Goal: register a materially different rule through the existing adapter,
    coordinator, result boundary, policy, and explanation path.
  - Depends: HAR-01 through HAR-06, VAL-05 and VAL-08 (done).
  - Chosen class (2026-10-04): **A1, independent bounded decisions**, from the Word
    catalogue. It exercises what S1 does not: `INT`/`REAL` domains, per-row bounds, a
    quadratic objective, and a plan that streams and so needs `DirectValidationBarrier`.
    The step-by-step queue is the scratchpad `context/descriptions/todo.md`; its
    decisions and findings move into tracked docs as each step lands.
  - Evidence and code: `src/optimizer/decide/direct/` (`direct_coordinator.cpp`,
    `direct_result_boundary.cpp`, `direct_builder.cpp` are the shared parts;
    `s1_rule.cpp` is the model rule), contract in `direct_rule.hpp`, the
    [add-a-rule checklist](../README.md#adding-a-rule). This is the same
    gate as HAR-02 in [01_harness/todo.md](../01_harness/todo.md).
  - Done when: the new rule is added to `RegisteredDirectRules()` with no edit to the
    coordinator logic, builder, or result boundary beyond that registration; it has
    its own proof and tests for a hit, a miss that falls back to the solver, and the
    `require` message.
  - Moves to: [done.md](done.md), and closes HAR-02 in [01_harness/done.md](../01_harness/done.md).
- [ ] **NEXT-03 — Wider keyed application.**
  - Goal: extend beyond the admitted row-scoped Boolean slice (source-column `PER`,
    matching top-level `WHEN`, aggregate-local `WHEN`, NULL-key bypass, paired bounds)
    to other keyed shapes.
  - Depends: VAL-05 (done).
  - Evidence and code: [NEXT-03 slice in done.md](done.md), `s1_rule.cpp`.
  - Done when: component independence and row/entity identity are proved and tested
    for the new shape. Estimates never establish independence.
  - Moves to: [done.md](done.md).
- [ ] **NEXT-04 — New-language / ANR adapter.**
  - Goal: map the new language's expression semantics into the shared problem and
    exact-fact vocabulary.
  - Depends: a stable language branch, HAR-01, VAL-05.
  - Evidence and code: `src/optimizer/decide/direct/direct_problem.cpp` (current
    bound-tree adapter), [architecture](../00_design/architecture.md).
  - Done when: the same adapter contract and direct/solver tests run against both
    syntaxes where comparable. Facts are extended when semantics change, not by
    teaching rules parser spelling.
  - Moves to: [done.md](done.md).
- [ ] **NEXT-05 — Cost choice among direct plans.**
  - Goal: pick between competing proved direct plans by cost.
  - Depends: NEXT-01.
  - Evidence and code: `Cost` in `direct_rule.hpp`; the coordinator already picks the
    cheapest proved candidate.
  - Done when: two proved plans compete on a real workload and cost decides between
    them. Cost never repairs a missing proof.
  - Moves to: [done.md](done.md).
- [ ] **NEXT-06 — Numeric source-valued per-row pins.**
  - Goal: admit a per-row bound such as `x <= pin_col` where `pin_col` is a numeric
    source column. BOOLEAN-typed source pins and exact zero/one pins already work.
  - Depends: the admitted keyed slice of NEXT-03 (done).
  - Evidence and code: pin handling in `s1_rule.cpp` (`Prove`), fixed-pin results in
    [done.md](done.md).
  - Done when: the bound and error semantics are proved from the complete plan, and
    tests cover global and grouped use, `WHEN` bypass, NULL pin values, and both
    solvers. Fixed status is never inferred from a sample.
  - Moves to: [done.md](done.md).
- [ ] **NEXT-07 — Throwing source expressions as bounds.**
  - Goal: admit source-only bound expressions that can raise at runtime (a plain
    narrowing `CAST`, for example). Today only nonthrowing ones such as `COALESCE`
    and `TRY_CAST` are admitted.
  - Depends: the admitted keyed slice of NEXT-03 (done).
  - Evidence and code: `DirectMayThrow` (`direct_expression.cpp`) and `Prove` in `s1_rule.cpp`; the
    NEXT-07 progress already in done.md. A multi-term
    objective whose coefficient can throw also still misses; handle it here or leave
    it, but say which.
  - Done when: the error and its order match the solver on every row, including rows
    that bypass the bound, and tests cover both solvers.
  - Moves to: [done.md](done.md).
- [ ] **NEXT-08 — Offset count bodies such as `SUM(x+0)`.**
  - Goal: decide whether `SUM(x + c)` style bodies can be admitted as a count with a
    shifted bound.
  - Depends: none beyond the admitted S1 slice.
  - Evidence and code: `IsUnitContribution` in `s1_rule.cpp`; bodies whose terms add up
    to exactly one `x` (`SUM(1*x)`, `SUM(2*x - x)`) are admitted today, and a constant
    term in the split body (`DecideTermKind::CONSTANT`) misses.
  - Done when: either an exact proof and tests admit the form, or the near miss is
    pinned in a test as a permanent solver case with a reason.
  - Moves to: [done.md](done.md).

Independently scoped clauses, such as quotas on two potentially overlapping subsets,
need a separate class and are not a wider S1 matcher. Do not add syntax cases without
a semantic reason and a workload benefit.

## Suggested batches

| Batch | Tasks | Why together |
| --- | --- | --- |
| A | NEXT-06, NEXT-07, NEXT-08 | Same `Prove` function, same error-order rule. The seeded fuzz test exists, so extend its generator to cover each form you admit |
| B | NEXT-01 | A1 is chosen; follow the queue in `context/descriptions/todo.md` |
| C | NEXT-03, NEXT-04, NEXT-05 | Wait on other work (language branch, a second rule); not for now |

**Exit gate for each item:** independent proof, behavior tests, and performance
evidence; merely finding a relational expression in the catalogue is not enough.
