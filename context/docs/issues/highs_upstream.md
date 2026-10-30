# Draft: HiGHS upstream issue — six presolve and MIP loops that never check the time limit

Not posted. The owner posts it on github.com/ERGO-Code/HiGHS. Before posting:

- attach one MPS file per case, written with `Highs::writeModel` from the model DeciDB loads (Q1, Q2, Q3 and Q10 at
  `large`), so the report does not depend on DeciDB;
- check the current HiGHS release for the same loops; drop any case it already fixes.

The patches it describes are listed in `third_party/highs/README.md`.

---

**Title:** MIP time limit and user interrupt are not checked in six loops (clique partition, symmetry detection,
dominated-columns presolve, clique extraction, conflict analysis, parallel rows and columns)

**Version:** HiGHS 1.11.0 (git 364c83a51), built from source as a static library. macOS arm64.

**Summary.** Five MIP models run minutes past `time_limit`, and the user interrupt callback is not called
meanwhile. In each case the time goes to a loop that reads the clock only after some event
(a reduction, a search leaf), and the model produces none of those events. We patched each loop locally with a
periodic check that leaves through an exit the code already has. The patches are below in case they are useful.

### 1. `HighsCliqueTable::cliquePartition` (objective clique partition, MIP setup)

- **Symptom:** a model with many binary objective columns that share cliques was still in MIP setup, before the first
  B&B node, when our harness killed it at 420 s. The pairwise neighbourhood-query loop has no limit check.
- **Patch:** `cliquePartition` takes the `HighsMipSolver` and calls `mipdata_->checkLimits()` once per 100,000
  neighbourhood queries. Past a limit, the remaining columns become singleton cliques: a weaker partition, still valid.
  The `checkLimits()` that follows returns the time limit (or interrupt) status.

```cpp
const int64_t kQueriesPerTimeCheck = 100000;
int64_t nextTimeCheck = numNeighbourhoodQueries;
for (HighsInt i = 0; i < numClqVars; ++i) {
  if (numNeighbourhoodQueries >= nextTimeCheck) {
    nextTimeCheck = numNeighbourhoodQueries + kQueriesPerTimeCheck;
    if (mipsolver.mipdata_->checkLimits()) {
      if (partitionStart.back() != i) partitionStart.push_back(i);
      for (HighsInt j = i + 1; j < numClqVars; ++j) partitionStart.push_back(j);
      break;
    }
  }
  ...
```

### 2. `HighsSymmetryDetection` (symmetry detection, root node)

- **Symptom:** a MIP with 5M columns, 8M rows and 2M binaries (6.4M rows, 4.2M columns after presolve) stopped with
  "Time limit reached" at 681.7 s under a 300 s limit, 0 nodes. Stack samples of smaller variants are in
  `HighsSymmetryDetection::run`, under `compareCurrentGraph` (which compares the whole graph at each search leaf) or
  `partitionRefinement`, while the main thread waits in `evaluateRootNode`. Time grows faster than size: the same
  model family at 25k, 50k and 100k items solves in 12 s, 32 s and 118 s, and 200k items hits a 120 s limit.
- **Related:** when the root solves the MIP while detection runs, the solve still waits for the search to finish: the
  task's cancellation is checked only at search leaves (`workerDeque->checkInterrupt()`). One model optimal at the
  root returned after 9.4 s instead of 1.6 s.
- **With the patch:** the 681.7 s model stops at 314 s.
- **Patch:** `initializeDetection` takes the MIP timer, the time limit and the top-level callback (null for a sub-MIP).
  A read-only `checkStop()` runs once per 100 refined cells in `partitionRefinement` and once per node in `run()`;
  `run()` also calls `checkInterrupt()` per node. On a stop, `run()` leaves through the same exit as the `maxPerms`
  break and keeps the generators found. `checkLimits()` is not called from the detection task, since it writes
  `modelstatus_` and the shared callback data while the main thread does too; the interrupt callback is called with
  local `HighsCallbackOutput`/`HighsCallbackInput`.

```cpp
bool HighsSymmetryDetection::checkStop() {
  if (stopped) return true;
  if (timeLimit < kHighsInf && timer->read() >= timeLimit) {
    stopped = true;
  } else if (callback && callback->user_callback &&
             callback->active[kCallbackMipInterrupt]) {
    HighsCallbackOutput dataOut;
    HighsCallbackInput dataIn;
    callback->user_callback(kCallbackMipInterrupt, "Symmetry detection",
                            &dataOut, &dataIn, callback->user_callback_data);
    stopped = dataIn.user_interrupt;
  }
  return stopped;
}

// in run(), at the top of the node loop:
if (checkStop()) break;
workerDeque->checkInterrupt();
```

