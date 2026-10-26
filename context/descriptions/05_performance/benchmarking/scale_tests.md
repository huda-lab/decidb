# Oracle-checked scale tests

`benchmark/decide/test_large_scale.py` holds two pytest tests that run DECIDE on a few hundred to a few thousand rows
and compare the result with the same model built independently in Python and solved by the oracle solver:

| Test | Query | Rows |
|---|---|---|
| `test_knapsack_large` | Boolean knapsack on `lineitem` with `l_orderkey < 500` | about 500 |
| `test_order_selection_large` | pick at most 1,000 `orders` of 1995 by total price | about 2,200 |

Each test times the DeciDB CLI call and the oracle's build and solve, compares the solutions, and records the timings
through the suite's `perf_tracker` fixture. Both carry the `large_scale` and `performance` markers.

## Running

The file uses fixtures from `test/decide/conftest.py` (`decidb_cli`, `duckdb_conn`, `oracle_solver`, `perf_tracker`)
and imports `solver.types` and `comparison.compare` from `test/decide/`. It sits outside `test/decide/tests/`, the
only path `run_tests.sh` collects (`pytest.ini` sets `testpaths = tests`). As a result
`./test/decide/run_tests.sh -m large_scale` selects no test, and running the file with pytest from the repository
root fails at import (`No module named 'solver'`). To run it, move it under `test/decide/tests/`.

For the oracle methodology, see [`02_operations/oracle.md`](../../02_operations/oracle.md).
