# Code quality

Code-quality issues include duplication, dead code, fragile patterns, unclear
naming, missing coverage, and diagnostic-quality defects that do not change the
ordinary optimization result. Solve-correctness bugs belong in [bugs.md](bugs.md).

Each entry: short title, location (`file:line`), what's wrong, why it matters,
and when/during which task it was discovered.

## CQ-5 — One reach calculator and checked arithmetic for derived numbers

- **Why:** BUG-16 and BUG-18 to BUG-22 share one cause: numbers DeciDB computes from finite data (sums, products,
  quotients, a row's range) are not checked, and "open" is written two ways. Review and probes: 2026-10-29.
- **Design (architect):**
  - `DecideReach { lo, hi, open_var }` replaces `AuxRange` and the seven range walks (BUG-16): an infinite end means an
    open variable and nothing else, so a refusal can always name one; overflow inside it raises TOO_LARGE. Interval
    arithmetic, as in SCIP's and Couenne's bound tightening (FBBT). About +4 / −14 decision points, ~250 → ~80 lines.
  - `DecideFiniteAdd/Mul/Div(a, b, written)` in `common/`, after DuckDB's `TryAddOperator`/`TryMultiplyOperator`:
    inputs finite by contract (D_ASSERT), an infinite result is TOO_LARGE. `AddCoefficient` becomes its Add;
    `MergeLikeAtoms` (BUG-18) and `rms_norm` (BUG-22) use it.
  - The role decides, as `DecideValueRole` does for read values: a coefficient, a Big-M, a box end or a structural
    constant that overflows is TOO_LARGE; a bound DeciDB computes (`k − offset`, `k/a`, `(k − lo)/a`, singleton `r/a`)
    keeps IEEE ±inf, the BUG-17 rule applied to DeciDB's own arithmetic.
  - Direct solve: one TOO_LARGE `DirectValidNumberPredicate` per computed finite-lane column (R1's `t`, `up_fin`,
    `down_fin`, `sterm`, window sums: BUG-19), and S1/A1's computed sums switch to TOO_LARGE (BUG-21). The C++ and SQL
    halves share the role enum and the message; no shared generator.
  - Small causes: `std::ilogb` for the HiGHS scaler's exponents (`deterministic_naive.cpp:110-118`; `int(floor(log2))`
    of a subnormal is UB); delete the unreachable unguarded cast (`physical_decide.cpp:4668`).
- **Decision needed (recommendation first):**
  1. Overflow inside a reach: TOO_LARGE at once (recommended: both solvers agree and the message is true), or round
     outward to an open end and refuse only where a Big-M needs it (keeps Gurobi's answers, needs a blame type).
  2. Like terms merged in SQL (`b + b`): check the addends in SQL and raise TOO_LARGE naming the clause (recommended),
     or merge late in C++ (one column per like term; the direct rules read one merged `a`).
  3. Bounds DeciDB computes keep IEEE infinity (recommended; `1e-10*ABS(y) >= b` then says "infeasible" on both).
  4. Derived numbers at or above the model's infinity: TOO_LARGE from `DECIDE_EFFECTIVE_INFINITY` (recommended, BUG-20),
     with wording that holds below the DOUBLE limit ("computes a number too large to optimize. Scale its data down."),
     or leave them to the large-number warning.
- **Risks:** eager TOO_LARGE turns three Gurobi answers (`MAX(b*y) >= 1`, `MINIMIZE MAX(b*y)`, BUG-16's easy side) into
  errors. Two open ends have no owner today (`linearization_bigm.cpp:112`, an auxiliary missing from the rigid box;
  `linearization_minmax.cpp:267`, the native arm) and need one. The kernel must assert finite inputs, or an unchecked
  inf read is misreported as TOO_LARGE.
- **Not ours:** DIAGNOSE on 1e200 coefficients (Gurobi says "feasible"); Gurobi's "Objective Q not PSD" at 1e308.
