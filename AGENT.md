# DeciDB Development Guide

DeciDB extends DuckDB with a `DECIDE` clause for in-database constrained
optimization. The current priority is an architectural overhaul: keep the
implementation cohesive, make layer boundaries explicit, and give every layer
one well-defined job.

## Before changing code

- Read `context/descriptions/README.md` for documentation navigation.
- Read `context/descriptions/00_project_overview/syntax_reference.md` before
  changing DECIDE syntax or semantics.
- Inspect the existing path end to end before adding a new abstraction. Prefer
  moving or consolidating logic over adding another scattered helper.
- Check `git status` first and preserve unrelated worktree changes.

## Layer ownership

Keep dependencies flowing downward through stable contracts. A layer may use
the contract below it, but must not take over its responsibilities.

1. **Parser and symbolic representation** — parse SQL/DECIDE syntax and retain
   source structure. Do not perform binding, optimization, or solver work.
2. **Binder and semantic analysis** — resolve names, scopes, types, and DECIDE
   validity. Produce a clear bound representation; do not execute or solve.
3. **Logical planning** — represent the decision query and its relational
   inputs. Do not contain solver-specific structures or execution mechanics.
4. **DECIDE optimization and rewriting** — normalize expressions and select
   mathematical formulations. Do not parse SQL, execute relations, or call a
   solver.
5. **Model formulation** — translate the bound/logical decision problem into a
   solver-neutral model. Own model variables, constraints, objectives, and
   indexing.
6. **Solver facade and backends** — translate the neutral model to Gurobi or
   HiGHS and normalize statuses, solutions, and backend errors. Do not inspect
   SQL plans or decide query semantics.
7. **Physical execution and readback** — execute relational inputs, invoke the
   solver contract, and map results or diagnostics back to DuckDB output. Do
   not implement rewrites or backend-specific formulation logic here.

When a change crosses layers, define or repair the contract between them. Do
not duplicate a concept in multiple layers merely to avoid choosing an owner.
New code should live with the layer that owns its behavior, not beside the
caller that happens to need it first.

## Development rules

- Follow DuckDB patterns before inventing DeciDB-specific ones.
- Keep the DuckDB core changes small and DeciDB extensions cohesive.
- Keep solver integration solver-agnostic; both Gurobi and HiGHS must remain
  valid implementations unless a task explicitly changes that contract.
- Keep user-facing errors and diagnostics concise, actionable, and expressed
  in SQL terms rather than solver internals.
- For non-trivial work, first write down the affected layers, their contract,
  and the intended ownership before editing.
- Add or update behavior-focused tests at the boundary being changed. Verify
  targeted tests before broadening validation.

## Build and test

```bash
make release
make decide-test
```

The executable is `build/release/decidb`. DECIDE tests live under
`test/decide/`; use its virtual environment when running Python tests:

```bash
test/decide/.venv/bin/python3 -m pytest <test-or-directory>
```

Grammar changes under `third_party/libpg_query/grammar/` require:

```bash
make grammar-build
```

## Documentation

Update the relevant documentation in the same session when behavior or
architecture changes. Keep `todo.md` for remaining work only and `done.md` for
the current implementation; do not use either as a historical checklist.
