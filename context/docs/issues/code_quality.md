# Code quality

Code-quality issues include duplication, dead code, fragile patterns, unclear
naming, missing coverage, and diagnostic-quality defects that do not change the
ordinary optimization result. Solve-correctness bugs belong in [bugs.md](bugs.md).

Each entry: short title, location (`file:line`), what's wrong, why it matters,
and when/during which task it was discovered.

## FMT-1 — DeciDB sources indented with spaces fail the formatter

Location: nine files under `src/decidb/` (among them `src/decidb/formulation/ilp_model_builder.cpp`,
`ilp_linearization.cpp`, `linearization_bigm.cpp`, `linearization_minmax.cpp`, `linearization_not_equal.cpp`), one in
`src/execution/operator/decide/` and two in `src/planner/expression_binder/decide/`. They are indented with four
spaces where `.clang-format` asks for tabs, and `make format-check` (which covers `src/`) reports them. A change that
formats only its own lines, as the repository asks, leaves tab lines among space lines in them. The fix is one
formatting-only commit, `make format-fix` limited to these files, made when no lane has edits open in them; it changes
no behavior. Found 2026-11-08 in BLOAT-30's review.
