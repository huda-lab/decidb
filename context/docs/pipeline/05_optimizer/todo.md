# Stage 05 — Optimizer: open work

Open work in the optimizer passes. Finished work is in [done.md](done.md).

## OPT-5 — The ABS rewrite names the call as written

- **Problem.** The ABS rewrite composes its auxiliary's label by hand, `"abs(" + RenderForPlan(argument) + ")"`
  (`decide_rewrite_abs.cpp:470`), so it prints the name `abs` whatever the user wrote. With
  `SET decide_direct_solve='off'`, the `EXPLAIN` of `MINIMIZE SUM(@(x - v))` shows `abs(x - v)` in the rewritten
  layer, where the query and canonical layers show `@(x - v)`. The name a call was written under is a tag on the call
  (`WRITTEN_NAME_TAG_PREFIX`, `../02_binder/done.md` § One spelling per function), and the renderer reads it.
- **Change.** The label renders the call itself (`RenderForPlan` over the `@` node), so no rewrite spells a function's
  name.
- **Done when.** That `EXPLAIN` names `@(x - v)` in every layer, and `ABS(x - v)` reads `abs(x - v)` there as it does
  today; `./test/decide/run_tests.sh -n 2 -k "abs_linearization or explain or alias"` passes.
