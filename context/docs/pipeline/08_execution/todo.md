# Stage 08 — Execution: open work

Open work in the tree evaluator and the operator around it. Finished work is in [done.md](done.md). How to work and
the checks: [`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in
these todo files.

## Formulation at the nodes is Phase 6

The top-down descent, per-node containers, bottom-up evaluation and frame pass are the deck's (pp. 81-110), and P9's F
nodes own ABS envelopes, Boolean products, L0 indicators and nonlinear reducer definitions. The deck's step 5,
formulation at the nodes (pp. 111-119), is not built: stage 06 writes the rows, because D89 needs every reader and
the implied bounds (done.md, "Departure from the deck"). Phase 6 builds it (ARCH-2, [`../todo.md`](../todo.md)):
that means re-ruling D89 so a G can decide at its node, and replacing `InlineReducerValueLinks` and `EmitExtremumLink`
rather than composing records beside them.

Other documented edge differences remain in `done.md` §1a: the D88 variant-name edge and unreferenced hidden
records. Direct-solve rules and DIAGNOSE's separate
named-column scan also bypass or revisit parts of the normal tree execution; their scope is open.
