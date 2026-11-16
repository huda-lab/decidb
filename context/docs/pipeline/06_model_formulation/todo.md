# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## FORM-1 — An overflowing reducer bound becomes 1e30 instead of TOO_LARGE

- **Problem.** `DeriveReducerValueBounds` (`src/decidb/formulation/linearization_reducer_value.cpp`) absorbs a reach
  that overflows into 1e30, where the rest of formulation raises TOO_LARGE. The model then carries a bound the data do
  not give. Both parents of the merge behave this way (`context/opa_rewrite/merge_notes.md` § 10, found with Q-T10).
- **Change.** Raise the TOO_LARGE error the builder raises for a non-finite end, naming the clause as written.
- **Done when.** A query whose reach overflows gets TOO_LARGE on both backends; the Q-T10 row of merge_notes § 10 gives
  a starting query. `./test/decide/run_tests.sh -n 2 -k "too_large or invalid_value"` passes.

## FORM-2 — On HiGHS, a helper link's TOO_LARGE names its plan label

- **Problem.** For `max(per id: sum(b * y) by (id))`, the TOO_LARGE message on HiGHS names the helper's plan label,
  where Gurobi names the clause as written. `linearization_reducer_value.cpp` sets `spec.written = link.label`.
- **Change.** The link carries the clause as written (its source span) into `spec.written`.
- **Needs.** FORM-1 (same file).
- **Done when.** A message test passes on both backends: `./test/decide/run_tests.sh -n 2 -k <test>` and
  `DECIDB_FORCE_SOLVER=highs ./test/decide/run_tests.sh -n 2 -k <test>`.

## FORM-3 — `close_rounded_box` has no test

- **Problem.** `close_rounded_box` (`src/decidb/formulation/ilp_model_builder.cpp`, two call sites) closes a variable
  box that floating-point rounding inverted by at most one part in 1e9. No test reaches it. Its own example,
  `3 * x <= 0.3 AND x >= 0.1`, comes back infeasible on Gurobi either way, from Gurobi's presolve tolerance.
- **Change.** By the owner's ruling (handoff § A6, item 11): find a query whose answer depends on it and test that
  query, or delete it.
- **Done when.** A test fails with the function removed, or the function is gone and
  `./test/decide/run_tests.sh -n 2 -k "bounds or edge_cases"` passes.
