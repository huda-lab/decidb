# Stage 07 — Solver facade and backends

Translates a solver-neutral `SolverModel` into a backend's own API, runs it, and
normalizes the outcome. It never inspects SQL plans or DECIDE query semantics.

**Key source files**

- `src/decidb/solver/ilp_solver.cpp` — selection, `SolveModel`, disambiguation, ray attachment
- `src/decidb/solver/solver_registry.cpp` — the backend table
- `src/decidb/gurobi/gurobi_solver.cpp`, `src/decidb/gurobi/gurobi_loader.cpp` — Gurobi (C API)
- `src/decidb/naive/deterministic_naive.cpp` — HiGHS (C++ API)
- `src/include/duckdb/decidb/solver/solver_registry.hpp` — the backend handle and the registry
- `src/include/duckdb/common/decide_solver_capabilities.hpp` — what upstream stages may assume
- `src/include/duckdb/decidb/solver/solver_result.hpp` — the normalized outcome
- `src/include/duckdb/decidb/solver/solver_session.hpp` — the warm-continuation handle
- `src/include/duckdb/decidb/solver/solver_config.hpp` — time limits

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
operator calls it only on an **undiagnosed** terminal — the statement carried no
`DIAGNOSE` prefix, or the status has no engine (`ITERATION_LIMIT` / `OTHER`). Its text
names the state and points at the prefix; a diagnosed failure raises nothing at all and
returns its findings as rows. See
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

`common/decide_solver_capabilities.hpp` declares the backend differences an **upstream** stage has to
branch on. That is the membership rule: a difference only the backend itself acts on
stays a virtual on `SolverSession` with a safe default — `SetInterruptPoll` is the
reference case.

There are exactly two kinds of difference and they mean opposite things when the answer
is "no", so they are two **types**, not two halves of one struct:

| Type | Fields | `false` means |
|---|---|---|
| `SolverConstructSupport` | `abs`, `min_max`, `not_equal`, `bilinear` | A lowering always exists, so stage 05 lowers as it always has. An optimization; the lowering path is never deleted. |
| `SolverModelClass` | `quadratic_constraints`, `nonconvex_quadratic`, `miqp`, `singular_quadratic` | No lowering exists. A gate: the answer is refusal, at plan time, blaming the host rather than the query. |

Splitting them is what lets each predicate take exactly the table it reads.
`FindModelClassGap(needed, supported)` now takes `SolverModelClass` on **both** sides —
what a query demands against what a backend accepts — so it is a plain containment test
with nothing to ignore. `SolverCapabilities` survives only as the pair
(`.constructs`, `.model_classes`) the registry asks each backend for in one call; no
call site below it reads both members.

Capability is asked through a function, not read off a constant, because it is partly
a **runtime** fact — a dynamically loaded library may not export the symbol a native
construct needs, so the answer is not known until the library is open. Gurobi's
`Capabilities()` therefore calls `GurobiLoader::Load()` **before** reading the symbol
table: the answer is cached for the process, so reading it while the library is still
closed would be a permanent false negative.

A flag is only worth a field if it is A/B-verifiable: forcing the construct back down
its lowering path must reach the same optimum. `DECIDB_NATIVE_CONSTRUCTS=off` is how
that is checked — a test-only switch, mirroring `DECIDB_FORCE_SOLVER`, that turns every
construct capability off so both arms run on one machine. It is applied **centrally**,
in `SolverBackend::Capabilities()`, which is why that accessor returns by value: the
switch is part of the capability contract, so every registered backend inherits it
instead of copy-pasting it. Model classes are never masked — a gate has no fallback
path to force a query onto, so masking one would refuse the query rather than slow it.

Construct flags declared today:

| Flag | Backend | Symbol behind it |
|---|---|---|
| `abs` | Gurobi | `GRBaddgenconstrAbs` |
| `min_max` | Gurobi | `GRBaddgenconstrMin` + `GRBaddgenconstrMax` |
| `not_equal` | Gurobi | `GRBaddgenconstrIndicator` |

`bilinear` stays false until the loader binds its symbols and stage 08 knows how to emit
it: a capability may not be declared ahead of the code that honors it.

There is deliberately **no** `in_list` field. SOS1 acceleration for `x IN (a, b, c)` was
measured and declined — the formulation's LP relaxation is already integral, so there is
nothing to branch on. Across the measured domain sizes and row counts, the declaration
was redundant work at the root and produced no meaningful win. A measured-and-rejected
capability is not a field: a permanently-false flag nobody reads cannot be told apart
from one whose implementation is merely still pending.

