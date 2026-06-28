# Code Quality Issues — Open

Code-quality issues (duplication, dead code, fragile patterns, unclear naming, missing test coverage) spotted opportunistically while working on other tasks. Not bugs — nothing here produces wrong results today; these are things that make the code harder to change safely. Actual bugs go to `../bugs/todo.md`.

Each entry: short title, location (`file:line`), what's wrong, why it matters, and when/during which task it was discovered.

Resolved entries are removed; if the fix taught a generalizable lesson, record it in `.claude/lessons.md`.

---

## `DiagnoseModeWantsUnboundedRay` doubles as the generic "diagnosis armed" signal

**Location:** `physical_decide.cpp` (Finalize, the solve site) + `decide_diagnostic.hpp` / `.cpp` (the predicate).

**What's wrong:** The predicate is named for unbounded-ray extraction, but it really answers "is diagnosis armed?" (true under `auto`). As of I0 it gates two things at the solve site — `solve_options.extract_unbounded_ray` *and* whether the built `SolverModel` is retained for the infeasible engine (`diagnosis_armed ? &retained_model : nullptr`). Reading `diagnosis_armed = DiagnoseModeWantsUnboundedRay(mode)` and then using it to retain a model for an *infeasible* solve is mildly confusing.

**Why it matters:** Each new failure terminal that pre-arms work (I1 infeasible elastic re-solve, the slow engine) will gate on the same "armed" condition, so a ray-named predicate keeps accreting unrelated consumers. A small rename/generalization (e.g. `DiagnoseModeArmsDiagnosis`, with the ray-specific need derived from it) would keep intent clear.

**Discovered:** 2026-06-27, during I0 (infeasible engine seam).

---

## Constructor BOOLEAN-domain check is dead — the type is already INTEGER

**Location:** `physical_decide.cpp:~1276` (`DecideGlobalSinkState` constructor).

**What's wrong:** The constructor sets `absorbed_upper_bounds[var] = 1.0` when
`op.decide_variables[var]->return_type == LogicalType::BOOLEAN`, but a `BOOLEAN` decide
variable is lowered to an INTEGER with synthesized `x >= 0` / `x <= 1` domain constraints
*before* this point — so `return_type` is `INTEGER` and the branch never fires (confirmed by
debug during I2.a: the absorption site reports `type=INTEGER` for a `DECIDE x IS BOOLEAN`
var). The 0/1 upper is instead enforced by the synthesized `x <= 1` constraint via
`TraverseBoundsConstraints`.

**Why it matters:** Dead code that *looks* load-bearing — a reader (or a future change) may
trust it to cap BOOLEAN domains when it does nothing. The reliable BOOLEAN signal at this
layer is `LogicalDecide::is_boolean_var` (threaded to `PhysicalDecide` in I2.a), not the bound
type. Either remove the branch or switch it to `op.is_boolean_var[var]` if the explicit cap is
still wanted as defense-in-depth.

**Discovered:** 2026-06-28, during I2.a (resolving the absorbed-bound / BOOLEAN-domain blocker).

---

## Reversed simple bounds are rejected instead of normalized

**Location:** `decide_constraints_binder.cpp:~482` (LHS-shape validation) and
`physical_decide.cpp:~1988` (`TraverseBoundsConstraints` simple-bound matcher).

**What's wrong:** Equivalent reversed bounds like `5 >= x` are rejected by the DECIDE
constraint binder because the left-hand side is not a DECIDE variable or aggregate. The
canonical spelling `x <= 5` is accepted and later absorbed into a compact column bound.

**Why it matters:** This is a SQL-equivalent shape users can reasonably write, and it is easy
to normalize by flipping the comparator before the physical absorption pass (`5 >= x` →
`x <= 5`). Until then, users must write variable-on-left bounds to get accepted syntax and the
column-bound/diagnostic path.

**Discovered:** 2026-06-28, during review of variable-bound handling after I2.
