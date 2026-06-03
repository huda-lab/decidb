# /test-fill — Iterative Test-Gap Closure

Picks a small batch of high-value gaps from `context/descriptions/05_testing/*/todo.md`, plans them with the user, writes oracle-compared tests, runs the suite, and updates the docs in the same session. Designed for tight iteration: one batch per invocation.

## Arguments

`$ARGUMENTS` — optional scope:
- *(empty)* — survey all areas, recommend a batch
- a feature area name (e.g., `per`, `bilinear`, `quadratic`) — restrict batch to that area
- a number (e.g., `5`) — override default batch size of 3

## Hard rules

- **No unilateral design choices.** Every nontrivial decision goes through `AskUserQuestion`: which gaps, how many, where to put tests, what SQL shape, what to do if a bug is found.
- **Every new correctness test is oracle-verified.** Build the same problem independently in gurobipy via `oracle_solver` and compare with `compare_solutions`. Analytical / hand-computed closed-form assertions (e.g. `expected = {1: 10.0, 2: 20.0, 3: 30.0}`) are **forbidden** — see `context/descriptions/05_testing/README.md` and `02_operations/oracle.md`. "constraint only" is a legacy tier, acceptable only for pure feasibility queries with no objective.
- **Independent semantics, not Big-M mirrors.** For discrete constructs DeciDB rewrites into Big-M ILPs (`<>`, etc.), encode the semantics natively using Gurobi features via `oracle_solver.add_indicator_constraint` or the primitives in `tests/_oracle_helpers.py`. Mirroring DeciDB's Big-M only detects lockstep bugs; independent encoding catches encoding errors too.
- **Never silently flip a failing test to make it pass.** Stop and escalate to the user. Oracle mismatches usually mean a real bug — don't rewrite the oracle to match DeciDB.
- **Update docs in the same session** — `done.md` and `todo.md` for every area touched. Not optional (CLAUDE.md mandate).
- **One batch per invocation.** Don't sprawl into Batch 2 unless the user asks.

## Phase 1 — Survey

Spawn one Explore subagent across all `context/descriptions/05_testing/*/todo.md` files (14 files). Ask it to produce a consolidated report:
- HIGH-risk gaps (and notable MEDIUM ones) grouped by area
- For each: short description, why high-risk, rough effort estimate (lines of test, infra needed)
- A top 5–8 recommendation list ranked by `risk × ease`

Read `context/descriptions/05_testing/README.md` first for the risk taxonomy if not already in context.

## Phase 2 — Scope the batch (all via `AskUserQuestion`)

