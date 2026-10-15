# Architecture — open work: standard patterns and names

Direct solve's design mostly follows known patterns under names we made up. These tasks settle one standard name per
concept, apply it to code and docs, and remove the parts of the rule lifecycle that a standard design does not need.
How to work and the shared checks: `../rules/_harness/todo.md` § How to work any task (the folder name itself is P-02).
This folder has no `done.md`: a shipped task is recorded in the architecture note it names and in
`../rules/_harness/done.md`. A decision is recorded as one line in § Decisions.

Rules for every rename task in this file:

- Use the names `glossary.md` fixes. Do not choose a new name inside a rename task.
- Text a user sees follows the glossary names (`EXPLAIN` keys and values, `require` reasons, the syntax reference,
  the website), and the tests that assert it change with it. The `decide_direct_solve` setting and serialized names
  stay.
- Do not rename a Python test function or move it to another file (the oracle cache is keyed on the node id). C++ test
  files and case names may change.
- A rename is behavior-neutral: the checks give the same pass counts before and after the task. Run them
  first for the baseline; the stamp in `../rules/_harness/done.md` can lag behind later commits.
- The commit of a rename also updates every doc in `08_direct_solve/` that uses the old term.

## Decisions

Owner decisions every task below follows. The names they lead to are in `glossary.md`.

- P-01: keep "direct solve"; the glossary defines it as a rule-based rewrite of a DECIDE query into a relational
  plan that computes the optimum. The setting, `EXPLAIN` keys and `Direct*` identifiers stay.
- P-02: the shared-engine folder becomes `08_direct_solve/framework/`. `rules/` stays; one rule implements one
  catalogue class.
- P-03: adopt every standard candidate. Match/Prove/Rewrite become pattern/condition/apply; proof becomes
  applicability condition (`DirectRuleProof` takes that name); proposal becomes substitute; coordinator becomes rule
  driver; registry stays registry + factory method; hit/miss become applies/does not apply; `DirectRuleOverride`
  becomes a test seam; `*Rewriter` becomes plan builder. "Reducer" and "absorbed bound" are renamed too (P-19).
  The `EXPLAIN` key `Direct solve proof` becomes `Direct solve condition` (P-22 below).
- P-04: one `Prove` that returns the applicability condition, and `Summary()` on the rule in place of Explain. The
  interface has two steps, Prove and Rewrite.
- P-05: keep the `Direct` prefix in `namespace duckdb`; drop it inside a rule's namespace (`direct_s1_uniform_matroid`,
  `direct_a1_separable_box`).
- P-16: rename the test files to the standard names. Exception to the no-rename rule above, for P-18 only; the
  orchestrator measures the cost and runs the oracle re-run itself.
- P-20: each class keeps its id and takes the textbook name of its problem (not of its algorithm; the optimization
  term wins over the database term): A1 separable box-constrained problem; A2 single-facility location (1-D); A3
  multilinear optimization over a box; A4 bounded-variable least squares; A5 monotone optimization; A6 rank-one
  bounded least squares; N1 Euclidean-ball projection and support function; N2 ellipsoid support function; S1
  uniform-matroid optimization; S2 laminar-matroid optimization; S3 maximum-cardinality knapsack; S4
  cardinality-constrained bottleneck knapsack; R1 continuous (fractional) knapsack; R2 discrete separable convex
  resource allocation; R3 continuous quadratic knapsack; R4 weighted max-min fair allocation; O1 Monge assignment;
  O2 Monge transportation problem; D1 block-diagonal decomposition. The glossary gives each one's source and its
  folder, file, identifier and `EXPLAIN` slug.
- P-22: user-visible text follows every rename: `EXPLAIN` keys and values, `require` reasons, the syntax reference
  and the website. The setting `decide_direct_solve` and serialized names stay.

## P-15 — Shared-engine folder rename

- **Problem.** Decision P-02 in § Decisions.
- **Change.** Move `rules/_harness/` to the chosen name with `git mv`. Update every link: `README.md`, the todo files
  of S1, A1 and R1, this file, and `context/descriptions/todo.md`. Rename "harness" in prose where it means the shared
  engine.
- **Needs.** Run it alone: every other task in this file links to the folder.
- **Done when.** `grep -rn "_harness" context .claude` finds nothing; doc-only, no checks.

## P-18 — Test vocabulary in the docs

- **Problem.** Decision P-16 in § Decisions.
- **Change.** Apply decision P-16 to `rules.md` § The standard tests every rule ships and the "Verified by" text of
  `../rules/_harness/done.md`.
- **Done when.** Both files use the glossary names. P-16 chose to rename the files: `git mv` the direct-solve test
  files to their glossary names, then run the oracle once. The orchestrator estimates the Gurobi cost and runs it on a
  quiet machine. This is the one task allowed to rename a Python test file.

## P-19 — Rename "reducer" and "absorbed bound"

- **Problem.** P-03 adopted the standard names for both. They reach about 55 files across the planner and solver,
  outside direct solve.
- **Change.** Rename per the glossary: "reducer" to the DuckDB term "aggregate", and "absorbed bound" to the
  operations-research term for a singleton row folded into a variable bound. Cover identifiers, comments and docs
  outside `08_direct_solve/` too, the `require` reasons, `syntax_reference.md` and the website. A Python test
  function, parameter id or file keeps its name (the oracle cache is keyed on the node id); module constants may
  change. Check that the parser tag is never serialized before renaming it.
- **Needs.** Run it alone: it touches the planner and solver.
- **Done when.** Neither old term greps under `src/` or `test/`; `make decide-test` and the forced-HiGHS run give the
  baseline counts.

## P-17 — Sweep for retired terms

- **Problem.** A rename task covers the files it knows of. Comments, other doc areas and `.claude/` skills may still
  use old terms.
- **Change.** For every retired term in `glossary.md`, grep `src/`, `test/`, `benchmark/decide/`, `context/` and
  `.claude/`, and fix what remains. Leave git history and the frozen paper alone.
- **Allowed leftovers.** The serializer property name `"proof"` (id 209, `direct_result_adapter.cpp`), because
  serialized names stay (P-22); the field behind it is `condition`. Oracle test names keep old terms, because a
  test's name is its cache key: `test_a1_unused_wide_columns_stay_out_of_the_barrier...` and the four
  `test_second_read_*` tests in `test_direct_user_facing.py`. "Second read" in its everyday sense
  (`_diagnose_relation.py`, `test_decidb_diagnostic_engines.cpp`) is not the retired term.
- **Needs.** Every other task in this file.
- **Done when.** Each retired term greps to zero outside history; `test/decide/run_direct_matrix.sh` gives the baseline
  counts, and the stamp in the shared-engine `done.md` is renewed.
