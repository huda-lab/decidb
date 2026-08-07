# Phase 4: Solver Backends

## Overview

DeciDB uses a solver facade pattern. The `SolveModel()` function in `ilp_solver.cpp` builds a `SolverModel` from the `SolverInput`, then dispatches to the selected backend. It supports a test override (`DECIDB_FORCE_SOLVER=highs|gurobi`) before the default auto-selection:

```cpp
vector<double> SolveModel(SolverInput &input, const VarIndexer &indexer) {
    SolverModel model = SolverModel::Build(input, indexer);
    if (const char *force = std::getenv("DECIDB_FORCE_SOLVER")) {
        std::string choice(force);
        if (choice == "highs" || choice == "HIGHS") {
            return DeterministicNaive::Solve(model);
        }
        if (choice == "gurobi" || choice == "GUROBI") {
            if (!GurobiSolver::IsAvailable()) {
                throw InvalidInputException(
                    "DECIDB_FORCE_SOLVER=gurobi but Gurobi is not available on this host");
            }
            return GurobiSolver::Solve(model);
        }
    }
    if (GurobiSolver::IsAvailable()) {
        return GurobiSolver::Solve(model);
    }
    return DeterministicNaive::Solve(model);
}
```

**Key Source Files**:
- `src/decidb/utility/ilp_solver.cpp` — solver facade (dispatch logic)
- `src/decidb/gurobi/gurobi_solver.cpp` — Gurobi backend (C API)
- `src/decidb/naive/deterministic_naive.cpp` — HiGHS backend (C++ API)

## Solver Selection

- If `DECIDB_FORCE_SOLVER=highs` or `DECIDB_FORCE_SOLVER=gurobi` is set, that backend is used first (unknown values are ignored and fall back to normal auto-selection).
- If `GurobiSolver::IsAvailable()` returns true, Gurobi is used. **Gurobi is the primary solver** and is strongly recommended — empirical benchmarking has shown it to be significantly faster than HiGHS for DeciDB workloads.
- Otherwise, HiGHS (via `DeterministicNaive`) is used as a fallback. HiGHS is substantially slower in practice and should only be used when a Gurobi license is unavailable.

`GurobiSolver::IsAvailable()` performs a one-time lazy check (static local variable with lambda initialization): it attempts to create a Gurobi environment via `GRBloadenv()`. If compilation did not include Gurobi (`DECIDB_HAS_GUROBI` not defined), it always returns false.

## Gurobi Backend

Uses Gurobi's **C API** (`gurobi_c.h`), not the C++ wrapper.

### Resource Management

An RAII wrapper `GurobiGuard` manages `GRBmodel*` and `GRBenv*` lifetime. The destructor calls `GRBfreemodel()` and `GRBfreeenv()`, ensuring cleanup on all exit paths including exceptions.

### Model Setup

1. **Environment**: Created via `GRBloadenv()`. `OutputFlag` is set to 0 (silent output).
2. **Variables**: All variables are added in one `GRBnewmodel()` call, which takes the variable types, bounds, and objective coefficients as arrays:
   - `GRB_BINARY` for `is_binary` variables
   - `GRB_INTEGER` for `is_integer` (non-binary) variables
   - `GRB_CONTINUOUS` for continuous variables
3. **Model sense**: Set via `GRBsetintattr(GRB_INT_ATTR_MODELSENSE, ...)` to `GRB_MAXIMIZE` or `GRB_MINIMIZE`.
4. **Constraints**: Added individually via `GRBaddconstr()`, which directly accepts COO format (index array, coefficient array, sense char, RHS).
5. **Quadratic objective** (QP/MIQP): If the model has a quadratic objective (`has_quadratic_obj`), Q matrix terms are added via `GRBaddqpterms()` which takes COO format (row indices, column indices, values). Gurobi natively supports both continuous QP and MIQP (quadratic with integer/boolean variables).

### Solving and Status

After `GRBoptimize()`, the status is checked:

| Gurobi Status | Action |
|---|---|
| `GRB_OPTIMAL` | Extract and return solution |
| `GRB_INFEASIBLE` | `InvalidInputException` with constraint diagnosis suggestions |
| `GRB_UNBOUNDED` | `InvalidInputException` suggesting bounds |
| `GRB_INF_OR_UNBD` | Disambiguated first: set `DualReductions=0` on the model's env (`GRBgetenv`) and re-optimize — yields a definitive `GRB_INFEASIBLE` or `GRB_UNBOUNDED` (Gurobi's documented recipe; presolve dual reductions are what blur the two). If the re-solve itself fails, a combined infeasible-or-unbounded `InvalidInputException` is raised |
| `GRB_TIME_LIMIT` | `InvalidInputException` about complexity |
| `GRB_ITERATION_LIMIT` | `InvalidInputException` about complexity |
| Other | Generic `InvalidInputException` with status code |

