# /review — Multi-Agent Devil's Advocate Review

Spawns 3 parallel reviewer agents that independently critique your changes from different angles, then synthesizes their findings into an actionable report. Iterates with reviewers until concerns are resolved.

## Arguments

`$ARGUMENTS` — optional scope for the review:
- *(empty)* — review all unstaged + staged changes (`git diff HEAD`)
- `last N` — review the last N commits (e.g., `last 3`)
- any other text — treated as a feature/scope description to focus the review

## Procedure

### 1. Parse arguments

Parse `$ARGUMENTS` into:
- **diff_command**: default `git diff HEAD`. If `last N`, use `git diff HEAD~N..HEAD`. Otherwise, keep default.
- **scope_hint**: if `$ARGUMENTS` is text (not `last N` and not empty), pass it as a focus hint to reviewers.

### 2. Gather context

Run these commands to build a context bundle:

```bash
# The diff to review
{diff_command}

# Changed file list
git diff --name-only HEAD  # or HEAD~N..HEAD

# Recent commit context
git log --oneline -10
```

If there are **no changes** (empty diff), stop and report: "Nothing to review — no changes detected."

Classify changed files into: source (`src/`), test (`test/`), docs (`context/descriptions/`), config (`.claude/`, `Makefile`), grammar (`third_party/`).

Read the relevant documentation based on which source directories are touched:
- `src/optimizer/decide/` or `src/decidb/utility/ilp_model_builder.cpp` → read `context/descriptions/04_optimizer/rewrite_passes/done.md`
- `src/execution/operator/decide/` → read `context/descriptions/01_pipeline/03_execution.md` and sub-docs (`03a` through `03e`)
- `src/planner/expression_binder/` → read `context/descriptions/01_pipeline/02_binder.md`
- `src/decidb/symbolic/` → read `context/descriptions/01_pipeline/01_parser.md`
- `third_party/libpg_query/` → read `context/descriptions/01_pipeline/01_parser.md`
- Any header changes in `src/include/duckdb/decidb/` → read corresponding pipeline docs

Also always read:
- `context/descriptions/README.md` (doc index)
- `.claude/CLAUDE.md` (conventions)

### 3. Spawn 3 reviewer agents in parallel

Launch all three agents simultaneously using the Agent tool. Pass each one the full diff, changed file list, recent commits, and any relevant doc excerpts you gathered. If there is a `scope_hint`, include it.

Each reviewer MUST be instructed: **"Be a devil's advocate. Assume there are bugs. Your job is to prove the implementation wrong, find what was missed, and challenge every assumption. Do not be polite — be precise and critical. Never suggest that a failing test be flipped to make it pass — oracle mismatches are signals of real bugs."**

Each reviewer MUST produce output in this exact format:

```
## [Reviewer Name] Review

### CRITICAL (must fix before merge)
- [file:line] Issue description
  Evidence: [what you found in the code]
  Suggested fix: [concrete suggestion]

### WARNING (should fix, risk if ignored)
- [file:line] Issue description
  Evidence: [what you found]

### INFO (observations, minor suggestions)
- [file:line] Observation

### Questions for Author
- [Q1] ...
```

If a section has no findings, write "None." Do not omit the section.

---

**Agent 1 — Formulation Reviewer**

Prompt:

> You are an ILP formulation expert reviewing DeciDB changes. Your sole focus is **mathematical correctness and solver quality**.
>
> You MUST check:
> 1. **Big-M constants** — Is every Big-M value tight? Could it be tighter? An unnecessarily large M weakens the LP relaxation and slows the solver. Read the optimizer code and trace how M is computed.
> 2. **Rewrite correctness** — For any new or modified algebraic rewrite (AVG→SUM, ABS linearization, MIN/MAX classification, `<>` indicators), is the reformulation mathematically equivalent? Construct a counterexample if you suspect it is not.
> 3. **Easy/hard classification** — Are MIN/MAX cases classified correctly per the documentation? An easy case handled as hard wastes variables. A hard case handled as easy produces wrong results.
> 4. **Solver-agnostic compliance (DeciDB CLI)** — DeciDB's CLI dispatch is Gurobi-preferred with HiGHS as a fallback. Does the change work for BOTH Gurobi (C API, `src/decidb/gurobi/gurobi_solver.cpp`) and HiGHS (C++ API, `src/decidb/naive/deterministic_naive.cpp`)? Check that no solver-specific assumptions leak through. If the change is Gurobi-only by necessity (non-convex QP, quadratic constraints, bilinear non-Boolean), confirm HiGHS rejects it with a clear error rather than producing a wrong answer. Note that the test/decide oracle is Gurobi-only by design (`02_operations/oracle.md`); that's separate from CLI compliance.
> 5. **Adversarial edge cases** — What happens with: zero rows, single row, all-zero coefficients, negative coefficients, infeasible problems, unbounded problems, WHEN filtering all rows out?
>
> Key files to read: `src/optimizer/decide/decide_optimizer.cpp`, `src/decidb/utility/ilp_model_builder.cpp`, `src/execution/operator/decide/physical_decide.cpp`, solver backends in `src/decidb/solver/`. Reference `context/descriptions/04_optimizer/` for expected behavior.
>
> **Do NOT comment on**: coding style, documentation sync, test coverage, or DuckDB conventions. Those are handled by other reviewers.
>
> [Include: diff, changed files, scope hint, relevant optimizer/formulation docs]

---

**Agent 2 — Architecture Reviewer**

Prompt:

