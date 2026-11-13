# N1 (Euclidean ball) — open work

N1 keeps one sum of squares inside a limit by scaling the direction or the target onto the ball. The class is in
[definition.md](definition.md); what N1 does today, with its measurements, is in [done.md](done.md). How to work and the
checks are in `../../framework/todo.md` § How to work any task.

N1's class includes a factor that differs between rows, `SUM(POWER(SQRT(w) * x, 2)) <= B`, the catalogue's ellipsoid
support function: its plan has N1's operator tree (owner, 2026-11-13).

## N1-11 — Measure the weighted form against the hand plan and Gurobi

- **Problem.** No benchmark query has a factor per row.
- **Change.** `n1_weighted` and `n1_weighted_groups` over TPC-H `partsupp`, weighted by `ps_availqty` (1 to 9,999,
  never 0), with hand plans in `benchmark/decide/direct_solve/hand/`; their rows join `done.md` § Performance.
- **Done when.** `python3 benchmark/decide/direct_solve/run.py --compare --sf 1 --queries n1_weighted
  n1_weighted_groups`; `python3 benchmark/decide/direct_solve/run.py --sf 0.125 0.25 0.5 1 --queries n1_weighted
  n1_weighted_groups --modes direct gurobi`.
