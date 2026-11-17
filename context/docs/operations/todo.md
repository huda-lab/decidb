# Operations — open work

Upkeep of the repository as a whole: releases, packaging and the docs across areas.

## OPS-3 — The OPA handoff and merge notes describe the branch before the merge

- **Problem.** `context/opa_rewrite/handoff.md` lines 9-17 say the merge comes next. Its A7 items 3 and 9 are listed
  open, and 4f4cb0acfc and 00ab2c5a5c settle them. `merge_notes.md` marks rough the rows for D-3, the HiGHS plan label,
  the session timeout and `DeriveReducerValueBounds`, which are settled, and calls R4 "still open" in two places. Both
  cite a test baseline other than the branch's (8,884 tests and 115 C++ cases at 052b85d26d), and their line numbers
  into `physical_decide.cpp`, `linearization_reducer_value.cpp` and `decide_tree_compile.cpp` drift.
- **Change.** Each item states the branch as it stands; code is cited by function name, not line.
- **Done when.** Every item above matches the branch at the commit that lands this task.

## OPS-4 — Docs name code that does not exist

- **Problem.** About 35 backticked identifiers in `pipeline/05_optimizer/done.md`, `pipeline/08_execution/done.md`,
  `pipeline/code_structure.md` and `performance/findings.md` name nothing in `src`, for example `DecidePreparedModel`,
  `install_row_checks`, `LiftNestedReducers`, `DefineNonlinearReducerBodies`, `EvaluateProductGroups` and
  `decide_rewrite_define.cpp`. `pipeline/02_binder/done.md` gives `DecideVarScope` as `ROW`, `ENTITY`, `SCALAR`; the
  enum is `ROW`, `KEYED`, `GLOBAL`, `FRAME_READ`, `INLINED`. `pipeline/03_logical_plan/done.md` lists `decide_sense` on
  the operator; the field is `written_sense`.
- **Change.** Grep each backticked name in these files against `src`; each passage naming a missing one names what does
  that work, or goes.
- **Needs.** EXEC-13 (it rewrites `08_execution/done.md` lines 98-461).
- **Done when.** A grep of the backticked identifiers in these files finds each one in `src`, SQL and paths outside
  `src` aside.

## OPS-5 — errors.md and the guide quote text the system does not print

- **Problem.** `language/errors.md` line 12 quotes "DECIDE: an aggregate cannot be used here.", which nothing in `src`
  prints. The review also reports that its "does not support '(x > 1)'" example prints without the parentheses and that
  its `<>` with MIN/MAX message is cut short; not re-run here. `guide.md` § 10 quotes solver output digits that differ
  per machine, and the guide sections written since the merge use solver terms (Big-M, McCormick, envelope, LP) in a
  document for SQL users.
- **Change.** Each quoted message is what the binary prints for its example; § 10 quotes no machine-dependent digits.
  Solver detail moves to the stage docs; **Decision needed** for any solver term the guide keeps.
- **Done when.** Each quoted message matches the binary on its example, and the guide holds only the solver terms the
  owner keeps.

## OPS-6 — The website teaches spellings the language removed

- **Problem.** `context/website` (`documentation.html`, `examples/intermediate-per.html`,
  `examples/advanced-relief-allocation.html`, `examples/advanced-scheduling.html`,
  `examples/intermediate-assignment.html`) teaches a postfix `PER`, `scalar x(INT)` and `T.x(INT)`, which the language
  no longer reads. The site predates the merge.
- **Decision needed.** Ask the owner before touching the site: bring it up to the guide, or retire it.
- **Done when.** The owner's ruling is recorded and carried out; every query the site keeps runs on the binary.

## OPS-7 — Docs cite ids that no file defines

- **Problem.** FRM-20 and FRM-23 (`direct_solve/framework/todo.md`), PERF-16 (`direct_solve/framework/done.md`,
  `performance/findings.md`) and TEST-2 (`direct_solve/rules/n1_euclidean_ball/done.md`) name closed tasks whose text is
  gone; `git log -S` finds each. D12 (`pipeline/08_execution/done.md` and `todo.md`) is defined in
  `context/opa_rewrite/review/09_user_facing_behaviour.md` § D12, which the citations do not name.
- **Change.** Each citation states what it stands for, or names the file that defines it.
- **Done when.** `git grep -w` of each id under `context/docs` finds no bare citation.
