# HiGHS (vendored, patched)

HiGHS 1.11.0 (`364c83a51`). `CMakeLists.txt` and `HConfig.h` are DeciDB's own build files. Each source edit is marked
`DeciDB patch (<id>)` (`grep -rn "DeciDB patch" third_party/highs` lists them); carry them over on an upgrade. Why
each exists: `context/docs/pipeline/07_solver/done.md`.

- SOL-1 (`mip/HighsCliqueTable.*`, `mip/HighsObjectiveFunction.*`): the objective clique partition stops at the time
  limit. SOL-2 changed the same hunk to call `checkLimits`, so Ctrl-C stops it too (marked SOL-1).
- BUG-3 (`presolve/HighsSymmetry.*`, `mip/HighsMipSolverData.cpp`): symmetry detection stops at the time limit, a
  Ctrl-C, or the MIP finishing without it.
- BUG-23 (`presolve/HPresolve.cpp`): the dominated-columns pass stops at the time limit.
- BUG-25 (`mip/HighsCliqueTable.cpp`, `mip/HighsDomain.*`, `mip/HighsPrimalHeuristics.cpp`): clique extraction's
  variable-bound loop, conflict analysis and line-search rounding stop at the time limit or a Ctrl-C.
- BUG-28 (`presolve/HPresolve.cpp`): the parallel rows and columns pass stops at the time limit.
