# Query Diagnostics — Infeasible (remaining work)

The elastic engine is complete. The entries below are deferred output-policy or
determinism improvements; current behavior is recorded in `done.md`.

## Repair-set tie policy is not a total order

The stage-2b rank tie-break covers every repair kind, but several exact ties can
still be backend-dependent:

- two multi-edit sets can have the same rank sum;
- quadratic-objective ties skip the tie-break because freezing the objective
  would require a quadratic row; and
- absorbed bounds are ranked after matrix rows because their original declaration
  position is not carried through provenance.

A total ordering needs either another frozen pass or a source-order rank stamped
from binding through `user_absorbed_bounds` and `ConstraintProvenance`. Do not use
super-increasing weights; that would reintroduce the scaling problem the
lexicographic passes removed.

**Test**: construct equal-budget alternatives for each case and assert both
backends select the same repair set.

## Unbounded-after-fix witness policy

A minimal infeasibility repair can make the constraints feasible while leaving
the restored objective unbounded. Current behavior reports the repair plus an
`edit_source='unbounded_after_fix'` finding.

**Decision needed**: whether that is the final contract, or whether diagnosis
should search for a different finite repair, identify which repair unlocked the
ray, or return multiple equivalent repairs.

## Grouped unreachable bounds omit the failing group

An unreachable bound reduced per group names the clause and qualifier but not the
group that contains the failure. `CollectUnreachableClauses` already emits the
typed `group` field when `ConstraintProvenance::group_label` is set; the label is
lost because a hard MIN/MAX `NO_SOLUTION` row is re-emitted as non-aggregate and
takes a branch that does not stamp it.

**Pointers**: grouped provenance in `src/decidb/utility/ilp_model_builder.cpp` and
`CollectUnreachableClauses` in the diagnostic engine.

**Decision needed**: whether every per-row grouped diagnosis should carry its
group label. This is presentation-only—the reported clause is correct—but it
changes typed output and golden text.

**Test**: a multi-group hard MIN/MAX constraint with an unreachable bound in only
one group, plus `_apply_reported_fix` and golden-output checks.