Ask, one question at a time only when needed:
1. **Slice**: standalone quick wins / start a cluster (e.g., PER cluster) / mix?
2. **Batch size**: 2 / 3 / 4–5? (Default 3 if `$ARGUMENTS` didn't specify.)
3. **Specific N gaps** from the chosen slice — let user pick.
4. **Test file location**: extend an existing file / extend the X-feature file / new cross-cutting file?
5. **Bug-found protocol**: xfail with note / stop and escalate?

If a particular gap is risky in a known way (e.g., may uncover an oracle bug), surface that too with options.

## Phase 3 — Plan to file

Write the plan file (path provided by plan-mode system message, or `/Users/hatim/.claude/plans/` if not in plan mode):

```
# Testing Gap Cleanup — Iterative Roadmap

## Context
<why this work, audit date, current weak area>

## Roadmap (sketch)
- Batch 1 — <focus> (this session)
- Batch 2+ — <future candidates>

## Batch N — <focus> (M gaps)
For each gap:
- **Gap** (file:line ref): description
- **Proposed query shape**: draft SQL
- **Docs**: which done.md/todo.md to update

### Bug-handling protocol (Batch N)
<from user choice>

### Verification
<commands>

### Critical files
<new files + read-only refs + doc files>

## Future batches (sketch)
<short bullet list per future batch>
```

Then `ExitPlanMode`.

## Phase 4 — Design each test (still asking)

1. Read 1–2 existing tests in the area to learn fixture conventions, oracle pattern, perf_tracker call shape. Also skim `tests/_oracle_helpers.py` for primitives that already cover the construct (group_indices, add_ne_indicator, add_in_domain, add_bool_and, emit_inner_max/min, emit_hard_inner_max/min).
2. For each test, draft SQL + outline oracle formulation. Show inline before writing code.
3. `AskUserQuestion`: use todo.md SQL verbatim / adjust / fresh? Scope (one direction / both / all variants)? Do not ask "oracle or constraint-only?" — oracle is the only valid answer for correctness tests.
4. If user says "sharper" or "show me", iterate the draft inline (don't write file yet).

## Phase 5 — Smoke-test SQL

For risky cases (equality constraints, bounded-data constraints, anything where infeasibility depends on data values), run the bare query via the CLI to confirm feasibility before committing to the test design:

```bash
./build/release/decidb decidb.db -readonly -json -c "<SQL>"
```

If infeasible: adjust constants, expand data scope (e.g., `l_orderkey <= 100`), or fall back to a CTE. Re-confirm with user if the change is material.

## Phase 6 — Write the test file

- Module docstring listing all tests + the cross-cutting concern they target
- Each test:
  - Pytest marks (per_clause, min_max, var_*, correctness, etc.)
  - SQL string
  - Run DeciDB via `decidb_cli.execute(sql)` → `(rows, cols)`
  - Fetch oracle data via `duckdb_conn` with explicit casts
  - Build the ILP in gurobipy via `oracle_solver`. Use the helpers in
    `tests/_oracle_helpers.py` for common constructs rather than rolling
    your own Big-M. For quadratic objectives/constraints use
    `oracle_solver.set_quadratic_objective(linear, quadratic, sense)` and
    `oracle_solver.add_quadratic_constraint(linear, quadratic, sense, rhs)`.
    For `<>`, discrete domain restrictions, use native
    indicator constraints via `oracle_solver.add_indicator_constraint` or
    the matching helpers — **do not mirror DeciDB's Big-M rewrite**.
  - `oracle_solver.solve()` → assert OPTIMAL (or INFEASIBLE/UNBOUNDED for
    cross-verified error tests)
  - Compare via `compare_solutions(decidb_rows, decidb_cols, result, data,
    decide_var_names, coeff_fn=...)`. For non-linear (QP/QCQP) objectives,
    pass `decidb_objective_fn=lambda rows, cols: ...` instead of `coeff_fn`
    and evaluate the objective directly on DeciDB's variable values.
  - `perf_tracker.record(..., comparison_status=cmp.status, decide_vector=cmp.oracle_vector)`

Reference patterns: `test_var_boolean.py`, `test_per_clause.py`,
`test_per_objective.py` (nested aggregates with `emit_inner_max/min`),
`test_quadratic.py`, `test_quadratic_constraints.py`, `test_error_infeasible.py`
(oracle cross-verification of infeasibility).

## Phase 7 — Verify

```bash
./test/decide/run_tests.sh tests/test_<new_file>.py    # targeted
make decide-test                                       # full suite
```

If anything fails: stop and escalate per the bug-handling protocol. Do not silently change the test.

## Phase 8 — Update docs

For every area whose gap was closed:
- `done.md`: add the new test to "Tests live in", "Scenarios covered", and "Feature interactions covered" tables.
- `todo.md`: remove the closed gap (or replace with a one-line "covered in test_X.py" pointer).

Reference: `context/descriptions/05_testing/README.md` for the docs layout.

## Tracking

Use `TaskCreate` at the start:
- One task per gap in the batch
- Plus "Read existing test patterns" and "Run full suite + update docs"

`TaskUpdate` to in_progress when starting each, completed immediately when done.

## End-of-batch summary

Report to the user:
- New file path + test names
- Suite result (X passed, Y skipped/xfailed, no regressions)
- Whether any bugs were surfaced
- Docs touched
- Top 3 remaining HIGH-risk gaps for the next batch

Do not auto-start Batch 2.
