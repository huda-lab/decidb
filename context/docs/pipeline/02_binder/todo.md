# Stage 02 — Binder: open work

Language-surface work is owned by `../../language/` (the scopes:
`../../language/scopes.md`); what the language does not accept is listed in
`../../language/errors.md`, and the scope suites in
`../../testing/scope_suite.md`.

## BIND-4 — The binder judges algebra by name

- **Problem.** About 1,120 of `decide_binder.cpp`'s 2,059 lines are validators that classify the parsed tree by name,
  beside binding, and each has a second path that reaches stage 04 or 05 with another answer: `MINIMIZE abs(sum(x) -
  2)` is refused while `MINIMIZE abs(sum(x) - 2) + 0` solves; in a frame `power(x, 2)` is a Not implemented error
  and `x ** 2` a Binder error (`RespellDecideAliases` renames `power` to `**` but leaves `is_operator` false, so
  `IsNonlinearPower` misses it); `sum(y) <= 9` over a query-wide `y` is refused while `sum(y) + sum(x) <= 9` reads
  `3 * y`; `x * avg(v) <= 8` is refused while the same through a macro (`ma(c) AS avg(c)`) solves. "The objective reads
  no decision" has three texts and one accept (`MAXIMIZE sum(v) + 1`).
- **Rulings (owner, 2026-11-21).** The objective as a whole must read a decision, one rule and one message; a data term
  beside a decision term is accepted, so `MAXIMIZE sum(x) + sum(v)` (refused today) is accepted and `MAXIMIZE sum(v) +
  1` (accepted today) is refused like `MAXIMIZE sum(v)`. A constant objective is almost always a typo, and `SATISFY`
  exists for no objective. The `abs` and `power` refusals over a reducer in the objective are switched off and tested.
