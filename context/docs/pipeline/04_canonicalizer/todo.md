# Stage 04 — Canonicalizer: open work

Open work in the canonical forms and the stages that read them. Finished work is in [done.md](done.md); a rewrite
belongs in this stage only when it meets [done.md](done.md) § What a rule must be. How to work and the checks:
[`../../direct_solve/framework/todo.md`](../../direct_solve/framework/todo.md) § How to work any task in these todo
files.

## CANON-1 — A frame's span message spells one column two ways

- **Problem.** `per t.cost does not determine the timeline ... t."cost" = 2 spans t.product = 'A', 'B'.` quotes `cost`
  in the span and not in the subject (`context/opa_rewrite/review/09_user_facing_behaviour.md`, item S7). The subject
  is `per->text` (`decide_canonicalizer.cpp`, `partition_subject`); the span renders the key another way.
- **Change.** The span spells each column as the query wrote it, as the subject does.
- **Done when.** A test of that message pins one spelling; `./test/decide/run_tests.sh -n 2 -k "frames and span"`
  passes.
