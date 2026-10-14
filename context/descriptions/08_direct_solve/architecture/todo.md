# Architecture — open work: standard patterns and names

Direct solve's design mostly follows known patterns under names we made up. These tasks settle one standard name per
concept, apply it to code and docs, and remove the parts of the rule lifecycle that a standard design does not need.
How to work and the shared checks: `../rules/_harness/todo.md` § How to work any task (the folder name itself is P-02).

Rules for every rename task in this file:

- Use the names `glossary.md` fixes (P-06). Do not choose a new name inside a rename task.
- Text a user sees does not change: the `EXPLAIN` keys and lines, error messages, `require` reasons and the
  `decide_direct_solve` setting, unless P-01 decides otherwise.
- Do not rename a Python test function or move it to another file (the oracle cache is keyed on the node id). C++ test
  files and case names may change.
- A rename is behavior-neutral: the checks give the same counts as the stamp in `../rules/_harness/done.md`.
- Each rename updates every doc in `08_direct_solve/` that uses the old term, in the same commit.

## P-01 — Name of the feature and its setting

- **Decision needed.**
- **Problem.** "Direct solve" is our phrase. The standard ideas are a specialized (problem-specific) algorithm in
  operations research, and a rule-based plan rewrite in databases. The name appears in the setting
  `decide_direct_solve`, `EXPLAIN` keys (`Direct solve mode`), every `Direct*` identifier, file and doc folder.
- **Options.**
  1. Keep "direct solve" and define it once in the glossary as "rule-based rewrite of a DECIDE query into a relational
     plan that computes the optimum". Cost: none.
  2. Rename the concept in docs only (for example "specialized solve"), keep the setting and identifiers. Cost: docs
     and code disagree.
  3. Rename everywhere, including the setting. Cost: a user-facing break and every `Direct*` identifier.
- **Recommendation.** 1. The term is short and only needs a standard definition; show it to the supervisor.
- **Done when.** The choice is recorded in `glossary.md` (P-06).

## P-02 — Name of the shared-engine folder and the docs tree

- **Decision needed.**
- **Problem.** `rules/_harness/` holds the shared engine's todo and done. "Harness" means test harness in standard
  usage, and the folder sits among the problem classes it is not one of. The docs call each class a "class" and the code
  calls it a "rule"; the folder `rules/` holds classes.
- **Options.**
  1. `08_direct_solve/engine/` beside `architecture/` and `rules/`. Cost: update 4 linking files and the queue.
  2. `08_direct_solve/framework/` (the standard name for shared code that calls plug-ins). Same cost.
  3. Merge into `architecture/` (design and engine status in one folder). Cost: mixes design notes with todo/done.
- **Recommendation.** 2. "Framework" is the textbook name for code that drives Strategy plug-ins it does not know.
  Keep `rules/` and state in the glossary that one rule implements one catalogue class.
- **Done when.** The choice is recorded in `glossary.md` (P-06).

## P-03 — Rule-framework vocabulary

