# Direct-solve: clear-everything queue (handover)

Untracked scratchpad. It orders every open item and says where each lives. The tracked records are the nested `todo.md`
files (what to do) and `done.md` files (what is true now). Delete this file when every box below is ticked.

## Start here (new agent)

- **State.** Branch `direct-solve-prototype`, HEAD `fa990cf941`, plus an **uncommitted docs cleanup** (9 files under
  `08_direct_solve/`: the dones now state only current facts, the todos only open work). Ask the owner whether to commit it
  before you start. The code is committed and green; the stamps in the three `done.md` files give the baseline
  (2,322 passed in auto, off and serializer; forced-HiGHS 2,321 plus one Gurobi failure; C++ 1,276 assertions in 44 cases).
- **Read first.** `.claude/CLAUDE.md`; `08_direct_solve/README.md`; `08_direct_solve/architecture/rules.md` (adding a rule);
  and "How to work any task" in `08_direct_solve/rules/_harness/todo.md`. Run all five checks with
  `test/decide/run_direct_matrix.sh` (about 3 minutes; it prints one line each).
- **How the owner works.** For each batch you draft a short brief (outcomes, decisions, constraints; not how-to), show it,
  and launch a subagent only after approval. Verify the report against the tree and re-run the tests yourself before you
  relay it. Explain decisions in plain language first, then ask. Do not commit unless asked; no `Co-Authored-By` trailer.
- **Rules.** Do not make design choices the owner has not made; surface them. Never loosen or flip a test to pass. Do not
  rename an oracle test or move it to another file (results are cached by node id; a miss needs a Gurobi licence). A
  `done.md` states only what is true now, no history. A `todo.md` holds only work still to do. When a task ships, move its
  result to the owning `done.md`, delete it from the todo, and tick it here.
- **Practical.** The Bash tool is zsh: use a bash script for loops and globs. Editor (clangd) errors are false positives;
  trust the CMake build (`make release`). Timing runs need a quiet machine. The owner turned off background worktree
  isolation locally, so work in place on the branch.

## Batches

| Batch | Contents | Needs |
|---|---|---|
| A. Decisions | Four owner decisions | Owner (minutes) |
| B. Quick wins | H-15; N-02 row 1 (if chosen); S1 no-objective | Nothing |
| C. Half-day widenings | S1 offset or negated count body; the other N-02 rows chosen | A done |
| D. About a day | S1-08; H-14 build (if option 2 or 3) | A done |
| E. Refresh the numbers | S1-13, A1-05 | B to D done; quiet machine; Gurobi licence |
| F. Needs a prerequisite | S1-12, H-03, H-02 | See each |
| G. New problem classes | 17 classes, one folder each | Owner picks the order |

## Batch A — Decisions (owner)

