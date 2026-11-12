"""Today's failures, by defect: each seed a fuzzer fails on at HEAD, under the id its defect was reported by. A later
run labels a failure recorded here, alike in module, seed, solver and category, with that id and counts it apart, so
only a new failure fails the run; a fix takes its defect's seeds out of here, and a run names a recorded seed that no
longer fails.

Recorded at 2188ae3f36 from seeds 1..4000 of nests, 1..1500 of bounds and 1..1000 of frames and order_keys, and from
seeds 1..600 of nests and 1..400 of the others under ``--solver highs``. ``solver`` is the one the run solved with
(``runner.solver_of``), forced or the CLI's own choice, which was Gurobi. D1-D5 are the defects the P0 fuzzers found
(2026-11-08), not entries of ``decisions.md``; a defect no seed of those ranges reaches is listed with none. So are the
Q7 review's older items the generators step aside from, each where it draws: lifting an exclusion is the time to
record the seeds it fails.
"""

from __future__ import annotations

#: id -> (what is wrong, {(module, solver, category): failing seeds}).
DEFECTS: dict[str, tuple[str, dict[tuple[str, str, str], tuple[int, ...]]]] = {
    "D3": (
        "AVG coefficients that sum to 6.999999999999999 in some row orders put a helper's upper bound one ulp below "
        "its lower bound, and the query is reported infeasible, while DIAGNOSE says feasible.",
        {},
    ),
    "Q7-ruling-equals": (
        "Needs a ruling: under `=` a column bound raises 'more than one value' on a group with no instance, where a "
        "frame bound says nothing. nests gives `=` a constant bound; bounds writes an `=` clause with no `per t.k:` "
        "and no `when c:` inside its reducer, over `by` keys no row holds a NULL in.",
        {},
    ),
}


def defect_of(module: str, seed: int, solver: str, category: str) -> str:
    """The id a failure is recorded under, or "" for a new one."""
    failing = (module, solver, category)
    return next((defect for defect, (_, seeds) in DEFECTS.items() if seed in seeds.get(failing, ())), "")
