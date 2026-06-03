# /learn — ELI5 Feature & Concept Explainer

Explains how a DeciDB feature or concept works — the mental model, not just the code. Use this when you want to understand something without reading through source files yourself.

## Arguments

`$ARGUMENTS` — **required**: the topic to explain. Can be:
- A keyword: `PER`, `WHEN`, `Big-M`, `QP`, `MIN/MAX`, `AVG`, `ABS`, `indicators`
- A pipeline stage: `parser`, `binder`, `optimizer`, `execution`, `solver`
- A concept: `linearization`, `easy vs hard`, `auxiliary variables`, `solver-agnostic`
- A broad area: `pipeline`, `how DECIDE works`, `constraint types`, `objective types`
- A file name or path: reads and explains that specific file

If `$ARGUMENTS` is empty, stop and say: "What do you want to learn about? Try something like `/learn PER`, `/learn Big-M`, or `/learn pipeline`."

## Procedure

### 1. Map topic to sources

Based on the topic, identify which files and docs to read:

**Keyword/feature topics** — map to `context/descriptions/03_expressivity/`:
- Look for a matching subdirectory (e.g., `per/`, `when/`, `problem_types/`)
- Read the `done.md` and `todo.md` there
- Also read the relevant source files listed in those docs

**Pipeline topics** — map to `context/descriptions/01_pipeline/`:
- `parser` → `01_parser.md` + `src/decidb/symbolic/decide_symbolic.cpp`
- `binder` → `02_binder.md` + `src/planner/expression_binder/decide_binder.cpp`
- `optimizer` → `context/descriptions/04_optimizer/` + `src/optimizer/decide/decide_optimizer.cpp`
- `execution` → `03_execution.md` (and sub-docs `03a`-`03e`) + `src/execution/operator/decide/physical_decide.cpp`
- `solver` → `03_execution.md` + solver backends in `src/decidb/solver/`
- `pipeline` (broad) → read `01_parser.md`, `02_binder.md`, `03_execution.md` for the full flow

**Concept topics** — read the optimizer docs and relevant source:
- `Big-M`, `linearization`, `indicators`, `easy vs hard`, `auxiliary variables` → `context/descriptions/04_optimizer/` + `src/optimizer/decide/decide_optimizer.cpp`

**File path topics** — if the argument looks like a file path, read that file directly and explain it.

Also always read:
- `context/descriptions/README.md` (for navigation context)
- `.claude/CLAUDE.md` syntax reference section (for quick reference)

### 2. Build the explanation — 4 sections

#### Section 1: The Idea

```
## The Idea

[2-3 sentences. Plain English. What is this thing and why does it exist?
Use an analogy if one fits naturally.
No jargon without inline definitions.]
```

The goal: after reading this, someone should be able to explain the concept to a friend in one sentence.

#### Section 2: How It Works

```
## How It Works

[Walk through the mechanism step by step. Start with a concrete example —
pick a simple DECIDE query that uses this feature, then trace what happens
to it through the relevant pipeline stages.

Show small code snippets (5-15 lines) from the ACTUAL codebase (not made-up code)
with `// ←` annotations. Only show the parts that matter for understanding.

Number the steps so the reader can follow the flow:
1. First, the parser sees X and does Y...
2. Then the binder takes that and...
3. The optimizer rewrites it to...]
```

#### Section 3: Example

```
## Example

```sql
-- A complete, minimal DECIDE query that uses this feature
SELECT ...
FROM ...
DECIDE ...
SUCH THAT ...
MINIMIZE/MAXIMIZE ...
```

**What this does**: [2-3 sentences explaining what this query asks for and what the expected result would be.]

**What happens under the hood**: [1-2 sentences on how the system processes this — connecting back to the "How It Works" section.]
```

#### Section 4: Where in the Code

```
## Where in the Code

If you want to go deeper, here's where to look:

- `src/path/to/file.cpp` — [one-line description of what this file does for this feature]
- `src/path/to/other.cpp` — [one-line description]
- `context/descriptions/path/to/doc.md` — [the doc that covers this in detail]
```

### 3. Handle follow-ups

If the user asks a follow-up question in the same conversation, answer it in the same ELI5 style — don't reset or re-read everything, just build on what was already explained.

## Tone Rules — MANDATORY

These rules define how you write every word of the output:

1. **ELI5 first**. Explain like the reader is smart but unfamiliar with the internals. Use analogies when they help (e.g., "PER is like a GROUP BY for constraints — instead of one rule for the whole table, you get one rule per group").
2. **No filler**. Never write "as you can see", "note that", "it's worth mentioning", "importantly", or similar padding.
3. **Define jargon inline**. If you must use a term like "linearization" or "Big-M", define it in parentheses the first time.
4. **Short code snippets**. Show only the essential lines (5-15), not entire functions. Annotate with `// ←` comments explaining what each important line does.
5. **Real code only**. Snippets must come from the actual codebase (with file path and line numbers), not invented examples. The SQL examples can be illustrative, but C++ must be real.
6. **Build mental models**. The goal isn't to document — it's to help the reader *understand*. Prefer "think of it like X" over "the implementation does X".
7. **Concrete, not abstract**. Instead of "PER enables per-group constraints", say "if you write `SUM(x) <= 100 PER region`, each region gets its own budget of 100 — the Northeast can't steal from the Southwest".
