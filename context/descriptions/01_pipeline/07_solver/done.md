# Stage 07 — Solver facade and backends

Translates a solver-neutral `SolverModel` into a backend's own API, runs it, and
normalizes the outcome. It never inspects SQL plans or DECIDE query semantics.

**Key source files**

- `src/decidb/utility/ilp_solver.cpp` — selection, `SolveModel`, disambiguation, ray attachment
- `src/decidb/utility/solver_registry.cpp` — the backend table
- `src/decidb/gurobi/gurobi_solver.cpp`, `src/decidb/gurobi/gurobi_loader.cpp` — Gurobi (C API)
- `src/decidb/naive/deterministic_naive.cpp` — HiGHS (C++ API)
- `src/include/duckdb/decidb/solver_registry.hpp` — the backend handle and the registry
- `src/include/duckdb/decidb/solver_capabilities.hpp` — what upstream stages may assume
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

## 2. The registry, and choosing a backend

### The table

`SolverRegistry` (`solver_registry.hpp`) is the one place a backend is named. Each
entry is a `SolverBackendInfo` row: the identifier, a runtime availability probe, a
capability accessor, and a session factory. Adding a backend means appending a row —
it changes **zero** `if` and `switch` statements anywhere else in the tree.

`SolverBackend` is the handle the rest of the pipeline passes around: a value type
wrapping a pointer into that table, so it copies freely, compares by identity, and
rides a plan node from stage 05 to stage 08. A default-constructed handle means "no
backend chosen yet".

Row order **is** preference order. Gurobi first — empirically much faster on DeciDB
workloads and strictly more capable; HiGHS last, and unconditionally available, which
is what makes selection total.

### Capabilities

`SolverCapabilities` (`solver_capabilities.hpp`) declares the backend differences an
**upstream** stage has to branch on. That is the membership rule: a difference only
the backend itself acts on stays a virtual on `SolverSession` with a safe default —
`SetInterruptPoll` is the reference case.

The flags split in two, and the split decides what "unsupported" means:

| Kind | Flags | `false` means |
|---|---|---|
| Construct | `abs`, `min_max`, `not_equal`, `in_list`, `bilinear` | A lowering always exists, so stage 05 lowers as it always has. An optimization; the lowering path is never deleted. |
| Model class | `quadratic_constraints`, `nonconvex_quadratic`, `miqp` | No lowering exists. A gate: the answer is refusal, at plan time, blaming the host rather than the query. |

Capability is asked through a function, not read off a constant, because it is partly
a **runtime** fact — a dynamically loaded library may not export the symbol a native
construct needs, so the answer is not known until the library is open.

A flag is only worth a field if it is A/B-verifiable: forcing the construct back down
its lowering path must reach the same optimum. `DECIDB_NATIVE_CONSTRUCTS=off` is how
that is checked — a test-only switch, mirroring `DECIDB_FORCE_SOLVER`, that turns every
construct capability off so both arms run on one machine.

Construct flags declared today:

| Flag | Backend | Symbol behind it |
|---|---|---|
| `abs` | Gurobi | `GRBaddgenconstrAbs` |

The rest stay false until the loader binds their symbols and stage 08 knows how to emit
them: a capability may not be declared ahead of the code that honors it.

### Selection

`SelectSolverBackend()`:

1. `DECIDB_FORCE_SOLVER=<registered name>` pins the backend, matched
   case-insensitively. This is a **test-only** override used by
   `test/decide/conftest.py` (`decidb_cli_highs` / `decidb_cli_gurobi`) to exercise
   both backends on one host. Forcing a backend that is unavailable throws; a name no
   backend answers to falls through.
2. Otherwise, the first available entry in registry order.

Selection is **not** cost-based, and it does not inspect the model.

It runs **once per query, at plan time** — `DecideOptimizer::OptimizeDecide` calls it
before any rewrite — and the answer rides the plan on
`LogicalDecide::solver_backend` → `PhysicalDecide::solver_backend` → `SolveModel` →
every diagnostic re-solve. Nothing asks a second time. The reason is not tidiness:
once a rewrite has consulted the backend's capabilities, a second selection that
answered differently would run a model on a solver it was not built for. See
[`../05_optimizer/done.md`](../05_optimizer/done.md) §0.

The choice is not serialized with the plan — which solver a host has is a property of
the host, not of the query.

`GurobiSolver::IsAvailable()` is a one-time lazy check (static local with lambda init)
that dlopens the library and attempts to start an environment. It is purely a runtime
probe: there is no compile-time flag, and DeciDB links without a Gurobi installation.

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

`SolveModel(input, indexer, backend, options, retained_model, retained_session)` is
the facade the operator calls. `backend` is passed in, not resolved here, so the
solver that runs the model is provably the one the rewrites were selected against. In
order:

1. `SolverModel::Build(input, indexer)`. A `DecideInfeasibleModelException` here
   means infeasibility was proven by a throw that abandoned the half-built model —
   return `INFEASIBLE` with no model, and diagnosis cannot run. Only the
   conflicting-column-bounds check still exits this way, and only with diagnosis
   off. Because no model comes back, the operator must treat an unretained model
   (`num_vars == 0`) as "no diagnosis available" rather than walk it.
