---
name: recap
description: ELI5 explainer for recent code changes - groups the current diff (or last N commits) by pipeline area and explains what changed and why in plain English. Use right after a plan or implementation, or when the user asks what just changed.
---

# /recap — ELI5 Explainer for Recent Code Changes

Explains what just changed in the codebase and why, in plain English. Designed to run right after a plan or implementation so you can understand the work without digging through diffs yourself.

## Arguments

`$ARGUMENTS` — optional:
- *(empty)* — explain all unstaged + staged changes (`git diff HEAD`)
- `last N` — explain the last N commits (e.g., `last 3`)
- any other text — focus the explanation on that area/topic within the changes

## Procedure

### 1. Parse arguments

Parse `$ARGUMENTS` into:
- **diff_command**: default `git diff HEAD`. If `last N`, use `git diff HEAD~N..HEAD`. Otherwise, keep default.
- **focus_hint**: if `$ARGUMENTS` is text (not `last N` and not empty), use it to prioritize which changes to explain in more detail.

### 2. Gather context

Run these commands:

```bash
# The diff
{diff_command}

# Changed file list
git diff --name-only HEAD  # or HEAD~N..HEAD

# Recent commit messages (for understanding motivation)
git log --oneline -10
```

If there are **no changes** (empty diff), stop and report: "Nothing to recap — no changes detected."

### 3. Read relevant docs

For each changed area, read the corresponding documentation to understand the feature context:
- `src/optimizer/decide/` → read `context/descriptions/01_pipeline/05_optimizer/done.md`
- `src/execution/operator/decide/` → read `context/descriptions/01_pipeline/08_execution/done.md`
- `src/planner/expression_binder/` → read `context/descriptions/01_pipeline/02_binder/done.md`
- `src/decidb/parsed/` or `third_party/libpg_query/` → read `context/descriptions/01_pipeline/01_parser/done.md`
- `src/planner/decide/` → read `context/descriptions/01_pipeline/04_canonicalizer/done.md`
- `src/planner/operator/decide/logical_decide.cpp` → read `context/descriptions/01_pipeline/03_logical_plan/done.md`
- `src/include/duckdb/decidb/` → read corresponding pipeline docs
- `test/decide/` → read test file(s) to understand what's being tested

### 4. Group changes by area

Classify every changed file into one of these groups:
- **Grammar/Parser** — `.y` files, `src_backend_parser_parser.cpp` (`base_yylex`), `decide_parse_hints.cpp`
- **Binder** — `decide_binder.cpp`, `decide_constraints_binder.cpp`, `decide_objective_binder.cpp`, `bind_select_node.cpp`, `plan_select_node.cpp`
- **Canonicalizer** — `decide_canonicalizer.cpp`, `decide_source_provenance.cpp`
- **Optimizer** — `decide_optimizer.cpp`
- **Model/Solver** — `ilp_model_builder.cpp`, `ilp_solver.cpp`
- **Execution** — `physical_decide.cpp`, solver backends (`src/decidb/gurobi/`, `src/decidb/naive/`, `src/decidb/{formulation,solver,diagnostics}/`)
- **Headers/Data Structures** — `.hpp` files
- **Tests** — `test/decide/`
- **Docs** — `context/descriptions/`, `CLAUDE.md`
- **Config/Build** — `Makefile`, `.claude/`, etc.

If there is a `focus_hint`, lead with the group most relevant to it.

### 5. Explain each group — ELI5 style

For each group that has changes, produce this structure:

```
### [Group Name]

**What changed**: [1-2 sentences, plain English. No jargon — or if jargon is unavoidable, define it in parentheses right there.]

**Why**: [The motivation. Why was this change needed? What problem does it solve or what feature does it enable? Infer from commit messages, related doc changes, and code comments.]

**Key snippet**:
```cpp
// The most important ~5-15 lines from the diff
some_code_here;           // ← what this line does, in plain English
more_code;                // ← why this matters
```

**How it connects**: [One sentence on how this group's changes relate to the other groups. e.g., "The parser change above is what lets the binder (below) recognize the new syntax."]
```

### 6. TL;DR

End with:

```
## TL;DR
[2-3 sentences summarizing the entire changeset. What was the goal? What's the end result? What should you know going forward?]
```

## Tone Rules — MANDATORY

These rules define how you write every word of the output:

1. **ELI5 first**. Explain like the reader is smart but unfamiliar with the internals. Use analogies when they help (e.g., "Big-M is like setting a speed limit so high nobody will ever hit it — it's only there to keep the math valid").
2. **No filler**. Never write "as you can see", "note that", "it's worth mentioning", "importantly", or similar padding.
3. **Define jargon inline**. If you must use a term like "linearization" or "Big-M", define it in parentheses the first time.
4. **Short code snippets**. Show only the essential lines (5-15), not entire functions. Annotate with `// ←` comments explaining what each important line does.
5. **Why > What**. The reader can see *what* changed in the diff. Your job is to explain *why* it changed and *what it means*.
6. **Concrete, not abstract**. Instead of "the constraint handling was improved", say "constraints like `SUM(x) <= 10 PER region` now get split into one constraint per region instead of one big constraint".
