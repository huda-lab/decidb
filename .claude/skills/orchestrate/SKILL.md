---
name: orchestrate
description: Work the scratchpad queue (context/docs/todo.md), whatever it currently holds, as an orchestrator - draft a brief per item, launch a Sonnet subagent after owner approval, review its diff and re-run the tests, update the docs yourself, then commit each item with the owner's permission. Use when the user says /orchestrate, "next task", or wants a queue item delegated to Sonnet.
---

# /orchestrate — Opus orchestrates, Sonnet builds

You (Opus) do not implement queue items yourself. You write the brief, Sonnet does the work in its own context, and you
review the result against the tree. Keep your own context small: read the diff and summary test lines, not build logs
or exploration.

The queue is `context/docs/todo.md`, a scratchpad tracked in git. Its contents change from project to project,
so nothing below assumes a particular feature area. Everything specific to the area (state, reading list, owner rules,
checks, where results are recorded) comes from the queue itself and the docs it points to.

## Arguments

`$ARGUMENTS`, optional:
- *(empty)*: read the queue, name the next open item whose prerequisites are met, and draft its brief.
- an item name or id as the queue writes it: draft the brief for that item.
- `review`: review the working-tree changes from the last agent (step 4 onwards).
- `status`: list the open items per group in a few lines; launch nothing.

## 0. Orient (once per session, or when the queue changes)

Read `context/docs/todo.md` in full. Extract and keep:
- **Reading list**: the docs a new agent must read before working (often a "Start here" or "Read first" section).
- **Owner rules**: how the owner wants work done and any hard rules the queue states.
- **Checks**: the commands that prove the tree is green (a matrix script, a suite, focused tests) and the baseline they
  should match.
- **Records**: each queue line points to a task in a nested `todo.md`; its area's `done.md` records the result.
- **Kinds of item**: which items are owner decisions, which are timing/measurement runs, which have prerequisites.

If the queue does not name checks, use the ones `.claude/CLAUDE.md` gives for the area touched (for example
`make decide-test`, the forced-HiGHS run for solver changes, `DECIDB_VERIFY_SERIALIZER=1` for serialization).

## Ground rules

- **Several batches at once go through `/phase`**, which gives each lane its own worktree. Within this skill:
- **One code item at a time.** Agents share one working tree and one `build/release`; parallel builds or test runs
  corrupt each other's results. Only read-only work (scoping, a requirements or design review) may run alongside.
- **No unilateral design choices**, by you or by Sonnet. Any choice the owner has not made goes in the brief as a
  stop-and-report item, and you explain it to the owner in plain language before asking.
- **Owner decisions are never delegated.** Explain each one, give a recommendation, and ask.
- **Timing and measurement items** (benchmarks, sweeps, refreshing recorded numbers): estimate the cost first and run
  them yourself on a quiet machine. Never launch a long CPU-bound run without saying how long it will take.
- **Docs are yours, not Sonnet's.** Sonnet changes code and tests only. You write the docs after review (step 5),
  following `.claude/skills/make-queue/doc-format.md`.
- **Commit after each item, only with permission.** See step 5.

## Procedure

### 1. Draft the brief

Read the item's text in the queue and the section it points to in its owning docs. Show the owner a brief in this shape,
at most about 40 lines:

```
Item: <id> — <one line>
Outcome: <what is true when done, in behavior terms>
Owner decisions already made: <list, or "none needed">
Stop and report instead of choosing: <open choices you foresee>
Constraints: <files/layers in scope; what must not change>
Exit gate: <the exact checks that must pass, plus new regressions to add>
Records: <the todo task you will delete and the done.md section you will update>
```

Copy the item's text into the brief; do not just point at the queue file. Wait for approval or edits.

### 2. Launch

After approval, call the Agent tool with `model: "sonnet"`, `subagent_type: "general-purpose"`, no worktree isolation.
The prompt is the approved brief, then the reading list from step 0, then this fixed block, verbatim, then any extra
hard rules the queue states:

```
Working rules:
- Read .claude/CLAUDE.md and the reading list above before changing anything.
- Do not make a design choice the brief does not settle. Stop and report it with the options you see.
- Never loosen, flip or delete a test to make it pass. Report unexpected failures instead.
- Never rename an oracle test or move it to another file (results are cached by node id; a miss needs a Gurobi licence).
- Compare against independent solver oracles, never hand-computed answers.
- The Bash tool is zsh: use a bash script for loops and globs. clangd errors are false positives; trust `make release`.
- Do not edit docs (todo.md, done.md, README, architecture notes); the orchestrator writes them after review. If a doc
  is wrong or a doc fact matters, say so under SKIPPED / DOUBTS.
- Preserve unrelated working-tree edits. Do not commit. Run `git diff --check` at the end.
- Format only the lines you changed (ranged clang-format/black from the test venv), not whole files.

Final report, at most 30 lines, exactly these headings:
FILES: path — one-line purpose, per file
BEHAVIOR: what changed for a SQL user (or "none" for internal work)
DECISIONS HIT: choices you stopped on (or "none")
TESTS: command → pass/fail counts, for each command you ran
SKIPPED / DOUBTS: anything not done, not verified, or that you are unsure of
```

Tell the owner the agent is running, then end the turn. Never predict its result.

### 3. On the report

If DECISIONS HIT is not "none", explain each decision to the owner in plain language and ask. Pass the answer back with
SendMessage to the same agent (it keeps its own context).

### 4. Review against the tree, not the report

1. `git status --short` and `git diff --stat`: do the files match FILES? Is anything touched outside the brief?
2. Read the diff. Check it against the brief's outcome, the layer that owns the behavior, and the working rules. Any
   doc edit by the agent is a review finding.
3. Re-run the exit-gate checks yourself, piping to `tail` (or the script's summary) so only result lines enter context.
   For a change to the direct-solve code, also report: the lizard CCN before and after of each function the diff
   touches (`test/decide/.venv/bin/python -m lizard -l cpp <files>`), and from `make decide-coverage` run before and
   after the change (`COVERAGE_RUN_DIR=<dir>`), `coverage_report.py compare` (no arm lost but deleted ones) and the
   tests that reach each arm the change adds. A warm run takes about 4 minutes.
4. If problems: SendMessage the same agent with a numbered list of concrete fixes, then repeat step 4. If the second
   round still fails, stop and bring it to the owner.

### 5. Close and commit

1. Tell the owner, in plain language: the problem, what changed, test results (counts), anything skipped.
2. Update the docs yourself, from what the reviewed diff actually does (not from the agent's report), following
   `.claude/skills/make-queue/doc-format.md`:
   - delete the task from its nested `todo.md`;
   - edit the area's `done.md` so it states what the system does now, and why when a reason is known; edit the
     paragraph that covers the behavior rather than appending; no changelog wording;
   - delete the item's line from `context/docs/todo.md` (and the file, if it is now empty).
   Show the owner the doc diff.
3. Propose a commit: the exact file list (this item's code, tests and docs, including its queue edit) and a subject
   in the repo's style (`decide:`, `docs:`, ...). **Ask for permission and wait.** Commit only after a yes. No
   `Co-Authored-By` or other generated-by trailer.
4. Offer the next item.