Native ABS remains enabled whenever the backend declares it. A 2026-08-24 Gurobi
comparison found the native arm about 7% slower at 500K rows because Gurobi's presolve
expanded it into a larger formulation, but routing around a vendor-specific presolve
choice was declined: the native model is the smaller, direct statement and supports
unbounded contributors that the Big-M lowering must refuse. Revisit only with new solver
evidence.

Which construct flag gates a given `GeneralConstraintKind` is one table,
`DeclaresGeneralConstraint`, kept beside that enum in `solver_input.hpp` — so a kind
added without a flag reads as undeclared and trips a loud internal error. That table is
the check that a general constraint reaching a backend was declared by it, not the
routing table for the capability set as a whole: a site that decides to go native is
rewriting one known construct and reads its flag directly, and `not_equal` and
`bilinear` have no `GeneralConstraintKind` to look up.

### Selection

`SelectSolverBackend()`:

1. `DECIDB_FORCE_SOLVER=<registered name>` pins the backend, matched
   case-insensitively. This is a **test-only** override used by
   `test/decide/conftest.py` (`decidb_cli_highs` / `decidb_cli_gurobi`) to exercise
   both backends on one host. Forcing a backend that is unavailable throws, and so does
   a name no backend answers to — the error lists the valid names. An unrecognized name
   is refused rather than ignored: anything that pins the backend is asking for one
   specific solver, so falling through would run the host default under a name
   promising otherwise, and a typo in a fixture would look like a passing test of a
   backend that never ran.
2. Otherwise, the first available entry in registry order.

Selection is **not** cost-based, and it does not inspect the model.

It runs **once per query, at plan time** — `ChooseDecideSolver`, called first thing
in `DecideOptimizer::OptimizeDecide` — and the answer rides the plan as a **name**:
`LogicalDecide::solver_backend_name` → `PhysicalDecide::solver_backend_name` →
`PhysicalDecide::PlannedSolverBackend()` → `SolveModel` → every diagnostic re-solve.
Nothing asks a second time. The reason is not tidiness: once a rewrite has consulted
the backend's capabilities, a second selection that answered differently would run a
model on a solver it was not built for. See
[`../05_optimizer/done.md`](../05_optimizer/done.md) §0.

A name and not a handle, because a `SolverBackend` can open a session and the plan
has no business doing that. `SolverRegistry::Find` turns it back into one at the two
points that are about to solve — the primary solve and the diagnostic re-solves —
which is stage 07's side of the boundary. The other thing stage 05 reads off the
backend, `SolverConstructSupport`, is copied onto the plan by value, so no stage
below stage 05 needs the backend to know what was lowered.

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
2. `AssertBackendAcceptsBuiltModel(model, backend)` — both halves of the capability
   contract, re-read off the model *as built*. The model class is checked against
   `Capabilities().model_classes` (stage 05 already refused the query if the backend
   could not take it, so this asserts the prediction matched the fact), and every
   native construct against `Capabilities().constructs` — which stage 05 read when it
   chose the formulation, so a mismatch here means the plan and the solve disagree about
   which backend is in play. Layer 8 does not repair, it checks — a mismatch is an
   `InternalException`, never a user error.
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

### Rejecting an optimum found against the integer ceiling

`DisambiguateInfOrUnbd` only helps when the backend admits it is unsure. One case
gives no such signal: **no mixed-integer solver can represent an unbounded integer.**
Both cap an integer column near ±2e9 and then report the answer against that cap as a
confident `OPTIMAL`.

This is invisible on the normal path, because a query that leaves an integer column
open is refused earlier — the Big-M lowering needs a finite box and says so (§5). But a
backend that expresses a construct **natively** needs no Big-M, so it never meets that
refusal. A native `<>` over an open column was the concrete case: `SUM(ship) <> 500`
with `MAXIMIZE SUM(ship)` came back as `ship = 2147483647`.

`RejectIntegerCeilingOptimum` closes it. An `OPTIMAL` whose value sits at
`INTEGER_SOLVER_CEILING` on a column **the query left open** is not an answer, and is
reported as `UNBOUNDED`. Two things keep it narrow:

- It keys on the returned **value**, not on the box being open. The same open box under
  `MINIMIZE` has a real optimum of 0, and a backend that can express `<>` natively still
  answers it. That capability is the point — refusing there would give up something
  HiGHS cannot do anyway.
- A bound the **user** wrote is answered against, however large. `x <= 2000000000`
  returns 2000000000.

It also hands over its own ray. The pinned column *is* the escaping direction, and
`BuildUnboundedRayFallbackModel` cannot derive one through an indicator row — so
without this the diagnosis would fall back to "a non-linear term prevents naming the
variable" while the guard was holding the variable's name.

Deliberately in the shared facade rather than in an adapter: the limitation is
arithmetic, not vendor behaviour, and a new backend inherits the guard for free.

Tests: `test/decide/tests/test_unbounded_bigm.py` — the native-path trio.

---

## 4. Gurobi backend

Declares every model class — `quadratic_constraints`, `nonconvex_quadratic`, `miqp`,
`singular_quadratic` — so no query is refused for the shape of its model. Its construct
flags are turned on one at a time, each together with the loader symbol that backs it:
`caps.constructs.abs = api.addgenconstrAbs != nullptr`, so a flag is never true on a
host whose library did not export it. The declaration opens the library first
(`GurobiLoader::Load()`) and is then cached for the process.

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
conversion by column, plus a rescale.

That rescale is the one place HiGHS and Gurobi genuinely disagree. `SolverModel`
stores Q as the plain coefficient of each monomial, which is what
`GRBaddqpterms` takes, so the Gurobi adapter passes Q through untouched.
`passHessian()` instead takes the Q of `(1/2) xᵀQx`, and applies that 1/2 to the
triangle it is given: `HighsHessian::objectiveValue` sums `0.5·qᵢᵢ·xᵢ²` on the
diagonal but a full `qᵢⱼ·xᵢxⱼ` off it. The 1/2 is there to undo the
double-counting of an off-diagonal pair, which sits at both `(i,j)` and `(j,i)`
of the symmetric matrix; a diagonal entry is never mirrored, so nothing cancels
its 1/2. **The HiGHS adapter therefore doubles diagonal entries and leaves
off-diagonal entries alone.** Scaling the whole triangle instead does not merely
change the objective's units — it inflates cross terms relative to squares and
turns a PSD Q indefinite. Getting this wrong is silent: the solver returns a
confident answer to a different problem.

### The coefficient window

HiGHS enforces a magnitude window on every constraint-matrix entry inside `passModel`,
at both ends and with different consequences. An entry at or below `small_matrix_value`
(1e-9) is **deleted from the matrix** and `passModel` returns `kWarning`; an entry at or
above `large_matrix_value` (1e15) makes it return `kError`. Gurobi has no such window.

Both edges are reachable from ordinary SQL, and neither is a malformed query. A per-unit
rate of 1e-9 is data. The heavy end is reached by coefficients DeciDB generates itself —
the Big-M closing `<>`, `MIN` and `MAX` is the decision's own span, so
`x(REAL) SUCH THAT x <= 4e15 AND MAX(x) >= 3` puts a 4e15 entry in the matrix while every
number the user typed is in range. The infeasible engine reaches the low end the same way:
its scale-normalized tier-1 weights are `ref / rms(Aᵢ)`, so a row with 1e9-scale
coefficients earns a 1e-9 weight, and that weight becomes a matrix entry in the stage-2
budget row.

**Each out-of-window row is therefore multiplied through by its own power of two, bounds
included, before the matrix is packed into CSR.** Scaling a row by a positive constant is
the one rewrite that leaves a constraint's meaning exactly intact — `1e-9x₁ + 1e-9x₂ ≥ 1`
and `x₁ + x₂ ≥ 1e9` have identical solution sets — so it cannot change an answer. Rounding
a sub-tolerance entry to zero, which is what HiGHS itself does, can: drop both
coefficients of that same row and it becomes `0 ≥ 1`, turning a feasible query infeasible.
The factor is a power of two so that `ldexp` is exact in binary floating point and
introduces no drift; both backends still solve bit-identical constraints. HiGHS's own
internal scaling picks powers of two for the same reason.

A row already inside the window is left exactly as it is rather than re-centred, so this
changes nothing about the models that loaded correctly before it existed. An
out-of-window row is centred in the window rather than shifted just far enough to clear
the edge — a coefficient sitting a hair above the drop threshold is inside the window but
still badly conditioned.

