# Operations — open work

## OPS-1 — Report peak process memory on Windows

`PeakProcessMemoryBytes()` in `src/execution/operator/decide/physical_decide.cpp`
returns 0 on Windows, so the slow-solve report drops its "peak memory" line
there. POSIX uses `getrusage`; the Windows equivalent is `GetProcessMemoryInfo`
(`PeakWorkingSetSize`), which needs a psapi link entry in both `CMakeLists.txt`
and `tools/pythonpkg/setup.py` — two build systems to keep in step, neither
testable outside a 25-minute CI run. It is deferred rather than risk the
Windows build for one advisory sentence.

`test/decide/tests/test_query_diagnostics_slow.py` asserts that line is present,
so it would fail on Windows. The DECIDE Python suite does not run there today.

Acceptance criteria:

- a slow solve on Windows reports a peak-memory figure matching Task Manager's
  peak working set to within a few percent;
- `make release` and the wheel build both still link on all three platforms;
- the two `TestSlowCheckpointReport` cases pass unchanged on POSIX.
