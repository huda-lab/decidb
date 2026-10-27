# Oracle-checked scale tests

`test/decide/tests/test_large_scale.py` holds two pytest tests that run DECIDE on a few hundred to a few thousand rows
and compare the result with the same model built independently in Python and solved by the oracle solver:

| Test | Query | Rows |
|---|---|---|
| `test_knapsack_large` | Boolean knapsack on `lineitem` with `l_orderkey < 500` | about 500 |
| `test_order_selection_large` | pick at most 1,000 `orders` of 1995 by total price | about 2,200 |

Each test times the DeciDB CLI call and the oracle's build and solve, compares the solutions, and records the timings
through the suite's `perf_tracker` fixture. Both carry the `large_scale` and `performance` markers.

## Running

The tests run with the suite. `./test/decide/run_tests.sh -m large_scale` runs only them; `-m "not large_scale"`
leaves them out.

For the oracle methodology, see [`02_operations/oracle.md`](../../02_operations/oracle.md).
