# Stage 01 — Parser: open work

Open work in the DECIDE grammar and the text it keeps. Finished work is in [done.md](done.md). How to work and the
checks: [`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these
todo files.

## PARSE-4 — Two span roles of a BETWEEN are never read

- **Problem.** The grammar records the two halves of a `BETWEEN` as span roles `BETWEEN_INPUT` and `BETWEEN_BOUNDS`
  (`noteDecideBetween` in `grammar.cpp`, `parsenodes.hpp`, `DecideSpanRole`), and no reader cuts either.
- **Change.** Delete the two roles and `noteDecideBetween`.
  Needs `make grammar-build`.
- **Done when.** `grep -rn "BETWEEN_INPUT\|BETWEEN_BOUNDS" src third_party` finds a reader or nothing;
  `./test/decide/run_tests.sh -n 2 -k "parser or between"` passes.