### Solution Extraction

Solution values are extracted via `GRBgetdblattrarray(GRB_DBL_ATTR_X, ...)` into a `vector<double>`. Each value is validated as finite (not NaN or Infinity).

## HiGHS Backend

> **Note**: HiGHS is retained as an open-source fallback for environments without a Gurobi license. Empirical benchmarking has shown it to be significantly slower than Gurobi for DeciDB workloads. It is not recommended for production use.

Uses HiGHS's **C++ API** (`Highs.h`). Despite the class name `DeterministicNaive`, this is a full-featured MIP solver.

### Variable Types

HiGHS uses `HighsVarType::kInteger` or `HighsVarType::kContinuous`. There is no separate binary type -- binary variables are represented as integer variables with bounds [0, 1].

### Constraint Conversion

ILP sense characters are converted to HiGHS range format (`row_lower`, `row_upper`):

| Sense | `row_lower` | `row_upper` |
|---|---|---|
| `'>'` (>=) | `rhs` | `1e30` |
| `'<'` (<=) | `-1e30` | `rhs` |
| `'='` | `rhs` | `rhs` |

### Matrix Format Conversion

The `SolverModel` stores constraints in COO format (row index, column index, value triples). HiGHS requires CSR (Compressed Sparse Row) format with `start_`, `index_`, `value_` arrays.

The conversion:
1. Count non-zeros per row (via `row_starts[a_rows[i] + 1]++`)
2. Prefix sum to get row start offsets
3. Scatter COO entries into CSR positions using a `current_pos` tracking array

The matrix format is set to `MatrixFormat::kRowwise`.

### Model and Solving

The `HighsLp` struct is populated with:
- `col_cost_` (objective), `col_lower_`/`col_upper_` (variable bounds)
- `row_lower_`/`row_upper_` (constraint bounds)
- `a_matrix_` (constraint matrix in CSR)
- `integrality_` (variable types)
- `sense_` (`ObjSense::kMaximize` or `kMinimize`)

Logging is disabled with `log_to_console = false`. The model is passed via `highs.passModel(lp)` and solved with `highs.run()`.

### Status Handling

| HiGHS Status | Action |
|---|---|
| `kOptimal` | Extract and return solution |
| `kInfeasible` | `InvalidInputException` (same message as Gurobi) |
| `kUnbounded` | `InvalidInputException` (same message as Gurobi) |
| `kTimeLimit` | `InvalidInputException` |
| `kIterationLimit` | `InvalidInputException` |
| Other | Generic `InvalidInputException` with status code |

Solution is extracted from `highs.getSolution().col_value`, validated for completeness and finite values.

### Quadratic Objective (QP via HiGHS)

If `model.has_quadratic_obj` is true, the Q matrix is passed via `highs.passHessian()`. HiGHS expects CSC (Compressed Sparse Column) format for the lower triangle.

**MIQP Limitation**: HiGHS does not support mixed-integer quadratic programs. If any variable is integer/boolean and the objective is quadratic, an `InvalidInputException` is thrown directing the user to install Gurobi or use `REAL` variables.

The COO→CSC conversion:
1. Count entries per column
2. Build column start offsets via prefix sum
3. Scatter COO entries into CSC positions

## Error Messages

Both backends produce identical user-facing error messages for each failure mode, ensuring a consistent experience regardless of which solver is active. Messages include:

- **Infeasible**: Lists common causes (contradictory bounds, impossible SUM constraints, overly restrictive variable types) and suggests relaxing constraints.
- **Unbounded**: Explains the objective can grow infinitely and suggests adding upper bounds, budget limits, or using BOOL.
- **Time/Iteration Limit**: Suggests simplifying constraints or reducing data size.

## Adding a New Solver Backend

To add a new solver:

1. Create a new class with a static `Solve(const SolverModel &) -> vector<double>` method.
2. Optionally add a static `IsAvailable()` method for runtime detection.
3. Add a dispatch entry in `ilp_solver.cpp` (between Gurobi and HiGHS, or as a new priority level).
4. The `SolverModel` struct provides everything needed: variable bounds/types, objective coefficients (linear + quadratic Q matrix), and constraints in COO format.

No changes to the expression analysis, coefficient evaluation, or model building phases are required.
