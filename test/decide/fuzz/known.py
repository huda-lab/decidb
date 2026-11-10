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
    "D1": (
        "A bare MIN or MAX over a `by` key constrains a row whose key is NULL, which is in no group: over rows "
        "(1,'q'), (2,'q'), (3,NULL), `min(x) by (t.h) >= 1` MINIMIZE gives 1,1,1 where 1,1,0 is right. Seed 453 is "
        "`per t.k: min(2 * x) by (t.g, t.h) >= 2`, seed 3408 `min(per t.k: 2 * x) by (t.h) = 2`.",
        {("nests", "gurobi", "wrong-answer"): (453, 3408), ("nests", "highs", "wrong-answer"): (453,)},
    ),
    "D2": (
        "`<>` drops a whole group or class when one of its rows has a NULL bound: over rows (1,'w',0), (2,'w',NULL), "
        "`sum(x) by (t.k) <> t.b` MINIMIZE gives 0,0 and warns that it constrains nothing, where x1 + x2 <> 0 stands. "
        "In seed 3657 a row with no bound in each `by` group drops both groups.",
        {("nests", "gurobi", "wrong-answer"): (3657,)},
    ),
    "D3": (
        "AVG coefficients that sum to 6.999999999999999 in some row orders put a helper's upper bound one ulp below "
        "its lower bound, and the query is reported infeasible, while DIAGNOSE says feasible.",
        {},
    ),
    "D5": (
        "On HiGHS, 'Rewriting MIN/MAX requires a finite bound on x' for a keyed decision that `x = t.v` pins, once a "
        "row has no instance of it: `per t.p: x(REAL)`, `x = t.v AND max(x) >= 5` over a row with a NULL p refuses, "
        "frame or not, where Gurobi answers 10, 10. errors.md gives that refusal to an unbounded member; this one is "
        "bounded.",
        {("frames", "highs", "false-error"): (104, 124, 376)},
    ),
    "Q7-wrong-2": (
        "A MIN or MAX reads a row whose class key is NULL as a member: `min(per t.g: x) <= 0` over rows (1,'b'), "
        "(2,'a'), (3,NULL) gives 2,2,0. nests writes `min(per K: ...)` and `max(per K: ...)` only over a key K no "
        "row holds a NULL in.",
        {},
    ),
    "Q7-wrong-3": (
        "A clause `per` constrains a `by` group none of whose rows is in a class. nests gives such a clause no `by`, "
        "and bounds drops its `per`.",
        {},
    ),
    "Q7-internal-abs": (
        "`sum(abs(sum(abs(x))))` raises INTERNAL 'not linear algebra'. nests puts no `abs` inside an `abs`.",
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