| # | Decision | Where | Recommendation |
|---|---|---|---|
| A1 | **H-14**: rows are held in memory until every row is checked. Keep holding, read twice for stored tables, or relax the error promise for A1. | `rules/_harness/todo.md` § H-14; measurements in `architecture/result_boundary.md` | Read twice, for stored tables |
| A2 | **N-02**: which of seven hand-written copies of DuckDB logic to replace. | `rules/_harness/todo.md` § N-02 | Row 1 only (the model builder's four strict shifts) |
| A3 | **Low-value widenings**: do or drop (a) bound expressions that can raise, such as a narrowing `CAST`; (b) a subquery or NULL factor on the objective; (c) a bound on `x` below 0 or above 1 without a `WHEN`. | `rules/s1/done.md` § Explicitly out of scope (no task exists yet) | Drop all three; (c) needs a solver change, not a direct-solve one |
| A4 | **S1-04** (wider keyed application): nothing is known to be missing. Delete or keep parked. | `rules/s1/todo.md` § S1-04 | Delete it |

## Batch B — Quick wins

| Task | What | Where | Size |
|---|---|---|---|
| [ ] H-15 | Three test files define the shared autouse fixture themselves; import it from `_direct_oracle_common.py` | `rules/_harness/todo.md` § H-15 | About 15 min |
| [ ] N-02 row 1 | Move `DecideStrictBound` into the numeric-policy header and call it from the model builder's four strict shifts | `rules/_harness/todo.md` § N-02 | Small |
| [ ] S1 with no objective | Accept constraints-only queries: treat every score as 0 so the plan picks only the rows a lower bound requires | `rules/s1/done.md` § Explicitly out of scope ("No objective"); write it into `rules/s1/todo.md` first | About 1 hour; follow the S1 exit gate |

## Batch C — Half-day widenings

| Task | What | Where | Size |
|---|---|---|---|
| [ ] S1 offset or negated count body | Admit `SUM(x + c)` and `SUM(1 - x)`: shift the per-group limit by `n*c` and flip the side when the coefficient is -1 | `rules/s1/done.md` § Explicitly out of scope ("`SUM(x + c)`"); write it into `rules/s1/todo.md` first | About 0.5 day; follow the S1 exit gate |
| [ ] N-02 other rows | Only the rows chosen in A2 (A1's `ProveBounds`, the four fold-to-DOUBLE helpers, the 2^53 message). The `CanThrow` rows change DuckDB code | `rules/_harness/todo.md` § N-02 | Not estimated |

## Batch D — About a day

| Task | What | Where | Size |
|---|---|---|---|
| [ ] S1-08 | Admit other aggregates on the right of a count bound (`AVG`, `SUM`, `MIN`, `MAX`, `COUNT(col)`). First check each reducer's NULL, empty-group and typing behavior on the solver | `rules/s1/todo.md` § S1-08 | About 1 day |
| [ ] H-14 build | Only if A1 chose read-twice or relax-the-promise: write the plan and tests into `rules/_harness/todo.md`, then build it. Read-twice adds a second plan shape in the result boundary | `rules/_harness/todo.md` § H-14 | Not estimated; larger than the rest |

## Batch E — Refresh the numbers (run last of the S1/A1/engine work)

Run after B to D, because the tables record the final code. Needs a quiet machine; the S1 sweep needs a Gurobi licence and
about 3 GiB.

| Task | What | Where | Size |
|---|---|---|---|
| [ ] A1-05 | Re-run the A1 sweep (two commands under "Reproduce") and rewrite the performance table and date | `rules/a1/todo.md` § A1-05 | About 15 min of runs |
| [ ] S1-13 | Re-run the S1 sweep (about 4.5 minutes), plus one run with and without NULLs in the score column to measure the NULL-message cost; rewrite the table and date | `rules/s1/todo.md` § S1-13 | About 45 min |
| [ ] Stamps | Re-run `run_direct_matrix.sh` and rewrite the stamp at the top of the three `done.md` files (harness, A1, S1) | `rules/_harness/done.md`, `rules/a1/done.md`, `rules/s1/done.md` | 5 min |

## Batch F — Cannot start until a prerequisite exists

| Task | Prerequisite | Where |
|---|---|---|
| [ ] S1-12 | A third rule that needs per-row bounds exists (A3, A5 or A6 from Batch G). Then merge S1's and A1's per-row bound code into one shared builder; about 1 day | `rules/s1/todo.md` § S1-12 |
| [ ] H-03 | Two rules prove the same query on a real workload, so cost can choose. Re-measure the cost estimate then | `rules/_harness/todo.md` § H-03 |
| [ ] H-02 | A stable language branch. Then a second adapter produces the same facts from the new syntax | `rules/_harness/todo.md` § H-02 |

## Batch G — New problem classes (17, none started)

Each folder under `08_direct_solve/rules/<class>/` has a `definition.md` (the class and its correctness argument) and a
`todo.md` whose only task is "break this class into pickable tasks". Follow `architecture/rules.md` (the add-a-rule
checklist and the standard tests). The catalogue status is from `08_direct_solve/README.md`.

| Group | Classes | Status | Note |
|---|---|---|---|
| Per-row rules (unblock S1-12) | [ ] `a3`, [ ] `a5`, [ ] `a6` | Exact | Suggested first: any one of them satisfies S1-12's prerequisite |
| Other A classes | [ ] `a2`, [ ] `a4` | A2 exact; A4 prototype | `a4/definition.md` says its numeric policy is not fully specified |
| Selection classes | [ ] `s2`, [ ] `s3`, [ ] `s4` | Exact | S2 also decides how independently scoped clauses relate to S1-04 |
| Resource allocation | [ ] `r1`, [ ] `r2`, [ ] `r3`, [ ] `r4` | R1, R2, R4 exact; R3 prototype | `r3/definition.md` says its numeric policy is not fully specified |
| Norm classes | [ ] `n1`, [ ] `n2` | Exact | |
| Assignment and transport | [ ] `o1`, [ ] `o2` | Prototype | |
| Keyed decomposition | [ ] `d1` | Exact | The facts already carry entity-scope data for it |

## Done when

Every box above is ticked or deleted by an owner decision in Batch A, the nested `todo.md` files hold only Batch F and G
items, and the three `done.md` stamps match a run. Then delete this file.
