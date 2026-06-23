# Query Diagnostics — The Router (how it works)

The unified router is **not built yet** — see `README.md` for the design and
`todo.md` for the work queue.

Today the dispatch the router will own is split across three places:

- **Status disambiguation** — `DisambiguateInfOrUnbd` in `ilp_solver.cpp` resolves
  the ambiguous "infeasible or unbounded" via a zero-objective feasibility probe
  (`foundations/done.md` — "Structured solver result" / "Solver behavior").
- **The diagnose gate** — `DiagnosisApplies(mode, status)` decides whether a failed
  status is diagnosed under `auto` (`foundations/done.md` — "the `diagnose_decide`
  pragma").
- **Engine selection** — `PhysicalDecide::Finalize` calls the matching state engine
  (today only unbounded) and otherwise falls through to `ThrowDecideSolveError`
  (`foundations/done.md` — "Diagnosis engine seam").

As the router lands (the tasks in `todo.md`), this doc replaces the bullets above
with the shipped dispatch behavior, terminal by terminal.
