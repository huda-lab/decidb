# Stage 02 — Binder: open work

Language-surface work is owned by `../../language/` (the scopes:
`../../language/scopes.md`); what the language does not yet accept is listed in
`../../language/errors.md` §2, and the scope suites in
`../../testing/scope_suite.md`.

## BIND-2 — Bind-time messages state the kind and where

- **Problem.** The bind-time rows of `language/errors.md` §1 and §2 tell the user what to write (`join that table into
  this query's FROM and write the clause with per`, `write the scope in front of the constraint or inside a reducer`).
- **Change.** By the owner's rulings (2026-11-18, `context/opa_rewrite/decisions.md` R12, R19): each says what is wrong
  and points at it, and stops. `errors.md` quotes the new text.
- **Needs.** TEST-15 (the tests the rewording breaks move to its helper).
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "binder or scope"` passes.

## BIND-3 — A user name that contains a DECIDE tag is refused

- **Problem.** A DECIDE tag is a substring of an expression's name, and a column's name is the user's:
  `context/opa_rewrite/repro/marker_names.sql` query 2 (`"w__query_wide_value__"`) breaks a constraint, query 3
  (`"__definition_function_0__"`) raises an INTERNAL error.
- **Change.** By the owner's ruling (2026-11-18, `context/opa_rewrite/decisions.md` R20): until Phase 5 removes the
  tags, the binder refuses a decision or a column a DECIDE clause reads whose name contains a DECIDE tag, with a
  message that names the identifier (R12). One check against the tag list in `enums/decide.hpp`, no case list.
  Phase 5 deletes it with the last tag.
- **Done when.** Repro queries 2 and 3 raise the refusal, as tests; `./test/decide/run_tests.sh -n 2 -k "binder"`
  passes.

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
