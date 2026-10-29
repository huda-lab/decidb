---
name: phase
description: Run one phase of the work queue (context/docs/todo.md) in parallel - collect the owner's decisions up front, launch one agent per lane in its own git worktree, relay each agent's design questions to the owner, review and gate each lane, then combine the lanes onto the main branch one at a time with the owner's approval. Use when the user says /phase, "next phase", or wants several queue batches worked in parallel.
---

# /phase — parallel lanes, one combine

You are the orchestrator, in the main tree. Each lane of the phase is one agent in its own git worktree, working one
queue batch (F1, F2, ...) start to finish. You collect decisions first, relay questions while lanes run, gate each
lane, and combine. `/orchestrate` is the one-lane form of this skill; its ground rules hold here except where this
skill says otherwise (worktrees, lane branches, commits at combine).

## Arguments

- *(empty)*: plan the next phase from the queue's lane line and show it to the owner.
- a list of batches (`F1 F2 F6 F7`): plan that phase.
- `status`: one line per running lane (working, waiting on the owner, ready for review, landed).
- `combine`: start step 6 for the lanes that are ready.

## 0. Plan the phase

Read `context/docs/todo.md` in full: the lane line at the top says which batches may run side by side, and each batch
line says what it needs. A phase is a set of batches whose needs are met and which the lane line allows together (for
example F4 never beside F2 or F5; F9 alone). Show the owner the phase: the batches, their tasks in order, and what is
left for later phases.

## 1. Decisions first

Every owner decision the phase's tasks need is asked before any lane starts: the queue's "Owner decisions" group
lists them per batch, and each task's own `todo.md` entry holds the options and the recommendation. Explain each in
plain language with your recommendation (owner rules: no option menus, mechanism first). Ask them all in one sitting.
Record each answer in the task's own doc as a dated **Ruling (owner, date).** line; drop it from the decisions group.

Tasks whose doc asks for an architect review get one now: run the read-only `decidb-architect` reviews in parallel,
bring any open choice to the owner, and write the reviewed design into the task.

## 2. Briefs

One brief per lane, in the `/orchestrate` shape (Outcome, Owner decisions already made, Stop and report, Constraints,
Exit gate, Records), covering the lane's tasks in queue order. Copy each task's text and ruling into the brief. Show
all the briefs together and wait for approval.

Add this block to every brief, verbatim:

```
Lane rules:
- You work in your own git worktree. First run `bash .claude/skills/phase/setup_worktree.sh` from its root. Every
  path you edit is inside this worktree; never edit the main tree.
- Your job is this brief. Fix another problem only when it blocks your job or your job cannot be done well without
  it; say so under FIXED ALONG THE WAY. Any other problem you notice: do not fix it, list it under FILED (what, where,
  how to reproduce) and the orchestrator files it.
- Any design choice the brief does not settle: stop and report it under DECISIONS HIT with the options and your
  recommendation. The orchestrator asks the owner and resumes you.
- Build with `CMAKE_BUILD_PARALLEL_LEVEL=3 make release` (other lanes build at the same time). Run only focused tests:
  `./test/decide/run_tests.sh -n 2 -k <pattern>`. Never run `make decide-test` or a full suite: the orchestrator runs
  full gates one lane at a time.
- Do not edit docs (todo.md, done.md, bugs.md, ...). Do not edit context/docs/todo.md under any circumstances.
- Commit on your worktree branch when a task is done: subject prefixed `decide:`, no trailer, never push.

Final report, at most 30 lines, these headings:
FILES / BEHAVIOR / DECISIONS HIT / TESTS / FIXED ALONG THE WAY / FILED / SKIPPED / DOUBTS
```

Then the `/orchestrate` working rules block (oracles, never loosen a test, never rename an oracle test, zsh, ranged
formatting), minus "Do not commit".

## 3. Launch

One `Agent` call per lane, in one message: `subagent_type: "general-purpose"`, `model: "sonnet"`,
`isolation: "worktree"`. Then `git worktree list`: every lane worktree must sit inside the repository. The owner
forbids hand-made folders beside the repo in `~/Code/`; if a worktree landed there, stop the lanes and ask. Never
create a worktree or branch by hand. Tell the owner which lanes are running, then end the turn.

Before the first phase on a machine: `ccache` must be installed (`brew install ccache`); the setup script points it at
the shared base directory so lanes reuse each other's objects. A cold build per worktree is about 3.4 GB on disk.

## 4. While lanes run

- A lane that stops with DECISIONS HIT: explain the choice to the owner in plain language with the agent's
  recommendation and your own, ask, and send the answer back with SendMessage to that lane. Other lanes keep going.
- Batch questions that arrive close together into one message to the owner.
- Never answer a design question yourself.

## 5. Review and gate each lane

When a lane reports done, review it as `/orchestrate` step 4 does, against its worktree: `git -C <wt> diff` from the
phase's base commit, the brief, the layer that owns the behavior. Then run its full gate in its worktree, one lane at
a time (never two full suites at once): `make decide-test`, plus the area suites `.claude/CLAUDE.md` names for what it
touched. Fixes go back to the same lane by SendMessage. A lane is ready when its gate passes and its report is
reviewed.

## 6. Combine

In the main tree, one lane at a time, in queue order:

1. Rebase the lane branch onto the main branch and fast-forward, so history stays linear and each task keeps its
   commit. Resolve conflicts yourself when they are mechanical; when they are not, ask the lane agent (it has the
   context) or the owner.
2. Build, run `make decide-test` and the area suites. A failure here that passed in the lane is an interaction
   between lanes: find which lanes, and bring it to the owner before fixing.
3. Golden diffs go to the owner before any regeneration.
4. Write the docs for the lane (done.md, task deletions, queue line deletions), as `/orchestrate` step 5 does.
5. File the lane's FILED items: each becomes a task in the owning nested `todo.md` with an id, and a queue line in the
   batch it belongs to (or a new batch). Only you edit the queue.
6. Ask the owner to approve the lane: its commits, the docs commit, the filed items. Land only after a yes.

After every lane has landed: remove the worktrees (`git worktree remove`) and their merged branches, report the phase
in plain language (what shipped, what was filed, what is next), and offer the next phase.
