# Stage 03 — Logical plan: open work

Open work in what `EXPLAIN` prints. Finished work is in [done.md](done.md) § 6. How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## PLAN-7 — Stages 03 and 04 run from one PlanDecide

- **Problem.** Stages 03 and 04 are orchestrated inline in `Binder::CreatePlan(BoundSelectNode &)`, about 240
  lines.
- **Change.** One `PlanDecide` function runs them; `CreatePlan` calls it.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "plan or canonical"` passes.

## PLAN-8 — The objective's canonical layer is compared as two renderings

- **Problem.** `objective_canonical_differs` (`plan_select_node.cpp`) is found by comparing the objective's text before
  and after canonicalization, while a clause's is reported by the canonicalizer. A rendering that changes without the
  objective changing would print a second layer.
- **Change.** `CanonicalizeObjective` reports "changed", as `CanonicalizeTree` does for a clause, and the comparison of
  two renderings goes.
- **Done when.** `./test/decide/run_tests.sh -n 2 -k "explain"` passes with no golden change.

## PLAN-9 — `source_columns` is built and serialized on every query

- **Problem.** `LogicalDecide::source_columns` is built and serialized for every query, and only `DIAGNOSE` reads it.
  `DIAGNOSE` is set after planning (`bind_showref.cpp`), so planning cannot tell whether to build it.
- **Change.** The columns are named where `DIAGNOSE` reads them, from the plan's column bindings, and the field and its
  serialized property go.
- **Needs.** EXEC-19 (it moves `NameInputColumns` into the unbounded engine).
- **Done when.** `DECIDB_VERIFY_SERIALIZER=1 ./test/decide/run_tests.sh -n 2 -k "unbounded or diagnose"` passes.

## PLAN-10 — Check names and comments say "message"

- **Problem.** A check no longer builds a message: `LogicalDecide::scope_checks` has a field `value` and
  `DecideGroupBound::invalid` holds a BOOLEAN check, and the comments in `decide_tree_terms.hpp` and
  `logical_decide.hpp` still say a check yields a message that is NULL on a valid row. `decide_tree_terms.hpp` is
  misformatted at HEAD.
- **Change.** The names and comments say what the BOOLEAN check does (it raises where its value is found), and the file
  is formatted.
- **Done when.** `make format-check` reports only `logical_operator.hpp`; `./test/decide/run_tests.sh -n 2 -k
  "bound_side_tables"` passes.
