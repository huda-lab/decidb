# Stage 08 — Execution: open work

Open work in the tree evaluator and the operator around it. Finished work is in [done.md](done.md). How to work and
the checks: [`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in
these todo files.

## Formulation at the nodes

The model is written at the nodes (`context/opa_rewrite/design.md` § 4, [done.md](done.md)). Three decisions of
that design are open, each with its options and a recommendation in [`../todo.md`](../todo.md): how far the
planner's two readings of a clause are joined (MODEL-6), the helper of a link whose data may split its classes
(MODEL-8), and the owner of a clause's emitted rows (MODEL-24).

Other documented edge differences remain in `done.md` §1a: the D88 variant-name edge and unreferenced hidden
records. Direct-solve rules and DIAGNOSE's separate
named-column scan also bypass or revisit parts of the normal tree execution; their scope is open.
