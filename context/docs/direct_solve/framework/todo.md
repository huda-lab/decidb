# Shared engine — open work

The engine is everything that is not one problem class: IR, rule driver, plan fragments, runtime checks, bound reader,
result adapter, policy.
What exists today is in `done.md`. Direct solve declines each language construct it cannot yet model; the
solver path answers those queries, so none of them is wrong, only slower.

## How to work any task in these todo files

- Each task stands alone. Read its **Problem**, make the change and run the checks. When it ships, its result moves to
  the owning `done.md` and the task is deleted from its `todo.md`.
- The baseline to compare against is the stamp at the top of `done.md`. Any other failure after your change is yours.
- Checks that every code task runs (from the repository root): `make decide-test`,
  `DECIDB_TEST_DIRECT_SOLVE=off make decide-test`, `DECIDB_VERIFY_SERIALIZER=1 make decide-test`,
  `DECIDB_FORCE_SOLVER=highs make decide-test`,
  and `build/release/test/unittest "[decidb]"`. Rebuild first with `make release`. `test/decide/run_direct_matrix.sh` runs all
  five and prints one line each (about 3 minutes). Doc-only tasks need no checks.
- Do not loosen or delete a test to make it pass. If a test fails for a reason the task did not predict, stop and report it.
- Tasks marked **Decision needed** have two or more reasonable answers. Ask the owner before coding.
- The oracle tests cache solver results under the test's node id (`results/oracle_cache.json`). Do not rename a test
  function or move it to another file unless the task says so; a miss re-runs Gurobi, which needs a licence.
- The Bash tool is zsh. In `grep`, put `--include` flags before `-e PATTERN`; after a `--` they are read as file names.
- Stop and ask also when a golden changes that the task did not name, or a user-facing message needs wording the
  doc does not give. A task that turns out to need the owner records its question as **Decision needed**.
- A behavior change is judged by an oracle or the solver path, on both backends. A structural change gets a
  `decidb-architect` review.
- Never without the owner: large-tier benchmarks or other long CPU-bound runs, regenerating goldens before reading the
  diff, pushing, new branches or worktrees, `git stash`.

## H-06 — The two-level objective `outer(per k: inner(e) by (k))`

- **Problem.** The objective is `MINIMIZE sum(per id: max(w*x) by (id))`. `ReadObjective`
  (`direct_ir.cpp`) declines on `IsTwoLevelObjectiveWrapper`. The nested-reducer reading (`DirectPart::inner` in
  `ReadReducer`) exists, but no query reaches it: the binder rejects a nested reducer anywhere else.
- **Change.** Read the two-level objective into the IR through the nested-reducer reading.
- **Done when.** `test_decidb_direct_ir.cpp`, "declines a two-level objective", reads the inner reducer. The nested order-0 norm case returns: an inner not-proved part leaves the outer not proved.
- **Needs (owner, 2026-10-29).** A rule that takes the shape. Read into the IR with no rule taking it, the reading was
  reached only by an IR test, so it was removed; this task waits for A2–A4 (`../todo.md`).

## H-08 — VALUE-shape clauses (helper decisions)

- **Decision needed.** The rule and clause shape this is for. No rule's definition takes a VALUE-shape clause, so
  reading the helper into the IR alone reaches no code.
- **Problem.** `ReadConstraint` (`direct_ir.cpp`) declines a leaf tagged `VALUE_REWRITE_TAG`. The solver builds a
  helper decision from `reducer_value_links`.
- **Options.** 1. Leave it until a rule names the shape it needs; the task then becomes that rule's. 2. The owner names
  a rule and shape now.
- **Recommendation.** 1.
- **Change.** Read the helper decision and its definition into the IR, so a rule that applies can take the clause.
- **Done when.** A differential case with a VALUE-shape clause runs on both paths with the same answer.