A row's bound has a window of its own, and the same scaling serves it. HiGHS reads
|bound| >= `infinite_bound` (1e20) as ±infinity, which on one side of a row is an error
and on the other silently drops the constraint. So a row is scaled when *either* its
coefficients or its finite bound is out of range — `SUM(x) >= 1e25` against `x <= 5` is
scaled down until HiGHS can hold the limit, and answers INFEASIBLE like Gurobi instead of
dying at `passModel` — and the scaling is capped so a bound that was representable stays
representable. A rescue that deletes a row would be the same silent model change the
dropped coefficient was.

Two rows cannot be placed inside the window at all, and both are refused in SQL terms
naming the clause as the user wrote it, rather than reaching HiGHS: one whose own
coefficients span more than the window's ~24 orders of magnitude, and one whose bound
would have to cross `infinite_bound` to get the coefficients in. Scaling gets an extreme
row *loaded*; it does not make it well-conditioned. A Big-M of 4e15 is numerically
untrustworthy on any solver, which is a separate concern from whether the API accepts it.

The bound is read from the clause's own `rhs`, never inferred from the packed range row.
The sense decides which side of `row_lower` / `row_upper` carries the limit, and the open
side holds HiGHS's ±1e30 "no bound here" sentinel — so a user limit of 1e40, or of 1e30
exactly, cannot be told from that sentinel by magnitude. Reading it from the clause is
what keeps a real limit from being mistaken for "no bound" and passed through to the
internal error this exists to prevent.

`passModel` and `passHessian` therefore throw only on `kError`, never on `kWarning` — the
same split `RunAndReadback` makes at `run()`. `kWarning` is HiGHS reporting a condition it
expects the caller to proceed through: a dropped sub-tolerance entry, or an inconsistent
bound pair left in place so the solve can deduce infeasibility from it. Treating those as
fatal turned an ordinary query into an `InternalException` with a stack trace, and made a
query's success depend on which solver happened to be installed.

Logging is off (`log_to_console = false`). The session sets `time_limit` per chunk
rather than once at load, which is what makes `Continue()` work: HiGHS resumes its
MIP search on a repeat `run()`.

**Model classes it cannot take** — quadratic constraints, a non-convex objective,
MIQP, and a rank-deficient Q — are declared `false` in its capability table and refused
at **plan time** (`RequireDecideSolverSupport`, stage 05), before the query reads a row.
The last differs in kind from the other three: HiGHS *loads* a rank-deficient Q happily,
it just answers it wrong (see stage 05's note on `singular_quadratic`), so the refusal
is a judgement about answer quality rather than about what the API accepts. In practice
it means every Q reaching `passHessian` here is diagonal, though the conversion above
stays written for the general case against the day the flag is lifted. They used to
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
files. A backend's `Capabilities()` reports what it *can* do and nothing else — the
`DECIDB_NATIVE_CONSTRUCTS` switch is applied above it, so a new backend inherits the
A/B harness for free. If a fourth step ever appears — an `if` on the backend's name, a
`switch` on a new enum — the difference it is branching on belongs in
`SolverConstructSupport` or `SolverModelClass` instead.

`SolverModel` provides everything needed: variable bounds and types, linear and
quadratic objective, and constraints in COO. No change to extraction, coefficient
evaluation, or model building is required.

---

## 7. Source map

| Concern | Location |
|---|---|
| Backend selection, `SolveModel`, disambiguation | `src/decidb/solver/ilp_solver.cpp` |
| Backend table | `src/decidb/solver/solver_registry.cpp`, `src/include/duckdb/decidb/solver/solver_registry.hpp` |
| Capability types, model-class gap, convexity predicate | `src/include/duckdb/common/decide_solver_capabilities.hpp` |
| `GeneralConstraintKind` → construct flag table | `src/include/duckdb/decidb/formulation/solver_input.hpp` |
| Normalized outcome and default error text | `src/include/duckdb/decidb/solver/solver_result.hpp` |
| Session contract | `src/include/duckdb/decidb/solver/solver_session.hpp` |
| Time limits | `src/include/duckdb/decidb/solver/solver_config.hpp` |
| Gurobi backend | `src/decidb/gurobi/gurobi_solver.cpp` |
| Gurobi dynamic loading | `src/decidb/gurobi/gurobi_loader.cpp` |
| HiGHS backend | `src/decidb/naive/deterministic_naive.cpp` |
