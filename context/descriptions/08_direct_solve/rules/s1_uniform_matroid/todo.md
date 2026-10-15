# S1 (uniform-matroid optimization) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../_harness/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## S1-12 — S1 reads a per-row bound on `x` in its own way, and so does A1

- **Needs.** The shared per-row range reader (`../r1_continuous_knapsack/todo.md` § R1-01) with a third rule calling it
  (§ R1-02). S1 is heavily tested, so changing its pins has to earn its risk.
- **Problem.** S1 decides "which of {0, 1} may this row take" for a per-row bound on `x` with its own code
  (`FixedDomain`, `ConstantFixedDomain`, `SourceBooleanFixedValue`, and the numeric fixed-value code in
  `ProveFixedVariables`, about 140 lines). A1 decides "what interval may this row take" for the same kind of bound with
  different code (`ClauseBounds` in `a1_separable_box_rule.cpp`). They also treat the solver's reading of a bare
  constant bound differently: S1 declines some spellings by inspecting them, A1 asks the `variable_bound` IR flag, which
  S1 never reads. Two readings of one rule can drift apart, and a third per-row rule would write a third.
- **Change.** Give S1's pins the shared reader with the domain {0, 1}; keep S1's rank and count logic as it is.
- **Done when.** S1's pin code is a call to the shared builder, S1's suites (three-way, contract, fuzz) pass unchanged, and
  the A1 suites still pass. About 1 day.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.
