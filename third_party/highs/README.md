# HiGHS (vendored, patched)

HiGHS 1.15.1 (`04024d701`). `highs/` and `extern/` are upstream's, minus the sources its static library does not
compile in DeciDB's configuration, plus the patch series in `patches/`. `CMakeLists.txt`, `HConfig.h` and `patches/`
are DeciDB's own; the build compiles every source in the tree.

`make upgrade-highs HIGHS_VERSION=<x.y.z>` (`scripts/upgrade_highs.py`) replaces `highs/` and `extern/` with that
release, writes its version here and into `HConfig.h`, and applies the series in order. Rerunning it at the current
version leaves the tree unchanged.

When a patch does not apply, the command stops and names it; the tree holds the release plus the patches before it.
Port that patch by hand (same check, same exit), or delete it when upstream checks the same limit in that loop, then
write it back and rerun:

    git add third_party/highs                     # the release plus the earlier patches
    # ... port the patch ...
    git diff --relative=third_party/highs -- third_party/highs/highs third_party/highs/extern \
        > third_party/highs/patches/<NNNN-ID>.patch

Each patch's edits are marked `DeciDB patch (<id>)` in the source. Why each exists:
`context/docs/pipeline/07_solver/done.md`.

- `0001-SOL-1` (`mip/HighsCliqueTable.*`, `mip/HighsObjectiveFunction.*`): the objective clique partition stops at the
  time limit. SOL-2 changed the same hunk to call `checkLimits`, so Ctrl-C stops it too (marked SOL-1).
- `0002-BUG-3` (`presolve/HighsSymmetry.*`, `mip/HighsMipSolverData.cpp`): symmetry detection stops at the time
  limit, a Ctrl-C, or the MIP finishing without it.
- `0003-BUG-23` (`presolve/HPresolve.cpp`): the dominated-columns pass stops at the time limit.
- `0004-BUG-25` (`mip/HighsCliqueTable.cpp`, `mip/HighsDomain.*`, `mip/HighsPrimalHeuristics.cpp`): clique
  extraction's variable-bound loop, conflict analysis and line-search rounding stop at the time limit or a Ctrl-C.
- `0005-BUG-28` (`presolve/HPresolve.cpp`): the parallel rows and columns pass stops at the time limit.