- **Rulings (owner, 2026-11-21), after the architect review.**
  1. A data term beside the decisions in the objective (`+ w`, `+ sum(v)`, `+ (SELECT 3)`) stays a term: it is a
     field the objective reads, so the run-time check that the objective is produced once sees it (as it sees `w` in
     `MAXIMIZE sum(x) - w * s`), and its one value is added to the objective's constant. Not dropped in the
     canonicalizer: a `w` that differs between rows would then be accepted silently. O2 (stage 04 done.md) changes
     from "rejected" to "kept as a term".
  2. `MINIMIZE power(sum(x) - 3, 2)` stays refused, now by stage 05's `'…' multiplies decisions outside a reducer,
     which is not supported`, the one refusal of a square outside a reducer in the objective (`MINIMIZE (y - 3) * (y -
     3)` over a query-wide `y` gets it today; `power(y - 3, 2)` gets the binder's `function 'power' is not supported
     here`, which goes). The gap is OPT-31. `abs` over a reducer in the objective solves.
  3. `MAXIMIZE sum(x) * avg(v)` solves, as the constraint form does: a data reducer with no `by` is one query-wide
     value (its placeholder is tagged query-wide, so `IsQueryWideConstant` accepts it). With a `by` it stays refused,
     and the message quotes the written reducer, not `'__bound_reducer__'` (`LogicalDecide::SetObjective`
     re-canonicalizes with no source to quote).
  4. A reducer whose body is query-wide decisions only (`sum(y)`) is refused at every position, constraint side,
     beside another term or in the objective: its two readings (1 or n times `y`) are different problems (guide §2).
     `sum(x + y)` and `sum(cost * y)` stay accepted.
  5. The degree and linearity checks inside a frame (`MultipliesDecisions`, `IsNonlinearPower`,
     `FindNonlinearDecisionUse`) are in this task: they move to stage 04's degree check.
  6. (BIND-5) One integer-step check, in stage 04.
- **Change: semantic analysis after binding** (the compiler front-end split). The binder keeps what needs the parsed
  tree: names (`RefuseDecideTagInName`), user casts, scope placement and frame binding, key resolution, outer columns,
  qualified decisions, volatile calls, `BindAggregate`'s kind, COUNT and FILTER checks, qualifiers, `norm` arguments,
  the NULL fold, subqueries, and in the constraints binder: unsupported comparison, comparison used as a number, not
  a comparison, IN, OR. Each property below gets one check:
  - **Reads a decision** (P1): one clause-level rule in `DecideShapeCheck`, `CheckClause(whole, sides)`: a constraint
    passes (comparison, {left, right}), the objective (its body under WHEN/PER, {body}). It raises
    `'q' reads no decision` (Binder) at `whole`, then runs vocabulary and degree per side. Goes: the constraints
    binder's `GetExpressionType` (P1 arms), the objective binder's, `ValidateSumArgument`'s flag, the canonicalizer's
    per-term refusal (`decide_canonicalizer.cpp:1346`).
  - **Vocabulary** (P2): already the shape check's; the objective binder's classification and the two `'q' is not
    supported` fallthroughs go.
  - **Reducer arity and the query-wide body** (P3, P4): move into `BindAggregate`, DuckDB's hook for aggregates, so
    they hold at every position. The `norm` 0-argument arm and the `@` arity check go (duplicates).
  - **The bound side is data** (P5): `IsAllowedDecisionFreeBoundExpression` goes. A window function gets a WINDOW arm
    in `DecideBinder` (the `WhereBinder` pattern); a non-scalar subquery is already refused.
  - **Data reducer scaling a decision** (P7): the gate goes for constraints, with oracle tests for a `by`, `<` and
    `<>`; the objective follows ruling 3.
  - **Degree in a frame** (P8): ruling 5.
  - Glue that goes: `DecideRefusal`, the virtual `GetExpressionType`, the validator calls in the declarations binder.
- **Call order.** `CheckConstraints` (`plan_select_node.cpp:194`) moves to right after `CanonicalizeTree` (`:187`): a
  comparison with no decision would otherwise reach the debug `VerifyCanonical` first. The canonicalizer returns such a
  comparison unchanged, so its location survives; one rebuilt by a side swap takes its clause's location from
  `decide_constraint_sources`.
- **Behavior changes.** `abs(sum(x) - 2)` in the objective solves; `abs(v) <= 5`, `MAXIMIZE abs(3)` and `MAXIMIZE 1 +
  2` become Binder `reads no decision` (NI today) with the caret on the clause; `sqrt(sum(x))` in the objective takes
  the shape check's text; `sum(x) <= max(v, 2)` says `'max(v, 2)' has 2 arguments`; a CASE or COALESCE side over a
  decision takes the shape check's text.
- **Order.** `CheckClause` and the call move (only `sum(v) + 1` and `sum(v) * 2` flip); the canonicalizer's per-term
  refusal; the objective binder's classification; arity and the query-wide rule into `BindAggregate` with the WINDOW
  arm, and the constraints binder's classification; BIND-5; P7 and ruling 3; P8.
- **Size.** About 1,000 lines leave the binder files and 290 land in stage 04, net about −700.
- **Done when.** One message per property; `./test/decide/run_tests.sh -n 2 -k "binder or objective or shape"`
  passes.

## BIND-5 — Two binder gates exist for wording or a cause that is gone

- **Problem.** The integer-step check is raised in three places: the parsed half of `decide_binder.cpp` (about 115
  lines, kept for its wording), the bound half predicting stage 04 from stage 02, and stage 05's
  `RefuseFractionalStep` (`decide_rewrite_scoped.cpp:224`). `ValidateDecideNoDataAggregateFactor` cites a layer the
  canonicalizer replaced: `sum(x * avg(v)) <= 8` solves while `x * avg(v) <= 8` is refused.
- **Ruling (owner, 2026-11-21).** One check, in stage 04: the bound half moves into `CheckClause` (the left side of a
  `<`, `>` or `<>`, since the canonical form puts the bound on the right) and stage 05's copy goes. If a test shows a
  case only stage 05 catches, stop and report.
- **Change.** The parsed half goes (the bound half refuses a REAL decision, a DOUBLE column, already). The bound half
  moves to stage 04 and drops three duplicate helpers: `SideReferencesDecision` (`BoundExpressionReferencesDecide`),
  `TryGetConstantExponent` (`TryGetDecideWholeExponent`), `DecideExtremumBodyIsWholeNumbered` (the canonical form has
  moved the addends). The data-aggregate gate is BIND-4's P7. The message keeps its text, `'<' compares 'q', which can
  take fractional values`; it names the first fractional operand (`sum(v * r) < 5` names `v`, not `r`).
- **Needs.** BIND-4.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "binder or integer"` passes.
