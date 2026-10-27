---
name: make-queue
description: Build or rebuild the scratchpad work queue (context/docs/todo.md) for a goal - organize the nested todo.md files into pickable tasks, then write a lean queue that only orders and points to them. Use when the user says /make-queue or asks to create, rebuild or refresh the work queue.
---

# /make-queue — Organize the nested todos, then write the queue

The queue orders work and points to it. All task detail lives in nested `todo.md` files. Read
`.claude/skills/make-queue/doc-format.md` first; it is the format for all three file kinds and the list of what is banned.

## Arguments

`$ARGUMENTS`:
- a goal and/or folders (`direct-solve: finish S1 and A1`, `direct_solve/rules/s1_uniform_matroid a1_separable_box`): build a queue for it.
- `refresh`: rebuild the existing queue against the nested todos as they are now (drop shipped lines, add new tasks).
- *(empty)*: ask what the goal is and which folders it covers. Do not guess.

If a queue already exists and the argument is not `refresh`, show its title and ask before replacing it.

## Procedure

### 1. Gather

Read every `todo.md` in scope and the `README.md`/`CLAUDE.md` of the area. Read a `done.md` only where a todo or the
owner points to open work recorded there (for example an "out of scope" list the owner wants done). Do not mine `src/`
for new problems; the queue is built from what the docs already say.

### 2. Plan the nested-todo changes, then ask

List, briefly, what you would change to make every task pickable:
- tasks to split (too big for one sitting) and the new ids;
- tasks to add (open work mentioned only in a done.md, README or the owner's words);
- content to remove from todos (status tables, indexes of other files, finished items, history);
- true-now facts found in a todo that belong in a done.md;
- decisions to mark **Decision needed** because the task has two or more reasonable answers;
- tasks that look obsolete (ask; never delete on your own judgment).

Show the list and wait for approval. Every judgment call goes in the list, not into the files silently.

### 3. Rewrite the nested todos

Apply the approved list in the nested-todo format. Keep ids stable. Move true-now facts to the owning `done.md` in the
done format. Keep the area's one "how to work" section.

### 4. Write the queue

Write `context/docs/todo.md` in the queue format. Group by dependency and by what can be batched (owner
decisions, quick tasks, larger tasks, measurement runs last, blocked tasks). "Read first" and "Checks" are pointers, not
copies.

### 5. Self-check before showing it

- Every queue pointer resolves: the file exists and contains a `## <ID>` heading.
- Every open task in scope appears in the queue exactly once.
- The queue has no prose beyond the header lines and no line over about 20 words.
- No banned content in any file you touched (grep for `now`, `added`, `previously`, `changed`, `was `, `Status`).
- `git diff --stat` shows only doc files.

Then show the owner the queue and a one-line summary per nested file changed. Do not commit; offer a `docs:` commit and
wait for permission. No `Co-Authored-By` trailer.
