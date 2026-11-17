# Stage 01 — Parser: open work

Open work in the DECIDE grammar and the text it keeps. Finished work is in [done.md](done.md). How to work and the
checks: [`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these
todo files.

## PARSE-1 — A declaration's `per` key has no written text to quote

- **Problem.** `DecideSourceText` holds the tail, `SUCH THAT` through the objective
  (`transform_select_node.cpp`), so a key written in a declaration (`DECIDE per D.region: x(REAL) ...`) has no span.
  A message that cuts a column's text from its query location falls back to the bound name there: DIAGNOSE prints
  `region` where the query wrote `D.region` when the declaration is the column's only reader, and a frame's span
  message prints a declaration key as DuckDB prints it.
- **Change.** The source text covers the declarations too: the kept text starts at `DECIDE`, and the grammar emits a
  KEY span for a declaration's `per`, as it does for a clause's. The readers cut the text as they do in the tail.
- **Done when.** A DIAGNOSE test whose only reader of `D.region` is `DECIDE per D.region: x(REAL)` finds
  `D.region = ...` in its group; `./test/decide/run_tests.sh -n 2 -k "query_diagnostics or scope_declare or echo"`
  passes, and `DECIDB_VERIFY_SERIALIZER=1` on the same pattern.
