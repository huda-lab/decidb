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

## OPS-2 — Normalize the DecidB spelling in internal comments

User-visible strings (Windows executable properties, release-notes template, PyPI
description) spell `DeciDB`. `DecidB` remains in file-header comments, prose and some names across the tree. The
comments ship to nobody, and rewriting them is a large mechanical diff best done alone rather
than mixed into a functional change.

Acceptance criteria:

- `git ls-files | xargs grep -c 'DecidB[^B]'` reports zero outside
  `context/docs/`;
- no identifier, macro, or environment variable name changes (`DECIDB_` prefixes
  are unaffected).

**Decision needed.** The acceptance criteria contradict each other: zero `DecidB` matches outside `context/docs/`, and
no identifier changes. About 470 matches remain (counted 2026-10-29: 378 in `test/decide/`, 63 in
`third_party/libpg_query/`, 12 in `src/`), and about 140 of them are names: the test harness classes `DecidBCli` and
`DecidBCliError`, the workflow file `.github/workflows/DecidBRelease.yml`, and CMake's `project(DecidB)` (the build's
project name). (a) Rename those too (`DeciDBCli`, `DeciDBRelease.yml`, `project(DeciDB)`), one mechanical change.
(b) Comments and prose only, and the acceptance test excludes the names. Recommended: (a), since the test classes are
internal and the spelling would otherwise stay.

**Ruling (owner, 2026-10-29).** (a): `DeciDB` is the spelling, names included.