2. Check `model.ModelClass()` against `backend.Capabilities()`. Stage 05 already
   refused this query if the backend could not take it; this re-reads the class off
   the model *as built* and asserts the two agree. Layer 8 does not repair, it
   checks — a mismatch is an `InternalException`, never a user error.
3. `DumpSolverModel(model)` — a no-op unless `DECIDB_DUMP_MODEL` is set. Emitted
   on the freshly built model so diagnostic re-solves never pollute the dump. This
   is the golden corpus's characterization oracle.
4. Record `model_constraint_rows` before anything moves the model.
5. `model.build_proven_infeasible`: a constraint reduced to a coefficient-free row
   against a bound it cannot meet (`SUM(0 * x) <= -1`, `x - x <= -1`). `Build`
   keeps that row — with its source provenance — instead of throwing, so the model
   survives for the elastic engine to name and relax the clause. No backend is
   asked to load a row with no coefficients: this short-circuits to `INFEASIBLE`,
   retaining the model. Unconditional, unlike the box below; with diagnosis off the
   retained model is simply discarded.
6. Under `tolerate_infeasible_bounds` (diagnosis mode), `Build` keeps an inverted
   column box rather than throwing, so the model survives for the elastic engine. An
   inverted box is not a hard model, it is an empty one — the answer is already known,
   and no solver contract says a backend must accept it — so this short-circuits to
   `INFEASIBLE`, retaining the model, without handing the box to any backend. (HiGHS
   is the concrete case: it errors hard on load and poisons the session.)
7. Solve on a live session.
8. `DisambiguateInfOrUnbd`.
9. `AttachUnboundedRayIfRequested`.
10. Hand back the session, then the model, to whoever asked.

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

Declares every model-class capability — `quadratic_constraints`,
`nonconvex_quadratic`, `miqp` — so no query is refused for the shape of its model.
Its construct flags are turned on one at a time, each together with the loader symbol
that backs it: `caps.abs = api.addgenconstrAbs != nullptr`, so a flag is never true on
a host whose library did not export it.

**General constraints** (`GRBaddgenconstrAbs` and its siblings) are bound
`nullptr`-gated, exactly as `terminate` is. They are what makes a construct capability
worth having: `aux = |t|` stated directly needs no Big-M, so a query whose contributors
have no finite bound — which the lowering path must refuse — simply answers.

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

The floor of the registry: every capability flag is false. No construct is native, so
everything arrives fully lowered; no model class beyond plain linear and **convex**
quadratic objectives is accepted, so the three refusals below are gates rather than
slow paths. It is also unconditionally available, which is what makes selection total.

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

**Model classes it cannot load** — quadratic constraints, a non-convex objective,
and MIQP — are declared `false` in its capability table and refused at **plan time**
(`RequireDecideSolverSupport`, stage 05), before the query reads a row. They used to
throw here, at model load, after a full scan and build; the backend now contains no
model-class check at all. `SolveModel` re-derives the class from the built model and
asserts the backend covers it, so nothing unsupported can reach `passModel`.

HiGHS has no thread-safe terminate, so it never sets `user_interrupted`.

---

## 6. Adding a backend

1. Implement `SolverSession` with `Solve(model, time_limit)` and
   `Continue(time_limit)`, both returning a normalized `SolverResult`. Map the
   backend's terminal statuses onto `SolverStatus`; anything unmapped goes to `OTHER`
   with `raw_status` set. Override a `SolverSession` virtual only for behavior the
   backend alone acts on.
2. Add `IsAvailable()`, `Capabilities()`, and `CreateSession()` statics.
3. Append one `SolverBackendInfo` row to `REGISTERED_BACKENDS` in
   `solver_registry.cpp`, positioned by preference.

That is the whole list, and step 3 is the only edit outside the new backend's own
files. If a fourth step ever appears — an `if` on the backend's name, a `switch` on a
new enum — the difference it is branching on belongs in `SolverCapabilities` instead.

`SolverModel` provides everything needed: variable bounds and types, linear and
quadratic objective, and constraints in COO. No change to extraction, coefficient
evaluation, or model building is required.

---

## 7. Source map

| Concern | Location |
|---|---|
| Backend selection, `SolveModel`, disambiguation | `src/decidb/utility/ilp_solver.cpp` |
| Backend table | `src/decidb/utility/solver_registry.cpp`, `src/include/duckdb/decidb/solver_registry.hpp` |
| Capability declarations | `src/include/duckdb/decidb/solver_capabilities.hpp` |
| Normalized outcome and default error text | `src/include/duckdb/decidb/solver_result.hpp` |
| Session contract | `src/include/duckdb/decidb/solver_session.hpp` |
| Time limits | `src/include/duckdb/decidb/solver_config.hpp` |
| Gurobi backend | `src/decidb/gurobi/gurobi_solver.cpp` |
| Gurobi dynamic loading | `src/decidb/gurobi/gurobi_loader.cpp` |
| HiGHS backend | `src/decidb/naive/deterministic_naive.cpp` |
