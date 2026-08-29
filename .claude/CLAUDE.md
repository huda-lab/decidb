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
the contract below it, but must not take over its responsibilities. These are
the same eight stages documented in `context/descriptions/01_pipeline/`, one
folder each.

1. **Parser and parsed representation** — parse SQL/DECIDE syntax, resolve
   `WHEN` association in the grammar, and retain source structure. Do not
   perform binding, optimization, or solver work, do not move comparison terms,
   and do not desugar a formulation on the parsed tree.
2. **Binder and semantic analysis** — resolve names, scopes, types, polynomial
   degree, and DECIDE validity. Produce a clear bound representation; do not
   execute or solve, and do not flip or repartition a comparison.
3. **Logical planning** — represent the decision query and its relational
   inputs. Own subquery-correlation provenance and the `AddConstraint` /
   `SetObjective` entry points. Do not contain solver-specific structures or
   execution mechanics.
4. **Canonicalization** — decide the structural shape of every constraint and
   objective, once, on the bound tree: decisions left, bound right, one spelling
   per reducer scale. Never open a term algebraically. This is the only layer
   that decides shape, and no later layer may re-decide it.
5. **DECIDE optimization and rewriting** — assume canonical input and select
   mathematical formulations (`norm`, `IN`, ABS, MIN/MAX, AVG, `<>`, bilinear).
   Lower the binder's `norm`/`IN` markers here, where types, scopes and casts
   are known. Return every emitted row through layer 3's entry points. Do not
   parse SQL, execute relations, decide shape, or call a solver.
6. **Model formulation** — translate the evaluated decision problem into a
   solver-neutral model. Own model variables, constraints, objectives, and
   indexing.
7. **Solver facade and backends** — translate the neutral model to Gurobi or
   HiGHS and normalize statuses, solutions, and backend errors. Do not inspect
   SQL plans or decide query semantics.
8. **Physical execution and readback** — execute relational inputs, extract
   model terms from the canonical trees, evaluate coefficients, invoke the
   solver contract, and map results or diagnostics back to DuckDB output. Do
   not implement rewrites or backend-specific formulation logic here, and do
   not repair shape — assert it.

When a change crosses layers, define or repair the contract between them. Do
not duplicate a concept in multiple layers merely to avoid choosing an owner.
New code should live with the layer that owns its behavior, not beside the
caller that happens to need it first.

## Development rules

- Follow DuckDB patterns before inventing DeciDB-specific ones.
- Keep the DuckDB core changes small and DeciDB extensions cohesive.
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

Plan serialization is checked by a switch that is **off by default**, so run it
periodically rather than only when touching `LogicalDecide`. It round-trips each
bound plan through serialization and runs the copy, so a plan field that never
reaches the wire shows up as a wrong answer in whatever test depends on it:

```bash
DECIDB_VERIFY_SERIALIZER=1 test/decide/.venv/bin/python3 -m pytest test/decide/tests
```

*Currently blocked* — the pragma also enables DuckDB's re-parse verifier, which a
DECIDE statement fails for an unrelated reason. See
`context/descriptions/01_pipeline/01_parser/todo.md`, and delete this note when it
closes.

Grammar changes under `third_party/libpg_query/grammar/` require:

```bash
make grammar-build
```

## Documentation

Update the relevant documentation in the same session when behavior or
architecture changes. Keep `todo.md` for remaining work only and `done.md` for
the current implementation; do not use either as a historical checklist.

## User Interaction rules

- Communicate with the user using simple intutive language. Avoid jargon but if you have to include jargon define it first.
- Remeber this is a large codebase. The user may have forgotten about some implementations. Use your judgement to provide refreshers when you think its needed.
