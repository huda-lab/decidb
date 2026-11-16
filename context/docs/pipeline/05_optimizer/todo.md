# Stage 05 — Optimizer: open work

Open work in the optimizer passes. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## OPT-6 — An ABS of a total in the objective is read per row

- **Problem.** `SELECT id, x FROM (VALUES (1, 3), (2, 3), (3, 2)) t(id, cap) DECIDE x(INT) SUCH THAT x <= cap AND
  x >= 0 MAXIMIZE 0 * sum(x) + abs(sum(x) - 2)` answers a total of 0 (objective 2). The optimum is a total of 8
  (objective 6), which mainline (`a50fba79c9`) answers. The tree's lowering reads `abs(sum(x) - 2)` once per row
  instead of once for the query. Without the `0 * sum(x) +` term, the outermost-function check refuses the objective,
  so the extra term is what reaches the wrong path. Recorded as D-3 in `context/opa_rewrite/merge_notes.md` § 10.
- **Change.** Port mainline's handling of an ABS over a reducer into OPA's lowering (`LowerDecideTree`,
  `src/optimizer/decide/decide_linear_form.cpp`), under ruling R2 and the owner's ruling of 2026-11-17
  (`context/opa_rewrite/handoff.md` § A6, item 11). If the fix needs more than a port, stop and bring the design to
  the owner. A refusal in place of the answer does not close this task.
- **Done when.** Oracle tests of this query, its MINIMIZE twin and a keyed twin (`abs(sum(x) by (k) - 2)`) pass on
  both backends and fail on the unfixed code. `./test/decide/run_tests.sh -n 2 -k abs_linearization` passes.

## OPT-7 — A constant bound under `per g:` holds on the row whose key is NULL

- **Problem.** `context/opa_rewrite/repro/null_key_and_anr_defects.sql`, query S1: `per g: x <= 2` caps the row whose
  `g` is NULL at 2. `context/docs/language/guide.md` § 6.2 gives that row no constraint, so it reaches 10. The same
  holds for `per g, h:`, `2 * x <= 4`, `x = 2`, `BETWEEN` and a MINIMIZE twin, with direct solve on or off. A bound
  read from the rows (`per g: x <= cap`) and `when id > 0 per g:` leave the NULL-key row free, so the defect lies in
  the path a constant bound takes. Mainline answers S1 the same way (handoff A7, item 3).
- **Change.** The code follows the guide (owner, 2026-11-17; handoff § A6, item 7): a row whose written `per` key is
  NULL takes no instance of the clause. Find where the constant-bound path loses the key, and make it skip a NULL key
  as the row-bound path does.
- **Done when.** Oracle tests of each spelling above pass on both backends and fail on the unfixed code:
  `./test/decide/run_tests.sh -n 2 -k null_key` and `DECIDB_TEST_DIRECT_SOLVE=off ./test/decide/run_tests.sh -n 2 -k
  null_key`. `./test/decide/run_tests.sh -n 2 -k scope_declare` passes.
