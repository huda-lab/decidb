# /docs-review — Deep Documentation Accuracy Review

Performs a thorough audit of all documentation in `context/descriptions/` against the actual codebase. Verifies that every claim, code pointer, example, and behavioral description is accurate and up to date. This is NOT a surface-level check — it reads the code and compares it to what the docs say.

## Arguments

`$ARGUMENTS` — optional scope:
- *(empty)* — full audit of all documentation
- a directory name (e.g., `per`, `optimizer`, `pipeline`) — audit only that area
- `claude` — audit only `.claude/CLAUDE.md` syntax reference and source paths

## Procedure

### 1. Parse arguments and determine scope

Parse `$ARGUMENTS` into a scope:
- *(empty)* → audit everything: all `context/descriptions/` subdirectories + `.claude/CLAUDE.md` + `.claude/lessons.md`
- directory name → map to the relevant `context/descriptions/` subdirectory:
  - `decide`, `such_that`, `when`, `per`, `maximize_minimize`, `problem_types`, `sql_functions` → `03_expressivity/{name}/`
  - `optimizer`, `rewrite_passes`, `partition_solve`, `matrix_efficiency` → `04_optimizer/{name}/`
  - `pipeline`, `parser`, `binder`, `execution` → `01_pipeline/`
  - `operations`, `benchmarking` → `02_operations/`
- `claude` → `.claude/CLAUDE.md` only

### 2. Spawn parallel audit agents

Launch up to 3 agents in parallel, each responsible for a different audit dimension. Each agent receives the scope from step 1.

---

**Agent 1 — Code Pointer Verifier**

