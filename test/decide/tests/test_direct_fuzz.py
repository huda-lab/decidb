"""Seeded differential fuzz: random small S1 queries, run on the solver path (`off`) and the direct path (`require`).

Where `test_direct_three_way.py` lists the cases a person thought of, this finds the interactions nobody listed. The
comparator and its rules are in `_direct_differential.py`.
"""

import random
import pytest
from ._direct_differential import compare_direct_with_solver


@pytest.fixture(autouse=True)
def _allow_direct_path(monkeypatch):
    # The suite also runs with a process-wide forced HiGHS backend; this file
    # needs the direct path, and ``off`` runs use the default backend.
    monkeypatch.delenv("DECIDB_FORCE_SOLVER", raising=False)


_OBJECTIVES = {
    "SUM(score * x)": "sum(score * x)",
    "SUM(score * x) + SUM(id * x)": "sum(score * x) + sum(id * x)",
    "SUM(score * x) - SUM(cap * x)": "sum(score * x) - sum(coalesce(cap, 0) * x)",
    "-2 * SUM(score * x)": "-2 * sum(score * x)",
    "SUM(score * x) / 4 - 3 * SUM(id * x)": "sum(score * x) / 4 - 3 * sum(id * x)",
    "0.5 * SUM(score * x) + SUM(id * x) / -2": "0.5 * sum(score * x) + sum(id * x) / -2",
}


_NORM_REDUCERS = ["norm(x, 'inf')", "norm(x, 1)", "norm(x, 0)"]


_NORM_OBJECTIVES = {
    "SUM(score * x) - norm(score * x, 1)": "sum(score * x) - sum(abs(score * x))",
}


_DOMAIN_RESTATEMENTS = ["x <= 1", "x >= 0", "x < 2", "x > -1", "x <> 2", "x BETWEEN 0 AND 1"]


def _fuzz_rowbound(rng):
    """One per-row bound on x: a 0/1 pin, a restated domain, or any constant bound on one row."""
    kind = rng.choice(["pin", "pin", "restate", "constant", "column"])
    if kind == "restate":
        return rng.choice(_DOMAIN_RESTATEMENTS)
    if kind == "column":
        # The per-row `cap` column is NULL on some rows, which both paths must reject the same way.
        when = " WHEN flag" if rng.random() < 0.4 else ""
        return f"x {rng.choice(['<=', '<', '>=', '>', '=', '<>'])} cap{when}"
    row = rng.randint(0, 5)
    if kind == "pin":
        return f"x = {rng.choice([0, 1])} WHEN id = {row}"
    op = rng.choice(["<=", "<", ">=", ">", "=", "<>"])
    return f"x {op} {rng.choice(['-1', '-0.5', '0', '0.5', '1', '1.5', '2'])} WHEN id = {row}"


def _fuzz_query(rng):
    """Returns the query and whether it was built as an S1 shape rather than a near miss."""
    near_miss = rng.choice([None] * 8 + ["clause", "objective"])
    rows = []
    for i in range(rng.randint(1, 9)):
        g = rng.choice(["'a'", "'b'", "'c'", "NULL"])
        score = str(rng.randint(-6, 9)) if rng.random() < 0.5 else str(round(rng.uniform(-5, 9), 2))
        cap = rng.choice(["NULL"] + [str(rng.randint(0, 4))] * 25) if rng.random() < 0.9 else "2.5"
        rows.append(f"({i}, {g}, {score}, {cap}, {rng.choice(['true', 'true', 'false'])})")
    source = f"(VALUES {', '.join(rows)}) t(id, g, score, cap, flag)"
    per = "PER g" if rng.random() < 0.6 else ""
    style = rng.choice(["none", "top", "local"])
    operators = rng.choice([("<=",), (">=",), ("=",), ("<=", ">="), ("<", "<=")])
    clauses = []
    for index, op in enumerate(operators):
        bound = rng.choice(
            [str(rng.randint(0, 4)), f"{rng.randint(0, 3)}.5", "cap", "COALESCE(cap, 2)"]
            + ["COUNT(*) / 2", "COUNT(*) - 1", "COUNT(*) // 2"]
        )
        if op == "=" and ("cap" in bound or "COUNT" in bound):
            bound = str(rng.randint(0, 3))
        reducer = rng.choice(_NORM_REDUCERS) if near_miss == "clause" and index == 0 else "SUM(x)"
        aggregate = f"{reducer} WHEN flag" if style == "local" else reducer
        scope = " ".join(part for part in ("WHEN flag" if style == "top" else "", per) if part)
        clauses.append(f"{aggregate} {op} {bound} {scope}".strip())
    if rng.random() < 0.25:
        clauses.append(_fuzz_rowbound(rng))
    objectives = _NORM_OBJECTIVES if near_miss == "objective" else _OBJECTIVES
    objective = rng.choice(sorted(objectives))
    sense = rng.choice(["MAXIMIZE", "MINIMIZE"])
    sql = (
        f"WITH r AS (SELECT * FROM {source} DECIDE x(BOOL) SUCH THAT {' AND '.join(clauses)} "
        f"{sense} {objective}) SELECT count(*), {objectives[objective]} FROM r"
    )
    return sql, near_miss is None


_FUZZ_SEEDS = [11, 12, 13, 14, 15, 16, 17, 18]


_FUZZ_CASES_PER_SEED = 30


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("seed", _FUZZ_SEEDS)
def test_direct_s1_seeded_fuzz_matches_solver_path(decidb_cli, seed):
    compare_direct_with_solver(decidb_cli, _fuzz_query, random.Random(seed), _FUZZ_CASES_PER_SEED)
