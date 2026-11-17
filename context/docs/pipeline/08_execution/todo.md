# Stage 08 — Execution: open work

Open work in the tree evaluator and the operator around it. Finished work is in [done.md](done.md). How to work and
the checks: [`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in
these todo files.

## Formulation stays in stage 06

The top-down descent, per-node containers, bottom-up evaluation and frame pass are the deck's (pp. 81-110), and P9's F
nodes own ABS envelopes, Boolean products, L0 indicators and nonlinear reducer definitions. The deck's step 5,
formulation at the nodes (pp. 111-119), is not built, by ruling Q0.4: stage 06 writes the rows, because D89 needs
every reader and the implied bounds (done.md, "Departure from the deck"). Building it means re-ruling D89 so a G can
decide at its node, and replacing `InlineReducerValueLinks`, `EmitExtremumLink` and `LinearizeMinMaxConstraints`
rather than composing records beside them.

Other documented edge differences remain in `done.md` §1a: the D88 variant-name edge, side-plan versus tree error
order for a gated reducer argument, and unreferenced hidden records. Direct-solve rules and DIAGNOSE's separate
named-column scan also bypass or revisit parts of the normal tree execution; their scope is open.

## EXEC-13 — done.md explains the tree by the deleted flat evaluator

- **Problem.** `done.md` lines 98-461 describe the tree evaluator by comparison with the deleted flat evaluator, with
  dates and history, where the docs state the current system and its reasons.
- **Change.** Each passage states the tree's rule in its own terms, as the comments do; dates and history
  go. "Flat" meaning a non-nested objective stays.
- **Done when.** Those sections name the flat evaluator nowhere and hold no dates.

## EXEC-14 — Finalize's comments number phases the code does not have

- **Problem.** `physical_decide.cpp` and `physical_decide.hpp` label `Finalize`'s steps "PHASE 1.6", "PHASE 2,
  sub-phase 3", "PHASE 3a" and so on (15 places). The numbering is the flat evaluator's: "PHASE 1.6 -- scope checks"
  heads code that checks no scope, and PHASE 2 is `EvaluateTree`.
- **Change.** Each banner and comment names the step by what its code does (`EvaluateTree`, `BuildSolverInput`,
  `FormulateModel`, `FinalizeSolveResult`), with no number; a banner over no step goes.
- **Done when.** `grep -rn "PHASE [0-9]" src` finds nothing; the diff changes comments only; `make release` builds.
