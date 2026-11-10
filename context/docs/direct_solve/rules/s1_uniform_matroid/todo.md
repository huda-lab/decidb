# S1 (uniform-matroid optimization) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../../framework/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## S1-20 — S1 flips a comparison with DuckDB's function

- **Problem.** S1 keeps its own `FlipComparison` (`s1_uniform_matroid_rule.cpp:166`), a copy of DuckDB's
  `FlipComparisonExpression` (`src/common/enums/expression_type.cpp:290`) for the comparisons S1 flips.
- **Change.** S1 calls DuckDB's function, and its copy goes.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k s1_uniform` passes.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.
