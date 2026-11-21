# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## FORM-10 — The model builder writes two diagnosis fields that nothing reads

- **Problem.** Under `DIAGNOSE` the model builder fills `ConstraintProvenance::weight_labels` (the column name of a
  data-weighted term) and `folded_terms` (the coefficient the user wrote for each summed decision, on the accumulating
  path), from `EvaluatedConstraint::coefficient_names`, which value extraction fills (`NameCoefficient`). Nothing in src
  reads either field: a diagnosis label quotes the clause as written (`SourceAwareLhs`). Only unit checks in
  `test_decidb_constraint_provenance.cpp` read them.
- **Change.** Delete `weight_labels`, `folded_terms`, `FoldedAggTerm`, the builder's `StampWeightLabels` and
  folded-terms block, `EvaluatedConstraint::coefficient_names`, `CoefficientName` and `NameCoefficient`, with the unit
  checks that read only them. `TermsHaveTheirOwnColumns` stays: the builder's row reader also calls it. The direct
  path's `coefficient_names` (`DecideNumberNames`) is a different field and stays.
- **Done when.** `grep -rnE "weight_labels|folded_terms|FoldedAggTerm|CoefficientName\b" src test` finds nothing;
  DIAGNOSE output does not change; `./test/decide/run_tests.sh -k "diagnose"` and `build/release/test/unittest
  "[decidb]"` pass. Docs: 06 done.md § 7's paragraph on the two fields, foundations done.md's mention.
