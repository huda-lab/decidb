# Stage 07 — Solver facade and backends

Translates a solver-neutral `SolverModel` into a backend's own API, runs it, and
normalizes the outcome. It never inspects SQL plans or DECIDE query semantics.

**Key source files**

- `src/decidb/utility/ilp_solver.cpp` — dispatch, disambiguation, ray attachment
- `src/decidb/gurobi/gurobi_solver.cpp`, `src/decidb/gurobi/gurobi_loader.cpp` — Gurobi (C API)
- `src/decidb/naive/deterministic_naive.cpp` — HiGHS (C++ API)
- `src/include/duckdb/decidb/solver_result.hpp` — the normalized outcome
- `src/include/duckdb/decidb/solver_session.hpp` — the warm-continuation handle
- `src/include/duckdb/decidb/solver_config.hpp` — time limits

Both backends must remain valid implementations. A Gurobi-only API is an
*accelerator*, never a dependency.

---

## 1. Outcome is a value, not an exception

`SolverResult` (`solver_result.hpp`) is what every solve returns. Callers branch on
`status`; they do not catch an exception to learn the outcome.

```cpp
enum class SolverStatus {
    OPTIMAL, INFEASIBLE, UNBOUNDED, INF_OR_UNBD,
    TIME_LIMIT, SUBOPTIMAL, ITERATION_LIMIT, OTHER
};
```

`solution` is populated at `OPTIMAL` (proven), at `TIME_LIMIT` when the backend
found a feasible incumbent, and at `SUBOPTIMAL` (feasible but unproven). **A
caller must branch on `status` / `has_solution`, never on emptiness alone**, to
know whether a value is proven optimal.

The trailing fields carry what a report needs: `objective_value`, `best_bound`,
`gap`, `user_interrupted`, `raw_status`, `diagnostic_timed_out`, `ray`, and
`model_constraint_rows` (the post-expansion row count actually handed to the
backend — the only figure comparable across queries).

`best_bound` and `gap` are `NaN` when no bound exists: LP/QP timeouts, a failed
attribute read, or a solver infinity sentinel. Report writers must skip them when
`!std::isfinite`.

`ThrowDecideSolveError` is the single home for the user-facing failure text. The
operator calls it only on an **undiagnosed** terminal — `diagnose_decide` is off,
or the status has no engine (`ITERATION_LIMIT` / `OTHER`). Every diagnosed failure
throws the `decide_diagnostics()` pointer error instead. See
[`../../07_query_diagnostics/`](../../07_query_diagnostics/).

---

## 2. Dispatch

`SelectSolverBackend()`:

1. `DECIDB_FORCE_SOLVER=highs|gurobi` pins the backend. This is a **test-only**
   override used by `test/decide/conftest.py` (`decidb_cli_highs` /
   `decidb_cli_gurobi`) to exercise both backends on one host. Forcing `gurobi`
   where it is unavailable throws; unknown values fall through.
2. Otherwise, Gurobi if `GurobiSolver::IsAvailable()`, else HiGHS.

Selection is **not** cost-based. Gurobi is always preferred — empirically much
faster on DeciDB workloads — and HiGHS is the always-available fallback.

`GurobiSolver::IsAvailable()` is a one-time lazy check (static local with lambda
init) that attempts `GRBloadenv()`. Without `DECIDB_HAS_GUROBI` at compile time it
always returns false.

### Sessions

A `SolverSession` is a live solver handle. `Solve(model, time_limit)` loads the
model once and runs the first chunk; `Continue(time_limit)` resumes **that same
warm solver** for another chunk without reloading. Both return a normalized
`SolverResult`. `Continue` has a precondition that `Solve` ran first.

This is what lets a `TIME_LIMIT` stop be extended: `SolveModel` hands the live
session back through `retained_session` so the continuation loop can resume it.
An interrupt poll installed before the first solve carries into every later
`Continue`, so a user Ctrl-C cuts the *initial* solve short too, not only
continuation chunks.

### Time limits

`solver_config.hpp` is the one place a limit is read, which makes it
solver-agnostic by construction. `ResolveDecideTimeLimit()` returns the default
unless `DECIDB_TIME_LIMIT` (seconds, double) overrides it; non-positive or
unparseable values are ignored. It is applied to **both** backends — Gurobi
`TimeLimit`, HiGHS `time_limit`.

Diagnostic follow-up solves are budgeted separately
(`ResolveDecideDiagnosticTimeLimit`), capped at the smaller of the primary budget
and the diagnostic cap, so a failed solve is not followed by several full-length
internal re-solves before the user sees anything.

---

## 3. `SolveModel`

`SolveModel(input, indexer, options, retained_model, retained_session)` is the
facade the operator calls. In order:

1. `SolverModel::Build(input, indexer)`. A `DecideInfeasibleModelException` here
   means infeasibility was proven during build — return `INFEASIBLE` with no
   model.
2. `DumpSolverModel(model)` — a no-op unless `DECIDB_DUMP_MODEL` is set. Emitted
   on the freshly built model so diagnostic re-solves never pollute the dump. This
   is the golden corpus's characterization oracle.
