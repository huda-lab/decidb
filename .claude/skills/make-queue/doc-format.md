# Doc format: queue, nested todo, done

Shared by `/make-queue` and `/orchestrate`. Each fact has one home. Anything written in two places goes stale in one.

| File | Holds | Never holds |
|---|---|---|
| Queue (`context/docs/todo.md`) | Order, grouping, dependencies, pointers | What a task is, sizes, state, numbers, rules, history |
| Nested `todo.md` | Open tasks, each pickable on its own | Finished work, status tables, an index of other files |
| Nested `done.md` | What the system does now, and why | History, "added", "changed", "now", "previously", task ids |

## Queue

```
# Work queue: <goal in under 10 words>

Scratchpad. Each line points to a task in a nested todo; details live there. Delete a line when its task ships; delete
this file when it is empty.

Read first: <2–4 paths>. Checks: <one command or path § section>.

## 1. <group name>
- [ ] <ID> <title, under 10 words> — <path from context/docs/> § <ID>

## 2. <group name> (needs: 1)
- [ ] <ID> <title> — <path> § <ID> (needs: <ID>)
```

- One line per task. No columns, sizes, estimates or prose under a heading.
- Groups run in order. `needs:` appears only when a task or group depends on another.
- Owner decisions form the first group when any exist.
- When a task ships, delete its line. Never tick and keep it.

## Nested todo.md

```
# <Area> — open work

<One or two sentences: what this area covers. Finished work is in done.md.>

## <ID> — <title>

- **Decision needed.** (only when the owner must choose; then Options and Recommendation are required)
- **Problem.** What is wrong or missing, in behavior terms, with the evidence.
- **Options.** (decisions only) Numbered, each with its cost.
- **Recommendation.** (decisions only)
- **Change.** What to build, at the level of outcome and constraints, not line-by-line.
- **Needs.** (only if blocked) What must exist first.
- **Done when.** The checks that pass and the regressions that exist.
```

- A task is pickable when one agent can finish it in one sitting (about a day at most) without asking anything but the
  listed decision. Split anything larger into tasks with their own ids.
- IDs are stable. Never renumber; new tasks take the next free number.
- How to work and the shared checks live once per area (one section in one todo file). Other files link to it.
- No status tables, no "next up", no list of other files' tasks. Order lives in the queue only.
- No finished items, no strikethrough, no "done" markers.

## Nested done.md

- Present tense, the current behavior: "S1 rejects X because Y." Never "S1 now rejects", "was changed to", "added in".
- Give the reason when one is known. Leave out a reason you would have to invent.
- Edit the existing paragraph that covers the behavior. Do not append a new section per task.
- A stamp (test counts and date) at the top states the last verified run, replaced, not appended.
- No task ids, no commit hashes, no names of who did what.

## Banned in all three

Changelog verbs (added, now, previously, renamed, moved, fixed), restating another file, filler ("note that",
"importantly", "it is worth"), em-dash chains, and anything only true while a task is in flight.
