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
