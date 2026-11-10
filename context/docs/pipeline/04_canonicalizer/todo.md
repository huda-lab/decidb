# Stage 04 — Canonicalizer: open work

Open work in the shape boundary (`ClassifyScoped`, `PeelScale` and the product form). Finished work is in
[done.md](done.md).

## CAN-2 — One objective sense and one power spelling

- **Problem.** A rule reads the objective as written, so equal objectives in different spellings reach it as different
  trees. O1 declines `MAXIMIZE -k*ABS(a - b)`, which is `MINIMIZE k*ABS(a - b)`, and declines a data-only square
  written `**`, `^` or `pow`, because only expressions that hold a decision are canonicalized today; the binder and
  `IsDecidePowerCall` already accept all four spellings.
- **Change.** The canonicalizer writes every objective as a minimization (a `MAXIMIZE` becomes the `MINIMIZE` of its
  negation) and every power, data-only expressions included, in one standard spelling, so a rule reads one form.
  Messages keep quoting the objective as the user wrote it. Design it with the owner when picked (owner's direction,
  2026-11-10).
- **Done when.** O1 answers `MAXIMIZE -k*ABS(a - b)` and squares written with `**`, `^` and `pow` under `require`,
  checked by oracle cases in `test_direct_o1_monge_assignment.py`, and `./test/decide/run_tests.sh -n 2 -k
  "o1_monge or canonicalize"` passes.
