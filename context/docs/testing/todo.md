# Testing — open work

Open coverage gaps, one section each. Coverage that exists is in [coverage.md](coverage.md) and
[scope_suite.md](scope_suite.md).

## TST-1 — The test CLI tells a NaN from a NULL

- **Problem.** `DeciDBCli.execute` reads the CLI's JSON output, which writes a NaN as `null`, so a test cannot tell a
  NaN from a NULL in a result. Reproduce: `SELECT 'NaN'::DOUBLE` through `DeciDBCli.execute` returns `None`. O1's
  NaN-key checker (`_same_value` in `test_direct_o1_monge_assignment.py`) relies on its tables holding no NULL.
- **Change.** The harness reads a NaN as `float('nan')`, for example by asking the CLI for a format that keeps it.
- **Done when.** A harness test reads back a NaN and a NULL as different values; the O1 checker drops its no-NULL
  assumption.
