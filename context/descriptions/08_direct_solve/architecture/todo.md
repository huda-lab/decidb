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
- P-05: keep the `Direct` prefix in `namespace duckdb`; drop it inside `direct_s1` and `direct_a1`.
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

## P-08 — Rule interface: one Prove, a summary instead of Explain

- **Problem.** Decision P-04 in § Decisions.
- **Change.** Apply decision P-04 to `DirectSolveRule`, `s1_rule.cpp`, `a1_rule.cpp`, the stub rules in
  `test_decidb_direct_coordinator.cpp`, and the lifecycle text in `rules.md` and `overview.md`. A Prove miss keeps its
  `require` reason text. `EXPLAIN` output is unchanged.
- **Done when.** `DirectRuleMatch` (if merged) has no references; the coordinator tests cover hit, miss and two proofs;
  the checks give the baseline counts.

## P-09 — Rule-framework renames

- **Problem.** Decision P-03 in § Decisions.
- **Change.** Rename per the glossary: `DirectRuleProof`, `DirectRelationalProposal`, `direct_coordinator.cpp` and
  `TryDirectSolve`, `DirectRuleOverride` and `DIRECT_RULE_OVERRIDE_KEY`, the `*Rewriter` classes, and "hit"/"miss" in
  `overview.md`, `rules.md`, `policy.md` and the code comments.
- **Needs.** P-08 (both edit the rule interface).
- **Done when.** None of the retired identifiers appears under `src/` or `test/`; the checks give the baseline counts.

## P-10 — Validation barrier becomes a pipeline breaker

- **Problem.** "Barrier" is our word for a blocking operator that reads every row before it emits one.
- **Change.** Rename per the glossary: `DirectValidationBarrier`, `DirectBarrier` and its `barrier` field,
  `validation_slots`, `streams_rows` (the standard opposite of blocking is pipelined), and the "Validation barrier"
  section of `result_boundary.md`.
- **Done when.** `grep -rn -i barrier src/optimizer/decide/direct src/include/duckdb/optimizer/decide/direct` finds
  nothing; the checks give the baseline counts.

## P-11 — Second read becomes late materialization

- **Problem.** The second read is late materialization, the technique DuckDB's own `LATE_MATERIALIZATION` pass uses,
  under another name.
- **Change.** Rename per the glossary: `DirectSecondRead`, `second_read` fields and variables,
  `direct_second_read.{hpp,cpp}`, `DirectExposeStoredRowId`, `ApplyDirectSecondRead`, and the "Second read" text in
  `result_boundary.md` and `../rules/_harness/done.md`. The name must stay distinct from DuckDB's pass (for example a
  `Direct` prefix), and the docs keep the reason it runs inside `UNUSED_COLUMNS`.
- **Done when.** `grep -rn -i "second.read" src test context/descriptions/08_direct_solve` finds nothing; the checks
  give the baseline counts.

## P-12 — Result boundary becomes the result adapter

- **Problem.** The boundary is a GoF Adapter (the substitute presents the DECIDE node's bindings) and an optimization
  fence (parent filters and limits cannot cross it).
- **Change.** Rename per the glossary: `LogicalDirectSolveResult`, `MapDirectResult`, `DirectResultExtension`,
  `MakeDirectResultExtension`, `direct_result_boundary.{hpp,cpp}`, `output_slots` ("slot map"), `prunable_*`, and
  `architecture/result_boundary.md` (file name and text). `DIRECT_RESULT_EXTENSION` and the extension's serialized name
  stay, so a serialized plan still reads back.
- **Needs.** after P-10 and P-11, which edit the same files.
- **Done when.** None of the retired identifiers appears under `src/` or `test/`; `DECIDB_VERIFY_SERIALIZER=1 make
  decide-test` gives the baseline counts.

## P-13 — Decision record becomes the EXPLAIN annotation

- **Problem.** "Decision record" usually means an architecture decision record.
- **Change.** Rename per the glossary: `DirectSolveDecisionRecord`, its `Render`, `record` fields, and the "decision
  record" text in `policy.md`, `overview.md` and `../rules/_harness/done.md`. The rendered `EXPLAIN` keys stay.
- **Needs.** after P-12, which edits the same files.
- **Done when.** `grep -rn DecisionRecord src test` finds nothing; `test_serializer_explain_and_model_dump` passes.

## P-14 — Builders, `Calc` and rule-local helpers

- **Problem.** "Builders" are free factory functions, not GoF Builders; `Calc`/`Ex` is an expression builder with a
  name that hides it; `s1_support` is a vague file name; rule-local helpers carry a double prefix
  (`direct_s1::DirectProjectScope`).
- **Change.** Rename per the glossary and P-05: `direct_builder.{hpp,cpp}` and its header comment, `Calc`, `Ex`,
  `s1_support.{hpp,cpp}`, and the prefix inside `direct_s1` and `direct_a1`. Update `rules.md` § Adding a rule and the
  "Shared builders" text in `../rules/_harness/done.md`.
- **Needs.** after P-10, which edits `direct_builder`.
- **Done when.** The new names compile under a unity build (`make release`); the checks give the baseline counts.

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

## P-21 — Problem classes take their textbook names

- **Problem.** Classes are known by id alone in code and folders (`s1_rule.cpp`, `S1Rewriter`, `direct_s1`,
  `rules/s1/`), and by home-made phrases in the docs. P-20 chose a textbook name per class.
- **Change.** Apply the glossary names everywhere, keeping the id as a prefix: `rules/<id>/` folders (`git mv`), each
  `definition.md` title, the `README.md` table, rule source files, types and namespaces, the rule's `EXPLAIN` value
  (`S1_CARDINALITY_INTERVAL`, `A1_*`; a user-facing change chosen by the owner) and the tests that assert it. The S1
  and A1 Python test files are renamed in P-18's single oracle re-run, not here. Classes without code are renamed in
  docs only.
- **Needs.** After P-09 and P-14 (rule-local renames in the same files), before P-15 and P-18.
- **Done when.** No class is named by its old phrase or by a bare id in a file or folder name; the checks give the
  baseline counts with the new `EXPLAIN` value.

## P-17 — Sweep for retired terms

- **Problem.** A rename task covers the files it knows of. Comments, other doc areas and `.claude/` skills may still
  use old terms.
- **Change.** For every retired term in `glossary.md`, grep `src/`, `test/`, `benchmark/decide/`, `context/` and
  `.claude/`, and fix what remains. Leave git history and the frozen paper alone.
- **Needs.** Every other task in this file.
- **Done when.** Each retired term greps to zero outside history; `test/decide/run_direct_matrix.sh` gives the baseline
  counts, and the stamp in the shared-engine `done.md` is renewed.
