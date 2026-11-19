# Stage 02 — Binder: open work

Language-surface work is owned by `../../language/` (the scopes:
`../../language/scopes.md`); what the language does not accept is listed in
`../../language/errors.md`, and the scope suites in
`../../testing/scope_suite.md`.

## BIND-4 — The binder judges algebra by name

- **Decision needed.** About 1,340 of `decide_binder.cpp`'s 2,098 lines are validators that classify the parsed tree
  by name, beside binding, with different answers for one property: `MAXIMIZE sum(v) + 1` is accepted with no
  decision in it; `MINIMIZE abs(sum(x) - 2)` and `MINIMIZE power(sum(x) - 3, 2)` are refused though the same algebra
  solves as a constraint; "the objective reads no decision" has four messages. The decision: are the two objective
  refusals real gaps or stale gates, and is `MAXIMIZE sum(v) + 1` refused?
- **Options.** 1. Switch each gate off and test; a gate that guards nothing goes. 2. Keep the refusals as they are.
- **Recommendation.** 1.
- **Change.** The binder refuses only what needs the parsed tree (scope placement, user casts, names). "Reads no
  decision", vocabulary, reducer-argument shape and degree become one check in `DecideShapeCheck` (stage 04), the
  same code for a constraint side and the objective. About 400 to 600 lines go.
- **Done when.** One message per property; `./test/decide/run_tests.sh -n 2 -k "binder or objective or shape"`
  passes.

## BIND-5 — Two binder gates exist for wording or a cause that is gone

- **Problem.** The integer-step gate is implemented twice in `decide_binder.cpp`: the parsed half (about 125 lines)
  for its wording, the bound half predicting stage 04 from stage 02. `ValidateDecideNoDataAggregateFactor` cites a
  layer the canonicalizer replaced: `sum(x * avg(v)) <= 8` solves while `x * avg(v) <= 8` is refused.
- **Change.** The parsed half goes; the bound half runs after canonicalization; the data-aggregate gate is switched
  off and tested, and goes if it guards nothing.
- **Needs.** BIND-4.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "binder or integer"` passes.

## BIND-6 — Two binder messages name the wrong fragment

- **Problem.** A refusal of `3 <= CASE WHEN ... END` names `'3'`, the side that reads no decision, where the user's
  mistake is the `CASE`. The outer-query refusal names a bare `'k'` for a column written `g.k`
  (`DecideBinder::ThrowIfOuterColumn`).
- **Change.** Each message quotes the node the user wrote: the whole side, and the column as qualified.
- **Tests.** `assert_error_at` with `at=` the quoted fragment (`testing/oracle.md` §3.6).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "binder or scope"` passes.
