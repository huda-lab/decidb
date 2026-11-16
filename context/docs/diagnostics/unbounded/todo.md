# Query Diagnostics — Unbounded — open work

Open work in the unbounded diagnosis. Current behavior is in [done.md](done.md). How to work and the checks are in
`../../direct_solve/framework/todo.md` § How to work any task.

## UNB-2 — Unbounded DIAGNOSE quotes an integer key

- **Problem.** Unbounded rows print `group = id = '11'` for an integer key, and `group = g = 'c'` where the infeasible
  diagnosis prints `group = c` (`context/opa_rewrite/review/09_user_facing_behaviour.md`, item S7, query d5).
- **Change.** The spelling INF-4 sets: the key as the query writes it, `id = 11`, `t.g = 'c'`. Both diagnoses use one
  renderer.
- **Needs.** INF-4 (the shared spelling).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k query_diagnostics_unbounded_variables` passes with the new
  spellings pinned.