> You are auditing DecidB documentation for **stale or broken code pointers**. Your job is to verify that every file path, function name, line number, constant, tag, and data structure reference in the documentation still exists and is accurate in the current codebase.
>
> **What to check in each doc file**:
>
> 1. **File path references** — Does the file still exist at that path? Use Glob to verify.
> 2. **Function/method names** — Does the function still exist with that name? Grep for it. Check it's in the file the doc says it's in.
> 3. **Line number references** (e.g., `file.cpp:170-177`) — Read those lines. Does the code there still match what the doc describes? Line numbers drift constantly — flag any that are off.
> 4. **Constants and tags** (e.g., `PER_CONSTRAINT_TAG`, `AVG_REWRITE_TAG`, `BOOLEAN_P`) — Grep for each one. Does it still exist? Is it still used the way the doc describes?
> 5. **Data structure fields** (e.g., `LinearConstraint::per_columns`, `row_group_ids`) — Grep for the struct/class and verify the field exists with the described type and purpose.
> 6. **Enum values** (e.g., values in `decide.hpp`) — Read the enum and verify all documented values still exist.
> 7. **Grammar rules** — If the doc references grammar rules in `.y` files, read those lines and verify the rules still match.
>
> **Scope**: {scope_description}
>
> Read each doc file in scope, extract every code pointer, and verify it against the codebase. For each broken pointer, report:
> - The doc file and the claim it makes
> - What you actually found (or didn't find) in the code
> - A suggested correction
>
> **Output format**:
> ```
> ## Code Pointer Audit
>
> ### BROKEN (reference no longer valid)
> - [doc_file:line] Claims `function_name` is in `file.cpp` — actually renamed to `new_name` / moved to `other_file.cpp` / deleted
>
> ### DRIFTED (exists but details wrong)
> - [doc_file:line] Says lines 170-177 contain X — those lines now contain Y; X is now at lines 200-207
>
> ### VERIFIED (spot-checked, still accurate)
> - [doc_file] N code pointers checked, all valid
> ```

---

**Agent 2 — Content Accuracy Verifier**

> You are auditing DecidB documentation for **content that no longer matches the code's actual behavior**. Your job is to read what the docs claim the system does, then read the code and verify those claims are true.
>
> **What to check in each doc file**:
>
> 1. **Behavioral claims** — The doc says "AVG is rewritten to SUM at optimizer time" — read the optimizer and verify. The doc says "BOOLEAN variables get bounds [0, 1]" — read the model builder and verify. Every factual statement about behavior must be checked.
> 2. **Syntax claims** — The doc says certain SQL syntax is valid or invalid. Cross-reference with the grammar (`.y` files), the parser (`decide_symbolic.cpp`), and the binder (`decide_*_binder.cpp`). Flag any syntax the doc says works but the code rejects, or vice versa.
> 3. **Restriction/limitation claims** — The doc says "X is not supported" or "X is rejected with an error". Verify in the code that the restriction still exists and the error message matches.
> 4. **Rewrite descriptions** — For each algebraic rewrite described (AVG→SUM, ABS linearization, MIN/MAX, `<>` indicators), read the actual rewrite function and verify the doc's description matches the implementation.
> 5. **Error message accuracy** — If the doc quotes an error message, grep for it in the code and verify it's still the same text.
> 6. **CLAUDE.md syntax reference** — The quick reference in `.claude/CLAUDE.md` is a condensed spec. Verify every bullet point against the actual parser/binder/optimizer behavior. This is critical because CLAUDE.md is what guides all future development.
>
> **Scope**: {scope_description}
>
> For each inaccuracy, report:
> - The doc file and the specific claim
> - What the code actually does (with file path and evidence)
> - Whether this is a doc bug (doc is wrong) or a code bug (code diverged from intended behavior)
>
> **Output format**:
> ```
> ## Content Accuracy Audit
>
> ### INACCURATE (doc contradicts code)
> - [doc_file] Claims: "X happens when Y" — Code (`file.cpp:fn_name`): actually does Z
>   Likely: doc bug / code bug
>
> ### OUTDATED (doc describes old behavior)
> - [doc_file] Describes old approach to X — code now uses new approach since [commit/evidence]
>
> ### MISSING (code has behavior not documented)
> - Feature X exists in code (`file.cpp`) but is not mentioned in any doc
>
> ### VERIFIED (spot-checked, still accurate)
> - [doc_file] N claims checked, all valid
> ```

---

**Agent 3 — Completeness & Consistency Auditor**

> You are auditing DecidB documentation for **gaps, inconsistencies, and staleness**. Your job is to find things that are missing, contradictory between docs, or that have been implemented but not documented.
>
> **What to check**:
>
> 1. **todo.md vs code** — Read every `todo.md` file. For each planned item, check if it has already been implemented in the code. If so, it should be moved to `done.md`. Grep for keywords from the todo item to find implementations.
> 2. **done.md completeness** — For each feature area, are all implemented behaviors documented? Read the relevant source files and check if there are code paths, edge cases, or features not mentioned in done.md.
> 3. **Cross-doc consistency** — The same concept is often described in multiple places (e.g., PER appears in `03_expressivity/per/done.md`, `01_pipeline/03b_coefficient_evaluation.md`, and `.claude/CLAUDE.md`). Are these descriptions consistent with each other? Flag contradictions.
> 4. **CLAUDE.md key paths** — The "Key DecidB source paths" section lists important files. Are any paths stale? Are important new files missing from the list?
> 5. **lessons.md relevance** — Read `.claude/lessons.md`. Are any lessons now obsolete (the underlying issue was fixed)? Are there gotchas in the code that should be lessons but aren't?
> 6. **README.md navigation** — Does `context/descriptions/README.md` accurately describe what's in each directory? Are any new files or directories missing from the navigation?
> 7. **Example queries** — Do the SQL examples in the docs use valid syntax? Cross-check against the grammar and parser. Flag examples that would fail if actually run.
>
> **Scope**: {scope_description}
>
> **Output format**:
> ```
> ## Completeness & Consistency Audit
>
> ### IMPLEMENTED BUT NOT DOCUMENTED
> - Feature X is in code (`file.cpp`) but todo.md still lists it as planned / done.md doesn't mention it
>
> ### CONTRADICTIONS BETWEEN DOCS
> - [doc_file_1] says X, but [doc_file_2] says Y — which is correct?
>
> ### STALE TODO ITEMS
> - [todo.md] Item "X" appears to be implemented (found in `file.cpp`)
>
> ### STALE LESSONS
> - [lessons.md] Lesson about X — the underlying issue was fixed in `file.cpp`, lesson may be obsolete
>
> ### MISSING FROM NAVIGATION
> - File/directory X exists but is not listed in README.md
>
> ### INVALID EXAMPLES
> - [doc_file] SQL example would fail: [reason]
> ```

---

### 3. Synthesize findings

Once all three agents return, merge their findings:

1. **Collect** all findings into a single list
2. **Deduplicate**: if two agents flag the same issue from different angles, merge into one finding noting both perspectives
3. **Categorize** by severity:
   - **CRITICAL** — Doc actively misleads (wrong behavior, broken code pointer to nonexistent file, syntax claimed valid but actually rejected). These cause wrong decisions if someone relies on the doc.
   - **STALE** — Doc is outdated but not dangerously wrong (drifted line numbers, old approach description that's directionally correct, todo item already done). These cause confusion but not incorrect work.
   - **GAP** — Something is missing (undocumented feature, missing cross-reference, incomplete example). These cause blind spots.
   - **COSMETIC** — Minor inconsistency, formatting, or wording issues. Low priority.
4. **Group by doc file** so fixes can be applied file-by-file

### 4. Present the report

```
# Documentation Audit Report
**Scope**: [full / area name] | **Date**: [date] | **Docs Checked**: [count]

## Summary
[2-3 sentence executive summary: overall health, biggest problem areas, estimated fix effort]

## Critical Issues ([count])
These docs actively mislead — fix first.

| # | Doc File | Issue | Evidence | Fix |
|---|----------|-------|----------|-----|
| 1 | ...      | ...   | ...      | ... |

## Stale Content ([count])
Outdated but not dangerous — fix when touching these files.

| # | Doc File | Issue | Evidence | Fix |
|---|----------|-------|----------|-----|

## Gaps ([count])
Missing documentation — add when time permits.

| # | Area | What's Missing | Where It Should Go |
|---|------|----------------|--------------------|

## Cosmetic ([count])
Minor issues — fix opportunistically.

| # | Doc File | Issue |
|---|----------|-------|

## Cross-Cutting Observations
[Patterns across the audit — e.g., "line numbers are stale throughout 03_expressivity/" or "optimizer rewrites are well-documented but execution stage docs lag behind"]

## Recommended Fix Order
1. [Highest priority — specific file and what to fix]
2. [Second priority]
3. ...
```

### 5. Offer to fix

After presenting the report, ask: "Want me to fix these? I can tackle them by priority — critical first, then stale, then gaps."

If the user says yes, work through fixes in the recommended order, updating one doc file at a time and marking each issue as resolved.
