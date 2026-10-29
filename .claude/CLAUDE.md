# Repository Guidelines

DeciDB extends DuckDB with SQL-native constrained optimization (`DECIDE`). Syntax reference:
`context/docs/language/guide.md`.

## Layout

- `src/` — C++ code; headers in `src/include/duckdb/`. DECIDE spans `src/decidb/` and the parser, planner, optimizer
  and execution layers. Change behavior in the layer that owns it.
- `test/decide/tests/` — Python tests; `test/decide/golden/` — model baselines; `test/common/` — C++ regressions.
- `benchmark/decide/` — performance work.
- `context/docs/` — internal docs; `context/website/` — website. Direct solve starts at
  `context/docs/direct_solve/README.md`.

## Design

Use the established pattern and its standard name before inventing one: GoF patterns, query-optimizer practice
(Volcano/Cascades rules, pipeline breakers, late materialization), compiler practice (front end, IR, back end), and
what DuckDB already does. When a design departs from the standard idea, say which one and why in the docs. Name the
pattern when proposing a design.

## Commands (from the repository root)

| Command | Purpose |
|---|---|
| `make release` | build the CLI and library |
| `build/release/decidb` | SQL shell |
| `make grammar-build` | regenerate the parser grammar and rebuild |
| `make decide-setup` | create the Python test environment |
| `./test/decide/run_tests.sh -k <pattern>` | focused Python tests |
| `make decide-test` | goldens + full pytest suite (~1 min) |
| `build/release/test/unittest "[decidb]"` | C++ tests |
| `make format-check` | formatting check |

## Style

DuckDB conventions: PascalCase types and methods, snake_case variables and files, tabs (width 4), 120 columns. Follow
`.editorconfig` and `.clang-format` (clang-format 11.0.1; Black 24+, 120 columns). No unrelated formatting changes.

## Testing

**What to run:**

- **While developing:** build, then `run_tests.sh -k <pattern>` over the tests the change touches. Always pass `-k` or
  `-m`: a file-only run prunes the oracle cache.
- **At each commit:** `make decide-test` once. Add `unittest "[decidb]"` if the change touches C++ tests or the code
  they cover (serialization, the direct IR).
- **Only when the change is in that area:**
  - serialization (`nodes.json`, `logical_operator.json`, `Serialize`/`Deserialize`):
    `DECIDB_VERIFY_SERIALIZER=1 make decide-test`;
  - solver backends or model building: `DECIDB_FORCE_SOLVER=highs make decide-test`;
  - a direct-solve rule: `DECIDB_TEST_DIRECT_SOLVE=off make decide-test` or `test/decide/run_direct_matrix.sh`.
- **Coverage:** `make decide-coverage` (8+ min) only in a test-evaluation pass the owner asks for. Read
  `build/coverage/run/report.json`; compare runs with `test/decide/coverage_report.py compare OLD NEW`.

Never rerun a suite without a code change since its last run.

**How to write tests:**

- Correctness tests compare CLI output with an independent Python solver oracle (needs a Gurobi license).
- Every decision point a change adds gets a test that reaches it and fails if that branch is removed; name the branch.
  A grid case must reach a branch no other case reaches. Lizard measures complexity:
  `test/decide/.venv/bin/python -m lizard -l cpp <files>`.
- Code deleted → the tests that only reached it are deleted in the same change.
- Inspect golden diffs before regenerating; commit intentional baseline changes with the code.

## Commits

Concise subjects prefixed `decide:`, `docs:` or `release:`; focused commits. Preserve unrelated working-tree edits;
run `git diff --check` first. PR descriptions state the problem, the resulting behavior, the validation run, and any
skipped checks.
