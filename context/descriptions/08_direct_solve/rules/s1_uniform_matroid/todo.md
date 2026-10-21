# S1 (uniform-matroid optimization) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../../framework/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## S1-18 — S1 builds its expressions with `ExpressionBuilder`

- **Needs.** `../../framework/todo.md` § H-25.
- **Problem.** S1 builds every expression by hand (`make_uniq<Bound...>`, 51 times in the rule and 17 in its helpers)
  and never uses `ExpressionBuilder`; `AnyCondition` copies `ExpressionBuilder::Any`. `SourceLimit` takes about 30
  lines for what the builder writes in about 8.
- **Change.** Convert `s1_uniform_matroid_rule.cpp` and `s1_uniform_matroid_helpers.cpp` to the builder and delete
  `AnyCondition`.
- **Done when.** The S1 suites pass with no edit to a test file, and a hand-built expression remains only where the
  builder has no operation for it.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.
