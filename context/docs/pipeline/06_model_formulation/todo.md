# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## FORM-8 — One record per kind of model row

- **Problem.** `EvaluatedConstraint` has about ten construct-specific fields; a hard MAX arrives spelled as an
  ordinary `SUM(e) >= K` row, so readers carry skip lists (`IsPlainLinearRow`). The provenance triple (source
  clause, removal group, repair group) is declared in 13 structs and copied field by field at three sites, one of
  which drops three fields.
- **Change.** Plain rows plus one typed list per construct; one provenance value type.
- **Needs.** OPT-25, OPT-27.
- **Done when.** `grep -rn "IsPlainLinearRow" src` finds nothing; `./test/decide/run_tests.sh -n 2 -k "minmax or abs
  or diagnose"` passes.
