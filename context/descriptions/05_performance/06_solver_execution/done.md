# Solver execution

Both backends are forced explicitly; no silent fallback is accepted. Gurobi `optimize` and HiGHS `run` are timed separately from loading/readback. Gurobi watcher join retains the existing monitor fix and has its own span.

Optional native logs expose versions, reductions and progress where available. Counters include iteration/node counts and objectives; Gurobi additionally reports bound/gap where available. Native solver internals are not all separately timed by these API-level spans.

Q5/Q7 require quadratic constraints and are rejected by the current forced-HiGHS contract before execution; they are capability exclusions. P2/P3 cover supported convex QP on both. The corrected 1,000-row pilot validated all 28 supported pairs, with two expected HiGHS refusals; it is preliminary, not the final baseline.

