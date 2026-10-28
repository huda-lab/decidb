# Repository Guidelines

## Project Structure & Module Organization

DeciDB extends DuckDB with SQL-native constrained optimization. Core C++ code lives in `src/`, with headers under `src/include/duckdb/`. DECIDE implementation spans `src/decidb/` and the parser, planner, optimizer, and execution layers. Keep changes in the layer that owns the behavior.

DECIDE Python tests live in `test/decide/tests/`, model baselines in `test/decide/golden/`, and C++ regressions in `test/common/`. Use `benchmark/decide/` for performance work. Internal documentation is in `context/docs/`; website content and assets are in `context/website/`. Consult `context/docs/language/guide.md` for DECIDE syntax.

For direct-solve prototype work, start with `context/docs/direct_solve/README.md`. Its navigation table points to the architecture notes and to each rule's `todo.md` and `done.md`.

## Design: Prefer Standard Patterns and Names

Before designing a component or naming a layer, look for the established pattern that already solves it: GoF design
patterns (*Design Patterns: Elements of Reusable Object-Oriented Software*), query-optimizer practice (Volcano/Cascades
transformation rules, pipeline breakers, late materialization), compiler practice (front end, IR, back end), and what
DuckDB already does. Use the standard name for the standard idea. Invent a mechanism or a term only when no standard one
fits, and say in the docs which standard idea it departs from and why. When proposing a design, name the pattern it
follows.

## Build, Test, and Development Commands

Run commands from the repository root:

- `make release`: build the optimized CLI and library; requires CMake, Python 3, and a C++11 compiler.
- `build/release/decidb`: start the local SQL shell.
- `make grammar-build`: regenerate the parser grammar and rebuild.
- `make decide-setup`: create the Python test environment and install dependencies.
- `make decide-test`: check model goldens, then run the pytest differential suite.
- `./test/decide/run_tests.sh -k test_var_boolean`: run focused Python tests.
- `build/release/test/unittest "[decidb]"`: run DeciDB C++ tests.
- `make format-check`: check repository formatting.

## Coding Style & Naming Conventions

Follow surrounding DuckDB conventions: C++ types and methods use PascalCase; variables and filenames use snake_case. C++ indentation uses tabs with width four and a 120-column limit. Follow `.editorconfig` and `.clang-format`. The formatter requires clang-format 11.0.1 and Black 24 or newer; Python formatting uses a 120-column limit. Avoid unrelated formatting changes.

## Testing Guidelines

Use pytest files/functions named `test_*` and existing category markers. Correctness tests compare CLI results with independent Python solver oracles; the oracle suite requires a valid Gurobi license. Add behavior-focused regressions for changed semantics, including relevant error cases.

Keep tests linked to the decision points of the code they test. Measure with lizard (installed in the test venv by `make decide-setup`): `test/decide/.venv/bin/python -m lizard -l cpp <files>` prints the cyclomatic complexity (CCN) of each function. Every decision point a change adds needs a test that reaches it and fails if that branch is removed; name the branch each new test reaches, from the code. A parametrized grid case must reach a branch no other case reaches; name the branch before adding it. When a change deletes code, delete the tests that only reached that code in the same change.

The suites are slow; run each one only when the change touches what it checks. Do not rerun a suite without a code change since its last run.

- **While developing:** build, then run the focused tests the change touches with `./test/decide/run_tests.sh -k <pattern>` (always pass `-k` or `-m`: a file-only run prunes the oracle cache). Nothing else.
- **At each commit:** `make decide-test` once (goldens and the full pytest suite, about a minute; direct solve is on by default, so it also compares the direct path). Add `build/release/test/unittest "[decidb]"` when the change touches C++ tests or the code they cover (serialization, the direct IR).
- **Only when the change is in that area:**
  - serialization (`Serialize`/`Deserialize`, `nodes.json`, `logical_operator.json`): `DECIDB_VERIFY_SERIALIZER=1 make decide-test` and the C++ serialization regressions;
  - solver backends or model building: `DECIDB_FORCE_SOLVER=highs make decide-test`;
  - a direct-solve rule: `DECIDB_TEST_DIRECT_SOLVE=off make decide-test` (the solver path alone) or `test/decide/run_direct_matrix.sh` (all five configurations, several minutes).
- **Coverage is not part of development.** `make decide-coverage` (8+ minutes) runs only in a test-evaluation pass the owner asks for, which trims tests that reach no branch of their own. Read `build/coverage/run/report.json` directly and compare runs with `test/decide/coverage_report.py compare OLD_RUN NEW_RUN`; `coverage_report.py report --unreached` takes over ten minutes.

Inspect golden differences before regenerating baselines; include intentional baseline changes with the implementation.

## Commit & Pull Request Guidelines

Recent commits use concise subjects, often prefixed with `decide:`, `docs:`, or `release:`. Keep commits focused. PR descriptions should explain the problem, resulting behavior, and validation performed; link relevant issues and disclose skipped checks or solver limitations. Include screenshots for visible website changes. Preserve unrelated working-tree edits and run `git diff --check` before submission.