- **Decision needed.**
- **Problem.** The rule framework is a Cascades-style transformation-rule optimizer behind a Strategy interface, but
  uses its own words. Each row needs a pick.

  | Ours (code) | Standard candidate | Source |
  |---|---|---|
  | Match / Prove / Rewrite | pattern / condition / apply (substitute) | Volcano, Cascades |
  | proof (`DirectRuleProof`) | applicability condition; witness | Cascades; type-driven design |
  | proposal (`DirectRelationalProposal`) | substitute (plan) | Cascades |
  | coordinator (`direct_coordinator.cpp`, `TryDirectSolve`) | rule driver; rule engine | optimizer texts |
  | registry (`direct_registry.cpp`) | registry + factory method | Fowler; GoF |
  | hit / miss | rule applies / does not apply | optimizer texts |
  | `DirectRuleOverride` | test seam (dependency injection) | Feathers |
  | `S1Rewriter`, `A1Rewriter` | plan builder | GoF Builder |

  Two terms reach beyond direct solve and are out of this file's scope: "reducer" (55 files across the planner and
  solver; DuckDB says "aggregate") and "absorbed bound" (shared with the solver; operations research says "singleton
  row folded into a variable bound").
- **Options.** Per row: adopt the candidate, or keep ours with a glossary line naming the standard term.
- **Recommendation.** Adopt the Cascades lifecycle words, "substitute", "rule driver" and "test seam". Keep "proof":
  the rule must prove optimality, which is stronger than a Cascades condition; the glossary says so. Keep "reducer" and
  "absorbed" and list their standard names in the glossary.
- **Done when.** The choices are recorded in `glossary.md` (P-06).

## P-04 — Rule lifecycle: merge Match and Prove, fold Explain

- **Decision needed.**
- **Problem.** Cascades splits pattern from condition because a cheap pattern is shared across many candidate
  subtrees. Direct solve has one candidate (the DECIDE node), and the driver always runs Match and then Prove, treating a
  miss in either the same way. `A1Match` is an empty struct (`a1_rule.cpp`). Explain writes one fixed line per rule.
- **Options.**
  1. One `Prove(facts, context, reason)` that returns the proof; Explain becomes a `Summary()` on the rule. The rule
     interface has two methods (Prove, Rewrite) and one name. Cost: S1, A1, the stub rules and `rules.md` change.
  2. Merge Match and Prove only.
  3. Keep the four steps and say in the glossary why they differ from Cascades.
- **Recommendation.** 1. Nothing uses the split, and a smaller interface is easier to explain as a Strategy.
- **Done when.** The choice is recorded in `glossary.md` (P-06).

## P-05 — Code naming convention: `Direct` prefix or a namespace

- **Decision needed.**
- **Problem.** About 60 free functions and types carry a `Direct` prefix (`DirectColumn`, `DirectIsNull`,
  `DirectRowBox`) inside `namespace duckdb`. Rule-local helpers use `namespace direct_s1` and `direct_a1`, which also
  carry `Direct` names (`direct_s1::DirectProjectScope`).
- **Options.**
  1. Keep the prefix, the DuckDB convention for a feature inside `namespace duckdb`. Drop it inside `direct_s1` and
     `direct_a1`. Cost: small.
  2. Move everything into `namespace duckdb::direct` and drop the prefix. Cost: every file; unity builds need care.
- **Recommendation.** 1. It matches the surrounding code, and the rule-local double prefix is the only real oddity.
- **Done when.** The choice is recorded in `glossary.md` (P-06).

## P-06 — Glossary: one standard name per concept

- **Problem.** No page maps our terms to the standard ones, so every rename task would have to pick names itself.
- **Change.** Write `architecture/glossary.md`: one row per concept with the chosen name, the standard term and its
  source (GoF, Cascades, compiler texts, DuckDB), and the code identifiers it covers. Include the decisions of P-01 to
  P-05, plus the uncontested rows: facts → intermediate representation (IR); "unknown fails closed" → conservative
  (sound) analysis; validation barrier → pipeline breaker; second read → late materialization; result boundary →
  adapter with an optimization fence; decision record → `EXPLAIN` annotation (a "decision record" means an architecture
  decision record elsewhere); builders → plan factory functions; `Calc`/`Ex` → expression builder; pins → fixed
  variables; three-way table → differential oracle test; contract suite → conformance suite. Add a "Patterns used"
  section to `overview.md` that names Strategy, Factory Method, Template Method (the driver's fixed steps), Adapter, the
  IR, and the one departure: rules must be disjoint instead of chosen by cost, and why. Link the glossary from
  `README.md`.
- **Needs.** P-01 to P-05.
- **Done when.** Every term in the P-03 table and the list above has one row with exact old and new identifiers.

## P-07 — Facts layer becomes the IR

- **Problem.** "Facts" and "adapter" are our words for a compiler-style intermediate representation built by a front
  end.
- **Change.** Rename per the glossary: `DirectProblemFacts`, the `Direct*Fact` types, `DirectFactStatus` (and its
  `UNKNOWN`), `FactReader`, `direct_problem.cpp`, `FirstUnknownReason`, `test_decidb_direct_facts.cpp` and its tag,
  and `architecture/facts.md` (file name and text). Update H-02 in `../rules/_harness/todo.md`, which calls the second
  front end an adapter.
- **Needs.** P-06.
- **Done when.** `grep -rnw -e Facts -e FactReader src/optimizer/decide/direct src/include/duckdb/optimizer/decide/direct`
  finds nothing; the checks pass at the stamp's counts.

## P-08 — Rule interface: one Prove, a summary instead of Explain

- **Problem.** See P-04.
- **Change.** Apply the P-04 choice to `DirectSolveRule`, `s1_rule.cpp`, `a1_rule.cpp`, the stub rules in
  `test_decidb_direct_coordinator.cpp`, and the lifecycle text in `rules.md` and `overview.md`. A Prove miss keeps its
  `require` reason text. `EXPLAIN` output is unchanged.
- **Needs.** P-06.
- **Done when.** `DirectRuleMatch` (if merged) has no references; the coordinator tests cover hit, miss and two proofs;
  the checks pass at the stamp's counts.

## P-09 — Rule-framework renames

- **Problem.** See P-03.
- **Change.** Rename per the glossary: `DirectRuleProof`, `DirectRelationalProposal`, `direct_coordinator.cpp` and
  `TryDirectSolve`, `DirectRuleOverride` and `DIRECT_RULE_OVERRIDE_KEY`, the `*Rewriter` classes, and "hit"/"miss" in
  `overview.md`, `rules.md`, `policy.md` and the code comments.
- **Needs.** P-08 (both edit the rule interface).
- **Done when.** None of the retired identifiers appears under `src/` or `test/`; the checks pass at the stamp's counts.

## P-10 — Validation barrier becomes a pipeline breaker

- **Problem.** "Barrier" is our word for a blocking operator that reads every row before it emits one.
- **Change.** Rename per the glossary: `DirectValidationBarrier`, `DirectBarrier` and its `barrier` field,
  `validation_slots`, `streams_rows` (the standard opposite of blocking is pipelined), and the "Validation barrier"
  section of `result_boundary.md`.
- **Needs.** P-06.
- **Done when.** `grep -rn -i barrier src/optimizer/decide/direct src/include/duckdb/optimizer/decide/direct` finds
  nothing; the checks pass at the stamp's counts.

## P-11 — Second read becomes late materialization

- **Problem.** The second read is late materialization, the technique DuckDB's own `LATE_MATERIALIZATION` pass uses,
  under another name.
- **Change.** Rename per the glossary: `DirectSecondRead`, `second_read` fields and variables,
  `direct_second_read.{hpp,cpp}`, `DirectExposeStoredRowId`, `ApplyDirectSecondRead`, and the "Second read" text in
  `result_boundary.md` and `../rules/_harness/done.md`. The name must stay distinct from DuckDB's pass (for example a
  `Direct` prefix), and the docs keep the reason it runs inside `UNUSED_COLUMNS`.
- **Needs.** P-06.
- **Done when.** `grep -rn -i "second.read" src test context/descriptions/08_direct_solve` finds nothing; the checks
  pass at the stamp's counts.

## P-12 — Result boundary becomes the result adapter

- **Problem.** The boundary is a GoF Adapter (the substitute presents the DECIDE node's bindings) and an optimization
  fence (parent filters and limits cannot cross it).
- **Change.** Rename per the glossary: `LogicalDirectSolveResult`, `MapDirectResult`, `DirectResultExtension`,
  `MakeDirectResultExtension`, `direct_result_boundary.{hpp,cpp}`, `output_slots` ("slot map"), `prunable_*`, and
  `architecture/result_boundary.md` (file name and text). `DIRECT_RESULT_EXTENSION` and the extension's serialized name
  stay, so a serialized plan still reads back.
- **Needs.** P-06; after P-10 and P-11, which edit the same files.
- **Done when.** None of the retired identifiers appears under `src/` or `test/`; `DECIDB_VERIFY_SERIALIZER=1 make
  decide-test` passes at the stamp's counts.

## P-13 — Decision record becomes the EXPLAIN annotation

- **Problem.** "Decision record" usually means an architecture decision record.
- **Change.** Rename per the glossary: `DirectSolveDecisionRecord`, its `Render`, `record` fields, and the "decision
  record" text in `policy.md`, `overview.md` and `../rules/_harness/done.md`. The rendered `EXPLAIN` keys stay.
- **Needs.** P-06; after P-12, which edits the same files.
- **Done when.** `grep -rn DecisionRecord src test` finds nothing; `test_serializer_explain_and_model_dump` passes.

## P-14 — Builders, `Calc` and rule-local helpers

- **Problem.** "Builders" are free factory functions, not GoF Builders; `Calc`/`Ex` is an expression builder with a
  name that hides it; `s1_support` is a vague file name; rule-local helpers carry a double prefix
  (`direct_s1::DirectProjectScope`).
- **Change.** Rename per the glossary and P-05: `direct_builder.{hpp,cpp}` and its header comment, `Calc`, `Ex`,
  `s1_support.{hpp,cpp}`, and the prefix inside `direct_s1` and `direct_a1`. Update `rules.md` § Adding a rule and the
  "Shared builders" text in `../rules/_harness/done.md`.
- **Needs.** P-06; after P-10, which edits `direct_builder`.
- **Done when.** The new names compile under a unity build (`make release`); the checks pass at the stamp's counts.

## P-15 — Shared-engine folder rename

- **Problem.** See P-02.
- **Change.** Move `rules/_harness/` to the chosen name with `git mv`. Update every link: `README.md`, the todo files
  of S1, A1 and R1, this file, and `context/descriptions/todo.md`. Rename "harness" in prose where it means the shared
  engine.
- **Needs.** P-06. Run it alone: every other task in this file links to the folder.
- **Done when.** `grep -rn "_harness" context .claude` finds nothing; doc-only, no checks.

## P-16 — Test vocabulary: files renamed or not

- **Decision needed.**
- **Problem.** The docs call the test kinds "three-way table", "contract suite" and "matrix". The standard names are
  differential oracle testing and conformance suite ("test matrix" is standard). Renaming the Python files re-runs
  every oracle case through Gurobi, because the cache is keyed on the node id, which includes the file name.
- **Options.**
  1. Docs use the standard names; files keep theirs; the glossary lists the file for each. Cost: names differ.
  2. Rename the files too and re-run the oracle once. Cost: a Gurobi run of the whole suite.
- **Recommendation.** 1.
- **Done when.** The choice is recorded in `glossary.md` (P-06).

## P-18 — Test vocabulary in the docs

- **Problem.** See P-16.
- **Change.** Apply the P-16 choice to `rules.md` § The standard tests every rule ships and the "Verified by" text of
  `../rules/_harness/done.md`.
- **Needs.** P-06.
- **Done when.** Both files use the glossary names; doc-only unless P-16 chose option 2.

## P-17 — Sweep for retired terms

- **Problem.** A rename task covers the files it knows of. Comments, other doc areas and `.claude/` skills may still
  use old terms.
- **Change.** For every retired term in `glossary.md`, grep `src/`, `test/`, `benchmark/decide/`, `context/` and
  `.claude/`, and fix what remains. Leave git history and the frozen paper alone.
- **Needs.** Every other task in this file.
- **Done when.** Each retired term greps to zero outside history; `test/decide/run_direct_matrix.sh` gives the stamp's
  counts, and the stamp in the shared-engine `done.md` is renewed.
