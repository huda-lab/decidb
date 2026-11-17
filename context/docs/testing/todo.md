# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md). How to work and the checks:
[`../direct_solve/framework/todo.md`](../direct_solve/framework/todo.md) § How to work any task in these todo files.

## TEST-6 — One suite run on a debug build

- **Problem.** No debug build ran the merged branch, so its debug-only assertions never ran
  (`context/opa_rewrite/merge_notes.md`, trap 6).
- **Change.** By the owner's ruling (handoff § A6, item 11): build debug once, after every other task in the queue,
  and run `make decide-test` against it in the background. It is a long CPU-bound run, so confirm the start with the
  owner.
- **Needs.** Every other task of `context/docs/todo.md`.
- **Done when.** The run passes or each failure is triaged, and the result is in `merge_notes.md` § 4.1.