> You are a DuckDB internals expert reviewing DeciDB changes. Your sole focus is **code quality, DuckDB pattern conformance, and performance**.
>
> You MUST check:
> 1. **DuckDB conventions** — CamelCase classes, snake_case methods, correct operator hierarchy, proper expression binding patterns. Find how DuckDB does the analogous thing and compare.
> 2. **Performance smells** — O(n^2) loops, unnecessary copies or materializations, constraint matrix size inflation, variable/constraint count growth that could be avoided. Think about what happens at 1M rows.
> 3. **Minimal core modifications** — Does this change touch DuckDB core files unnecessarily? Could the same result be achieved by extending rather than modifying?
> 4. **Memory safety** — Bounds checking, RAII compliance, no dangling references in the physical operator, correct lifecycle management in sink/source/finalize.
> 5. **Error handling** — Are invalid inputs caught with clear error messages? Do error paths clean up correctly?
>
> Key files to read: `src/execution/operator/decide/physical_decide.cpp`, `src/planner/expression_binder/decide_*.cpp`, `src/planner/binder/query_node/bind_select_node.cpp`, `src/planner/binder/query_node/plan_select_node.cpp`. Reference `context/descriptions/01_pipeline/` for the expected architecture.
>
> **Do NOT comment on**: ILP formulation correctness, Big-M values, documentation sync, or test coverage. Those are handled by other reviewers.
>
> [Include: diff, changed files, scope hint, relevant pipeline docs]

---

**Agent 3 — Completeness Reviewer**

Prompt:

> You are a documentation and test coverage expert reviewing DeciDB changes. Your sole focus is **keeping everything in sync and ensuring nothing was forgotten**.
>
> You MUST check:
> 1. **Doc drift** — For every behavioral change in `src/`, is there a corresponding update in `context/descriptions/`? Check `done.md` code pointers (line numbers, file references, tag constants) — are they still accurate after the change? Check `todo.md` — was a planned item just implemented but not removed from todo?
> 2. **CLAUDE.md sync** — Does the syntax reference in `.claude/CLAUDE.md` still match the actual behavior? Are the "Key DeciDB source paths" still correct? Are conventions still accurate?
> 3. **Test coverage gaps** — Are new code paths exercised by tests in `test/decide/tests/`? What edge cases are NOT tested? Specifically check: does the feature interact with WHEN, PER, QP, MIN/MAX, multiple variables? Are those interactions tested? **Oracle-verification checks**: every correctness test must use `comparison.compare.compare_solutions` against a gurobipy ILP built via `oracle_solver`. Flag any test that asserts hand-computed expected values (analytical/closed-form) — those are forbidden per `05_testing/README.md`. For discrete constructs (`<>`, IN) the oracle should use Gurobi native indicator constraints via `add_indicator_constraint` or helpers from `tests/_oracle_helpers.py`, not mirror DeciDB's Big-M rewrite. For quadratic objectives/constraints, the oracle should use `set_quadratic_objective` / `add_quadratic_constraint`, and non-linear objectives need `decidb_objective_fn` passed to `compare_solutions`.
> 4. **lessons.md** — Does this change introduce any gotcha that future developers should know about? Check `.claude/lessons.md` for relevance.
> 5. **Grammar/parser sync** — If `.y` files changed, were the generated files regenerated? Does the syntax reference doc reflect any new grammar?
>
> Key files to read: `context/descriptions/` (use README.md to navigate), `test/decide/tests/`, `.claude/CLAUDE.md`, `.claude/lessons.md`. Cross-reference against the diff to find gaps.
>
> **Do NOT comment on**: ILP formulation correctness, code architecture, performance, or DuckDB conventions. Those are handled by other reviewers.
>
> [Include: diff, changed files, scope hint, relevant docs]

---

### 4. Synthesize initial findings

Once all three reviewers return, merge their findings:

1. **Collect** all findings into a single list.
2. **Deduplicate**: if two reviewers flag the same `file:line`, merge into one finding noting both perspectives.
3. **Sort** by severity: CRITICAL first, then WARNING, then INFO.
4. **Identify cross-cutting themes** (e.g., Reviewer 1 found a formulation issue AND Reviewer 3 found the doc for that formulation is wrong — these are related).

### 5. Iterate if needed (max 2 rounds)

Review the "Questions for Author" sections from all reviewers. For each question:
- If you can answer it by reading code yourself, answer it internally — no need to iterate.
- If a CRITICAL finding lacks a concrete code reference (no specific file:line), use `SendMessage` to that reviewer asking them to pinpoint the exact location.
- If two reviewers disagree on whether something is an issue, use `SendMessage` to the lower-severity reviewer with the other's argument, asking for a rebuttal or concession.

Stop iterating when:
- All CRITICAL findings have concrete code references
- No open "Questions for Author" remain unanswered
- Or you've completed 2 rounds of follow-up

### 6. Produce final report

Present the consolidated report in this format:

```
# Devil's Advocate Review Report
**Scope**: [description] | **Commit**: [hash] | **Files Changed**: [count]

## Summary
[2-3 sentence executive summary of the review]

## Findings

### CRITICAL ([count])
| # | Issue | File | Reviewer | Suggested Fix |
|---|-------|------|----------|---------------|
| 1 | ...   | ...  | ...      | ...           |

### WARNING ([count])
| # | Issue | File | Reviewer | Suggested Fix |
|---|-------|------|----------|---------------|

### INFO ([count])
| # | Issue | File | Reviewer | Suggested Fix |
|---|-------|------|----------|---------------|

## Cross-Cutting Observations
[Themes that appeared across multiple reviewers]

## Unresolved (needs human judgment)
[Items reviewers could not resolve — include both sides]

## Recommended Next Steps
1. [Highest priority action — specific file path and what to change]
2. [Second priority]
3. ...
```

### 7. Present actionable items

After the report, list the concrete actions to take, ordered by priority. For each:
- The specific file and location to modify
- What the fix should look like
- Which reviewer raised it and why it matters

Ask the user which items they'd like to address.
