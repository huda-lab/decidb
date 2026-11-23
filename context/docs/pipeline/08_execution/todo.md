# Stage 08 — Execution: open work

Open work in the tree evaluator and the operator around it. Finished work is in [done.md](done.md). How to work and
the checks: [`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in
these todo files.

## Formulation at the nodes is Phase 6

The top-down descent, per-node containers, bottom-up evaluation and frame pass are the deck's (pp. 81-110), and P9's F
nodes own ABS envelopes, Boolean products, L0 indicators and nonlinear reducer definitions. The deck's step 5,
formulation at the nodes (pp. 111-119), is not built: the emitter hands stage 06 arrays as long as the input, and
stage 06 decides where a helper is written in. Phase 6 builds it as MODEL-1 to MODEL-7 ([`../todo.md`](../todo.md);
design: `context/opa_rewrite/design.md` § 4).

Other documented edge differences remain in `done.md` §1a: the D88 variant-name edge and unreferenced hidden
records. Direct-solve rules and DIAGNOSE's separate
named-column scan also bypass or revisit parts of the normal tree execution; their scope is open.
