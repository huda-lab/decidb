# Website — Open Work

The code, internal documentation, and website were reconciled on 2026-08-30.
There are no remaining unconfirmed content decisions from that review.

## Post-publication checks

Complete these after the CLI binaries and Python packages are published:

- Verify the website's GitHub release links resolve to the new release and that
  every advertised binary filename exists.
- Install `decidb` from PyPI in a clean supported Python environment and run
  both `SELECT 42` and a typed `DECIDE x(BOOL)` query with bundled HiGHS.
- Recheck the Getting Started installation path against the published package,
  then deploy the reviewed website.

The composed MIN/MAX equality inconsistency found during this review is a code
issue, not a website decision. It is tracked in
`context/descriptions/04_testing/min_max/todo.md`; the public documentation does
not recommend the equal-endpoint `BETWEEN` spelling as a workaround.
