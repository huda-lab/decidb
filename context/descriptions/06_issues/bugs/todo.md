# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved entries are removed; if the fix taught a generalizable lesson, record it
in `.claude/lessons.md`.

---

## HiGHS returns the wrong optimum for a convex quadratic objective

**Symptom.** A convex QP solved on HiGHS minimizes *half* the intended quadratic
against the full linear term, so it returns the wrong answer — silently, with no
error and no warning.

**Reproduction.**

```sql
WITH data AS (SELECT 1 AS id, 3.0 AS target UNION ALL SELECT 2, 7.0)
SELECT id, x FROM data DECIDE x(REAL) SUCH THAT x <= 10
MINIMIZE SUM(POWER(x - target, 2));
-- Gurobi: x = 3, 7   (correct: the minimizer of (x-t)^2 is t)
-- HiGHS:  x = 6, 10  (wrong, and exactly 2*target)
```

Force the backend with `DECIDB_FORCE_SOLVER=highs` on a Gurobi-linked host.

**Cause.** The two solvers use different quadratic conventions and DeciDB stores one
of them. Gurobi's `GRBaddqpterms` adds raw `x^T Q x`. HiGHS's `passHessian` takes the
`Q` of `1/2 x^T Q x + c^T x`. `SolverModel` holds Q in Gurobi's convention, and
`deterministic_naive.cpp` passes it through unscaled
(`q_value[pos] = model.q_vals[k]`, in the COO→CSC conversion), so HiGHS optimizes half
the quadratic term against the full linear one. With `Q = 1` and `c = -6` that is
`min 0.5x^2 - 6x`, whose minimizer is 6 rather than 3.

**Ruled out.** Not caused by the plan-time model-class gate. The gate only decides
*whether* a model reaches HiGHS; the COO→CSC Hessian conversion is byte-for-byte
unchanged across that work (`git diff 3a1278796d..HEAD --
src/decidb/naive/deterministic_naive.cpp` touches no line of it). Not re-run against a
pre-gate build, so this is an inspection result rather than a bisect.

**Why it went unseen.** The QP tests run on the unforced fixture, so on a Gurobi-linked
host they only ever take the Gurobi path. Nothing in the suite solves a QP on HiGHS.

**Where to look next.** Pick ONE convention for `SolverModel` and state it in
`ilp_model.hpp`, rather than leaving each backend to guess: either double the Hessian
on the way into HiGHS, or store the 1/2-convention Q and halve it for Gurobi. The test
gap is as much the bug as the code is — the fix has to add a HiGHS-forced QP case whose
optimum is checked against `oracle_solver`, and the QP suite should run on both
backends rather than on whichever the host prefers.

**Found.** While validating the plan-time model-class gate (solver-wrapper work,
batch A′).

