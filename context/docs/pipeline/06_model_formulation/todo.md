# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## FORM-11 — A helper's rows name the clause that reads the helper

- **Problem.** The rows that define a MIN, MAX or reducer helper (`linearization_reducer_value.cpp`, the sum row and
  `EmitExtremumLink`) carry no source clause. HiGHS's load-window refusal (`ThrowUnscalableRow`,
  `deterministic_naive.cpp`) reads an unclaimed row as the objective's, so it names the objective for a constraint's
  helper. Over the rows `(1, 1e308)` and `(2, 1e308)` as `t(id, b)`, on HiGHS with direct solve off,
  `x(REAL), y(REAL) SUCH THAT x <= 5 AND y <= 5 AND SUM(x) + MAX(b * y) <= 10 MAXIMIZE SUM(x)` gives `the objective
  holds numbers too far apart in size`, with the caret at `MAXIMIZE`.
- **Change.** The helper's rows take the link's clause (`ReducerValueLinkSpec::source_clause_id`) as their origin,
  so every reader of a row's clause reads one field. DIAGNOSE shows nothing new: its removal groups key on
  `removal_group_id`, which a helper row never sets; its labels skip `STRUCTURAL` rows; its elastic blocks take only
  `USER_PARAMETER` rows. `ThrowUnscalableRow`'s comment then states that a row with no clause is the objective's.
- **Done when.** The query above names `SUM(x) + MAX(b * y) <= 10` on HiGHS;
  `./test/decide/run_tests.sh -k "past_a_double or too_far_apart or diagnose"` passes.
