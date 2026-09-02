# Operations — Open Release Work

## Validate distributable Python artifacts before production publication

The release workflow smoke-tests every wheel on every configured OS/Python axis by
importing `decidb`, executing `SELECT 42`, and solving a typed `DECIDE x(BOOL)` query
through bundled HiGHS. It does not install or execute the source distribution, and it
does not run the broader Python package suite against the built artifacts. Before
publishing the next production release, run the TestPyPI path and test the sdist in a
clean environment; restore a bounded post-build package-suite job if release risk calls
for more than the wheel smoke.

Acceptance criteria:

- wheel and sdist filenames carry the requested release version;
- a clean environment can install the sdist, import `decidb`, run ordinary SQL, and
  run a typed DECIDE query through bundled HiGHS;
- the workflow's per-wheel smoke remains green on Linux x86-64, macOS arm64, and
  Windows AMD64 across Python 3.11–3.13 before PyPI publication.

## Report peak process memory on Windows

`PeakProcessMemoryBytes()` in `src/execution/operator/decide/physical_decide.cpp`
returns 0 on Windows, so the slow-solve report drops its "peak memory" line
there. POSIX uses `getrusage`; the Windows equivalent is `GetProcessMemoryInfo`
(`PeakWorkingSetSize`), which needs a psapi link entry in both `CMakeLists.txt`
and `tools/pythonpkg/setup.py` — two build systems to keep in step, neither
testable outside a 25-minute CI run. Deferred at the v0.2.0 build fixes rather
than risk the Windows build for one advisory sentence.

`test/decide/tests/test_query_diagnostics_slow.py` asserts that line is present,
so it would fail on Windows. The DECIDE Python suite does not run there today.

Acceptance criteria:

- a slow solve on Windows reports a peak-memory figure matching Task Manager's
  peak working set to within a few percent;
- `make release` and the wheel build both still link on all three platforms;
- the two `TestSlowCheckpointReport` cases pass unchanged on POSIX.

## Normalize the DecidB spelling in internal comments

v0.2.0 fixed the ~10 user-visible strings (Windows executable properties,
release-notes template, PyPI description) from `DecidB` to `DeciDB`. Roughly 430
occurrences remain in file-header comments and prose across `src/`. They ship to
nobody, and rewriting them is a large mechanical diff best done alone rather
than mixed into a functional change.

Acceptance criteria:

- `git ls-files | xargs grep -c 'DecidB[^B]'` reports zero outside
  `context/descriptions/`;
- no identifier, macro, or environment variable name changes (`DECIDB_` prefixes
  are unaffected).
