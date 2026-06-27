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
