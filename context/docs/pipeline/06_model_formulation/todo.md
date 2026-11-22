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
- **Decision needed.** The helper link already holds the clause that first reads the helper
  (`ReducerValueLinkSpec::source_clause_id`); the too-large refusal names it. Copying it into the rows' origin is the
  one-line fix, but DIAGNOSE reads a row's origin: removal groups (`decide_diagnostic_engines.cpp`) and row labels
  (`decide_diagnostic_render.cpp`). Check whether a structural row with a clause joins that clause's removal group or
  takes its label. If it does not, copy the clause. If it does, the refusal reads the clause of the first row that
  reads the helper's column, as the hand-off check in `ilp_model_builder.cpp` does. Recommendation: the copy, if the
  check allows it, since every reader then reads one field.
- **Done when.** The query above names `SUM(x) + MAX(b * y) <= 10` on HiGHS;
  `./test/decide/run_tests.sh -k "past_a_double or too_far_apart or diagnose"` passes.
