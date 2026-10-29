# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md).

## FRM-1 — Tighten the Big-M of ABS, `<>`, hard MIN/MAX and IN

- **Problem.** `../../issues/bugs.md` § BUG-6 holds the reproductions, the cause and the owner's ruling.
- **Change.** As ruled in BUG-6: M is the signed interval of the expression, with no doubling, in ABS, `<>`, hard
  MIN/MAX and IN.
- **Done when.** A model dump of each construct shows M equal to its expression's signed interval, and the oracle tests
  pass on both backends. Goldens that print M change; the owner reads the diff before any regeneration.
- **done.md.** [done.md](done.md) § 9, "Big-M constants".