### 3. `HPresolve::dominatedColumns` (presolve)

- **Symptom:** a MIP with 840,000 columns and 1,560,001 rows, some rows covering all columns, stayed in presolve for
  638 s under a 300 s limit and reduced nothing (the size line is the same at 0 s and 638 s), then printed "Presolve:
  Time limit reached". Stack samples at 120 s and 240 s are all in `dominatedColumns`. The column loop scans every row
  of each column in full, and `checkTimeLimit()` runs only from `checkLimits()`, once per 1,024 reductions.
- **Patch:** the column loop calls `checkTimeLimit()` once per 1,024 columns and returns its result, so presolve stops
  through its existing `kStopped` path. The check runs before a column's work starts, so every earlier column is
  complete when it stops.
- **With the patch:** the 638 s model stops at 301 s.
- **Not patched:** `HPresolve.cpp` never calls the user interrupt callback, so a Ctrl-C during a long presolve pass
  waits for presolve to end.

```cpp
for (HighsInt j = 0; j < model->num_col_; ++j) {
  if ((j & 1023) == 0) HPRESOLVE_CHECKED_CALL(checkTimeLimit());
  ...
```

### 4. `HighsCliqueTable::extractCliques` (presolve probing)

- **Symptom:** a MIP with 1,000,000 binary columns over 8 rows; presolve merges duplicate columns into general integers,
  leaving 4 rows of about 700,000 nonzeros, each mixing about 146,000 binaries with 138,000 integers. One presolve
  round took 215 s (the size line moves from 13 s to 228 s and removes 3 columns); in another run the solve ran past
  420 s under a 300 s limit. Stack samples are in `runProbing` → `extractCliques(HighsMipSolver&, bool)` → the per-row
  `extractCliques`, whose variable-bound loop pairs every binary with every non-binary column of the row.
- **Patch:** the pair loop calls `mipdata_->checkLimits()` once per 2^20 (binary, non-binary) pairs. Past a limit it
  returns from the row, keeping the variable bounds already added (they only strengthen the model).

```cpp
for (HighsInt i = 0; i != nbin; ++i) {
  pairsSinceLimitCheck += ntotal - nbin;
  if (pairsSinceLimitCheck >= kPairsPerLimitCheck) {
    pairsSinceLimitCheck = 0;
    if (mipsolver.mipdata_->checkLimits()) return;
  }
  ...
```

### 5. Conflict analysis under line-search rounding (root heuristics)

- **Symptom:** the same model in another run: stack samples at 330 s and 400 s, under a 300 s limit, are both in
  `evaluateRootNode` → `linesearchRounding` → `tryRoundedPoint` → `HighsDomain::conflictAnalysis` → `resolveDepth` →
  `explainBoundChangeLeq`. Each explanation scans a row of about 700,000 entries, so one conflict analysis outlasts the
  limit.
- **Patch:** `ConflictSet` counts the row entries its explanations scan; `resolveDepth` calls `checkLimits()` once per
  2^20 entries and, past a limit, clears its queue and marks the set stopped. `computeCuts` then adds no cut, and both
  `conflictAnalysis` overloads and `conflictAnalyzeReconvergence` return on the mark. `linesearchRounding` also calls
  `checkLimits()` before each rounded point.
- **With the patches (4 and 5):** the model stops at 300 s.

### 6. `HPresolve::detectParallelRowsAndCols` (presolve)

- **Symptom:** one row over 120,000 integer columns with distinct coefficients stays in this pass 25 s under a 1 s
  limit; 110,000 inequalities over two columns each, 23.5 s. Columns (or rows) that share a hash are compared pairwise
  in their bucket, and when no pair is parallel no reduction happens, so `checkLimits` never runs.
- **Patch:** both bucket loops count the candidates they compare and call `checkTimeLimit()` once per 2^20 of them,
  before a candidate's work, returning through `kStopped`.
- **With the patch:** both models stop at about 2 s (a 1 s limit plus process start).

```cpp
while (it != buckets.end() && it->first == colHashes[i]) {
  if (++numBucketCandidates % kCandidatesPerTimeCheck == 0)
    HPRESOLVE_CHECKED_CALL(checkTimeLimit());
  ...
```

### Suggestion

A clock check inside each loop whose work does not depend on making progress (reductions, nodes, leaves) would cover
these cases and others like them. We are happy to open a pull request with any of the six patches.
