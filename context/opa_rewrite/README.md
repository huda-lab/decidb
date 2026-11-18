# OPA-rewrites: what comes after the queue

The plan for the branch `OPA-rewrites` once the work queue (`context/docs/todo.md`) is empty: the cleanup a
whole-codebase review asked for, and the design changes the owner has planned.

## Where the branch stands

- `OPA-rewrites` is mainline (`direct-solve-prototype`, `7865506c62`) with `Sami-OPA` merged in (`39b4173fe9`), plus
  the loose ends the queue has closed.
- A query is answered by a direct-solve rule (plain DuckDB operators) or by the solver path, which evaluates the
  clauses on a tree of nodes and hands a model to Gurobi or HiGHS.
- The goal is the deck's one-pass algorithm on a tree the parser builds. The branch has four of its five steps, on a
  tree built last, at physical planning.
- A review of all DECIDE code (2026-11-18, at `38699104bc`) found the direction right and the parts that decide
  meaning sound, but the pipeline not yet simple. Three causes:
  1. **Old carriers.** Marker strings in expression names, one AND-tree for all clauses, linear-form records and
     arrays as long as the input still carry meaning beside the tree, so each fact has several homes.
  2. **Message machinery.** Beside the one mechanism that quotes the query by location sit an error-ranking system,
     a rewriter for removed syntax, stored copies of clause text and diagnosis state built on every query.
  3. **Two evaluators of a bound.** The solver path computes bound reducers and data checks in the tree; the direct
     path computes them as SQL side tables.

## What to do, in order

A recommendation; the owner decides the order.

| Step | Work | Where |
|---|---|---|
| 1 | Finish the queue | `context/docs/todo.md` |
| 2 | Get the rulings marked **first** | [decisions.md](decisions.md) |
| 3 | Fix the defects | [cleanup.md](cleanup.md), part 1 |
| 4 | Remove message machinery | [cleanup.md](cleanup.md), part 2 |
| 5 | Simplify structure that stands alone | [cleanup.md](cleanup.md), part 3 |
| 6 | Phase 2: the tree becomes the representation | [design.md](design.md), section 3 |
| 7 | Phase 3: write the model at the nodes | [design.md](design.md), section 4 |

Steps 3 to 5 delete code the design changes would otherwise carry through a rewrite. [cleanup.md](cleanup.md)
part 4 lists what to leave alone until Phases 2 and 3, which replace it.

## Files

| File | Holds |
|---|---|
| [cleanup.md](cleanup.md) | The review's findings as tasks |
| [design.md](design.md) | What the deck requires, the target architecture, Phases 2 and 3 |
| [decisions.md](decisions.md) | Rulings in force and decisions still open |
| `repro/` | A reproducing query file for each task that has one, and the deck's running example |
| `tools/ab_bench.py` | Two binaries on the scaling workloads: whole-process time minus solver time, and peak memory |

A task here is not on the queue yet. File it in the owning stage's `todo.md` under `context/docs` with that file's
next free id, then add its queue line (`/make-queue`). The ids `CU-nn` and `OD-nn` are local to this folder. Delete a
task or a decision here when it ships or is ruled.

Evidence labels: **[reproduced]** run on the binary at `38699104bc`, with the query in `repro/`; **[measured]** a script
counted or timed it; **[read]** a reviewer read it in the code; **[estimate]** a reviewer's guess, usually a line count.

## The gate for every change

```bash
make release
make decide-test                              # goldens, then the suite; about 1 minute
build/release/test/unittest "[decidb]"        # C++ tests
DECIDB_FORCE_SOLVER=highs make decide-test    # when model building changes
DECIDB_VERIFY_SERIALIZER=1 make decide-test   # when serialization changes
DECIDB_TEST_DIRECT_SOLVE=off make decide-test # when a direct-solve rule changes
cd test/decide && .venv/bin/python3 -m fuzz all --seed 1 --count 2000 --jobs 6
```

- Run each mode as its own command and read its exit status.
- Goldens stay identical unless the change is meant to move them; read the diff before regenerating.
- Last full run, at `052b85d26d` (2026-11-17): 8,884 passed in each of the four modes; 115 C++ cases; fuzzers on
  seeds 1 to 2000 clean (`nests` refuses 628 by design). Record a new baseline with the first cleanup commit.
- For a change to evaluation or model building, `tools/ab_bench.py` against the previous build: no workload moves
  outside about 10%.
- A fresh test environment needs `gurobipy==13.0.3`; version 14 is refused by the licence.

## Places

| What | Where |
|---|---|
| The deck | `context/New_syntax.pdf`, 119 pages, not in git. Pages 77 to 119 are the algorithm |
| The OPA parent of the merge, built | `.claude/worktrees/sami-opa` (`f56845f560`), local only; for comparing answers |
| Language reference | `context/docs/language/guide.md`. Where it and the binary disagree, trust the binary |
| Debug dumps | `SET decide_debug = 'tree,bodies,containers,values,steps'`; `DECIDB_DUMP_MODEL=<file>`; `DECIDB_PROFILE=1` |
| Earlier documents of this folder | `git show 5db8334431:context/opa_rewrite/handoff.md` (also `merge_notes.md`, `review/`) |
