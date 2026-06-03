# Matrix Efficiency — Done

No matrix-level optimizations are currently implemented. The constraint matrix assembled by `SolverModel::Build()` is passed as-is to the solver (Gurobi or HiGHS). Both solvers perform their own internal presolve (variable fixing, constraint reduction, bound propagation), but DeciDB does not currently exploit problem structure to reduce the matrix before handing it off.

The optimizations in this area aim to make the ILP smaller and safer to solve — fewer constraints, tighter bounds, and configurable time limits.