3. Record `model_constraint_rows` before anything moves the model.
4. Under `tolerate_infeasible_bounds` (diagnosis mode), `Build` keeps an inverted
   column box rather than throwing, so the model survives for the elastic engine.
   That box *is* infeasible and some backends reject it at load — HiGHS errors
   hard and poisons the session — so this short-circuits to `INFEASIBLE`,
   retaining the model, without ever handing the inverted box to a solver.
5. Solve on a live session.
6. `DisambiguateInfOrUnbd`.
7. `AttachUnboundedRayIfRequested`.
8. Hand back the session, then the model, to whoever asked.

### Disambiguating INF_OR_UNBD

Gurobi's `GRB_INF_OR_UNBD` and HiGHS's `kUnboundedOrInfeasible` (status 9) both
normalize to `INF_OR_UNBD`. `DisambiguateInfOrUnbd` re-solves a zero-objective
probe model on a diagnostic budget: `OPTIMAL` means the feasible region is
non-empty, so the original was `UNBOUNDED`; `INFEASIBLE` means it was
`INFEASIBLE`. If the probe hits its own budget, `diagnostic_timed_out` is set and
the ambiguous status stands — so the operator can say diagnosis ran out of time
rather than fall back to a misleading static error.

Gurobi additionally applies its documented recipe first (set `DualReductions=0` on
the model's env and re-optimize), since presolve dual reductions are what blur the
two.

---

## 4. Gurobi backend

Uses the **C API** (`gurobi_c.h`), not the C++ wrapper, and loads it dynamically
through `gurobi_loader.cpp` so DeciDB links without a Gurobi installation.

An RAII `GurobiGuard` owns `GRBmodel*` / `GRBenv*` and frees both on every exit
path, exceptions included.

| Step | Call |
|---|---|
| Environment | `GRBloadenv()`, `OutputFlag = 0` |
| Variables | one `GRBnewmodel()` with type/bound/objective arrays — `GRB_BINARY`, `GRB_INTEGER`, `GRB_CONTINUOUS` |
| Sense | `GRBsetintattr(GRB_INT_ATTR_MODELSENSE, ...)` |
| Constraints | `GRBaddconstr()` per row, taking COO directly |
| Quadratic objective | `GRBaddqpterms()` (COO). Gurobi supports continuous QP **and** MIQP |

Solution values come from `GRBgetdblattrarray(GRB_DBL_ATTR_X, ...)` and each is
validated finite. Incumbent reads (`solution`, `objective_value`, `gap`) are gated
on `SolCount > 0`, because with no incumbent those attributes return solver
sentinels (`-1e100` / `inf` / `nan`).

`GRB_INTERRUPTED` reuses the `TIME_LIMIT` terminal with `user_interrupted` set;
it changes report wording ("you stopped it" vs "hit the time limit"), not routing.

---

## 5. HiGHS backend

Uses the **C++ API** (`Highs.h`). The class is named `DeterministicNaive` for
historical reasons; it is a full MIP solver.

HiGHS has no binary variable type — a binary is an integer with bounds `[0,1]`.

Sense characters convert to HiGHS's range rows:

| Sense | `row_lower` | `row_upper` |
|---|---|---|
| `'>'` | `rhs` | `1e30` |
| `'<'` | `-1e30` | `rhs` |
| `'='` | `rhs` | `rhs` |

`SolverModel` stores the matrix in COO; HiGHS needs CSR (`MatrixFormat::kRowwise`).
The conversion counts non-zeros per row, prefix-sums to row starts, then scatters
COO entries using a `current_pos` tracker. A quadratic objective goes through
`passHessian()`, which wants CSC for the lower triangle — same three-step
conversion by column.

Logging is off (`log_to_console = false`). The session sets `time_limit` per chunk
rather than once at load, which is what makes `Continue()` work: HiGHS resumes its
MIP search on a repeat `run()`.

**MIQP limitation**: HiGHS does not support mixed-integer quadratic programs. If
any variable is integer or boolean and the objective is quadratic, an
`InvalidInputException` directs the user to install Gurobi or use `REAL`
variables. HiGHS also has no thread-safe terminate, so it never sets
`user_interrupted`.

---

## 6. Adding a backend

1. Implement `SolverSession` with `Solve(model, time_limit)` and
   `Continue(time_limit)`, both returning a normalized `SolverResult`.
2. Optionally add a static `IsAvailable()` for runtime detection.
3. Add a `SolverBackend` enum value and a `CreateSolverSession` case.
4. Map the backend's terminal statuses onto `SolverStatus`. Anything unmapped goes
   to `OTHER` with `raw_status` set.

`SolverModel` provides everything needed: variable bounds and types, linear and
quadratic objective, and constraints in COO. No change to extraction, coefficient
evaluation, or model building is required.

---

## 7. Source map

| Concern | Location |
|---|---|
| Backend selection, `SolveModel`, disambiguation | `src/decidb/utility/ilp_solver.cpp` |
| Normalized outcome and default error text | `src/include/duckdb/decidb/solver_result.hpp` |
| Session contract | `src/include/duckdb/decidb/solver_session.hpp` |
| Time limits | `src/include/duckdb/decidb/solver_config.hpp` |
| Gurobi backend | `src/decidb/gurobi/gurobi_solver.cpp` |
| Gurobi dynamic loading | `src/decidb/gurobi/gurobi_loader.cpp` |
| HiGHS backend | `src/decidb/naive/deterministic_naive.cpp` |
