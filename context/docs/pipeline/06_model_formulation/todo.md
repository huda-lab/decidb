# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## FORM-5 — Three helper-link messages name the plan label, not the clause as written

- **Problem.** A helper link carries the clause that first read it (`ReducerValueLinkSpec::source_clause_id`), and
  the extremum link's TOO_LARGE quotes it as written. Three other sites raise TOO_LARGE with the plan label, the reducer
  as the plan prints it (lowercased): `BeginForDecisions` and `AddCoefficient` in `LinearizeReducerValueLinks`
  (`src/decidb/formulation/linearization_reducer_value.cpp`), and a non-finite member constant in
  `DecideModelEmitter::ValueLinks` (`decide_model_emitter.cpp`). One family of messages spells its clause two ways.
- **Change.** Each site quotes the clause through `DecideWrittenClause`, as the extremum link does. A test whose
  expected text changes goes to the owner with the old and new text before it is edited.
- **Done when.** A test per site, written in upper case so the label and the clause differ, finds the clause as
  written; `./test/decide/run_tests.sh -n 2 -k "too_large or invalid_value"` passes.
