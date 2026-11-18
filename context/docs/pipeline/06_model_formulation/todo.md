# Stage 06 — Model formulation: open work

Open work in the linearizations this stage emits. Finished work is in [done.md](done.md). How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## FORM-6 — HiGHS and a concave square in the objective

- **Decision needed.** `MAXIMIZE sum((x - score) * (score - x))` answers on Gurobi; under `DECIDB_FORCE_SOLVER=highs`
  it is refused as "multiplies two decision variables, which needs Gurobi". The product group is not marked a square,
  so `IsNonconvexProductGroup` reads it as bilinear.
- **Options.** 1. Recognise the square, so HiGHS solves the concave maximization (a small change in the product
  group's classification). 2. Keep the refusal and say in the guide that HiGHS refuses it.
- **Recommendation.** 1: the problem is convex as written; the refusal comes from a missed classification. Check
  first whether HiGHS solves a convex quadratic over integer decisions; if not, that case keeps a refusal that says
  so.
- **Ruling (owner, 2026-11-18).** Option 1: the product group recognises the square, so HiGHS solves the concave
  maximization over continuous decisions. Over integer decisions HiGHS refuses a quadratic objective (MIQP) at plan
  time; that refusal states its kind and where (R12).
- **Done when.** An oracle test under `DECIDB_FORCE_SOLVER=highs` passes;
  `./test/decide/run_tests.sh -n 2 -k "bilinear or square"` passes.

## FORM-7 — A refusal names an internal column

- **Decision needed.** `MINIMIZE SUM(b * ABS(x - score))` says "requires a finite upper bound on variable
  '__abs_aux_0__'. Add a constraint like '__abs_aux_0__ <= <bound>'" (`linearization_bilinear_abs.cpp`).
- **Options.** 1. By R12 (`context/opa_rewrite/decisions.md`), the message states the kind of error and points at
  the `ABS(...)` the user wrote. 2. The helper also takes its bound from its operand's, so the refusal is left for
  an unbounded operand only.
- **Recommendation.** 1: an unbounded operand is refused under either option, so the message is needed either way.
- **Ruling (owner, 2026-11-18).** Option 1: the message states the kind of error and points at the `ABS(...)` the
  user wrote (R12); the helper's bound is unchanged.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "abs"` passes.

## FORM-8 — One record per kind of model row

- **Problem.** `EvaluatedConstraint` has about ten construct-specific fields; a hard MAX arrives spelled as an
  ordinary `SUM(e) >= K` row, so readers carry skip lists (`IsPlainLinearRow`). The provenance triple (source
  clause, removal group, repair group) is declared in 13 structs and copied field by field at three sites, one of
  which drops three fields.
- **Change.** Plain rows plus one typed list per construct; one provenance value type.
- **Needs.** OPT-26, OPT-27.
- **Done when.** `grep -rn "IsPlainLinearRow" src` finds nothing; `./test/decide/run_tests.sh -n 2 -k "minmax or abs
  or diagnose"` passes.
