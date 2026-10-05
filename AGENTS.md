# Repository Guidelines

## Project Structure & Module Organization

DeciDB extends DuckDB with SQL-native constrained optimization. Core C++ code lives in `src/`, with headers under `src/include/duckdb/`. DECIDE implementation spans `src/decidb/` and the parser, planner, optimizer, and execution layers. Keep changes in the layer that owns the behavior.

DECIDE Python tests live in `test/decide/tests/`, model baselines in `test/decide/golden/`, and C++ regressions in `test/common/`. Use `benchmark/decide/` for performance work. Internal documentation is in `context/descriptions/`; website content and assets are in `context/website/`. Consult `context/descriptions/00_project_overview/syntax_reference.md` for DECIDE syntax.

For direct-solve prototype work, start with `context/descriptions/08_direct_solve/README.md`. Its navigation table points to the architecture notes and to each rule's `todo.md` and `done.md`.

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

For solver changes, also run `DECIDB_FORCE_SOLVER=highs make decide-test`. Direct solve is on by default; `DECIDB_TEST_DIRECT_SOLVE=off make decide-test` runs the whole suite on the solver path alone. For serialization changes, run `DECIDB_VERIFY_SERIALIZER=1 make decide-test` and C++ serialization regressions. Inspect golden differences before regenerating baselines; include intentional baseline changes with the implementation.

## Commit & Pull Request Guidelines

Recent commits use concise subjects, often prefixed with `decide:`, `docs:`, or `release:`. Keep commits focused. PR descriptions should explain the problem, resulting behavior, and validation performed; link relevant issues and disclose skipped checks or solver limitations. Include screenshots for visible website changes. Preserve unrelated working-tree edits and run `git diff --check` before submission.
