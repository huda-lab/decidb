# Correctness — open work

Correctness tests assert both **path selection** and **result semantics**. Tied
decision vectors need not match; schema, cardinality, feasibility, and primary
objective must. Use an independent solver oracle for representative cases so
two DeciDB paths cannot share one unnoticed mistake.

The first-slice correctness gate is met: all admitted test cases preserve the
observable contract, all tested non-admitted cases use the unchanged solver
path, and serializer/prepared-plan coverage confirms the result boundary in
multiple execution shapes. See [done.md](done.md) for the one forced-HiGHS
failure unrelated to this S1 rule.

Each task below can be picked up on its own. When a task is done, its result moves
to [done.md](done.md) and the task is deleted from this file.

- [ ] **VAL-06 — Direct-vs-oracle cases with the `oracle_solver` fixture.**
  - Goal: check a few direct results against the independent Python oracle, not
    only against the two DeciDB solver backends.
  - Depends: none. Needs a valid Gurobi license, as the rest of the oracle suite does.
  - Evidence and code: the `oracle_solver` fixture in `test/decide/conftest.py`;
    existing cases in `test/decide/tests/test_direct_solve.py`.
  - Done when: a small set of cases (global bound, `PER` group, `WHEN`, source-valued
    bound, pins) runs direct mode and the oracle, and compares schema, row count,
    feasibility, and primary objective. Tied selections need not match.
  - Moves to: [done.md](done.md).
- [ ] **VAL-07 — Permanent seeded fuzz test.**
  - Goal: turn the throwaway seeded fuzz check into a pytest case.
  - Depends: none. Do it before [NEXT-06 through NEXT-08](../05_follow_on/todo.md).
  - Evidence and code: the generator was a scratch script used during review and is
    not in the repository. It builds random small sources (NULL keys, NULL caps,
    ties), random `PER`/`WHEN`/pins, bound forms, and objectives, then runs `off` and
    `auto` and compares them.
  - Done when: a fixed list of seeds runs in the normal suite, compares error class
    on failures and row count plus primary objective on success, and ignores the
    selected count when only a zero-score row differs.
  - Moves to: [done.md](done.md).

## Suggested batches

| Batch | Tasks | Why together |
| --- | --- | --- |
| A | VAL-06, VAL-07 | Both extend `test_direct_solve.py` with independent references |
