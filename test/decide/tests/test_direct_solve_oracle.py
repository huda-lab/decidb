"""Independent checks for the S1 direct-solve path.

VAL-06 compares direct results with the project's independent ILP oracle
(``oracle_solver``), which builds its model from the raw rows rather than from
DeciDB's SQL interpretation. VAL-07 is a seeded differential fuzz: random small
S1 queries run on the solver path (``off``) and the direct path (``require``).

Note: ``oracle_solver`` caches each objective under a hash of the *test
function's* source plus the database checksum. The case table and model builder
below live outside that source, so editing them does not invalidate the cache; a
stale value then fails loudly with an objective mismatch. After changing a case
or ``_build_oracle``, remove that test's entries from ``results/oracle_cache.json``.
"""

import random

import pytest

from comparison.compare import compare_solutions
from decidb_cli import DecidBCliError

from ._direct_differential import compare_direct_with_solver
from solver.types import ObjSense, SolverStatus, VarType


@pytest.fixture(autouse=True)
def _allow_direct_path(monkeypatch):
    # The suite also runs with a process-wide forced HiGHS backend; this file
    # needs the direct path, and ``off`` runs use the default backend.
    monkeypatch.delenv("DECIDB_FORCE_SOLVER", raising=False)


# ---------------------------------------------------------------------------
# VAL-06: direct result vs independent oracle
# ---------------------------------------------------------------------------

# Row layout: (id, g, score, cap, flag). ``g`` is the PER key, ``flag`` the WHEN
# membership, ``cap`` a per-row source-valued count bound.
_ROWS = [
    (0, "a", 9.0, 2, True),
    (1, "a", 5.5, 1, True),
    (2, "a", -1.0, 3, False),
    (3, "b", 8.0, 2, True),
    (4, "b", 4.0, 2, True),
    (5, "b", 3.5, 1, False),
    (6, "c", -2.5, 2, True),
    (7, "c", 0.5, 3, True),
    (8, None, 6.0, 1, True),
    (9, None, -3.0, 2, False),
]


def _values(rows):
    def lit(value, sql_type):
        return f"CAST({'NULL' if value is None else repr(value)} AS {sql_type})"

    return ", ".join(
        f"({i}, {lit(g, 'VARCHAR')}, {lit(float(score), 'DOUBLE')}, {lit(cap, 'INTEGER')}, {str(flag).lower()})"
        for i, g, score, cap, flag in rows
    )


def _sql(case):
    scope = " ".join(part for part in (("WHEN flag" if case.get("when") else ""),
                                       ("PER g" if case.get("per") else "")) if part)
    clauses = [f"SUM(x) {op} {bound} {scope}".strip() for op, bound in case["bounds"]]
    for value, ids in case.get("pins", ()):
        clauses.extend(f"x = {value} WHEN id = {i}" for i in ids)
    return f"""
        SELECT id, g, score, x FROM (
            FROM (VALUES {_values(_ROWS)}) t(id, g, score, cap, flag)
            DECIDE x(BOOL) SUCH THAT {' AND '.join(clauses)}
            {case.get('sense', 'MAXIMIZE')} SUM(score * x)
        ) q ORDER BY id
    """


def _build_oracle(oracle_solver, case):
    """Encode the case as a plain ILP, straight from the rows.

    A source-valued bound becomes one constraint per eligible row (the group
    count must respect every row's own cap), so the oracle never computes the
    group MIN/MAX that the direct rule uses.
    """
    oracle_solver.create_model("s1_direct_oracle")
    names = [f"x_{i}" for i in range(len(_ROWS))]
    for name in names:
        oracle_solver.add_variable(name, VarType.BINARY)
    members = {}
    for index, (_, g, _, _, flag) in enumerate(_ROWS):
        if case.get("per") and g is None:
            continue
        if case.get("when") and not flag:
            continue
        members.setdefault(g if case.get("per") else None, []).append(index)
    for group in members.values():
        coefficients = {names[i]: 1.0 for i in group}
        for op, bound in case["bounds"]:
            if bound == "cap":
                for i in group:
                    oracle_solver.add_constraint(coefficients, op, float(_ROWS[i][3]))
            else:
                oracle_solver.add_constraint(coefficients, op, float(bound))
    for value, ids in case.get("pins", ()):
        for i in ids:
            oracle_solver.add_constraint({names[i]: 1.0}, "=", float(value))
    sense = ObjSense.MAXIMIZE if case.get("sense", "MAXIMIZE") == "MAXIMIZE" else ObjSense.MINIMIZE
    oracle_solver.set_objective({names[i]: float(_ROWS[i][2]) for i in range(len(_ROWS))}, sense)


_CASES = {
    "global_upper_max": {"bounds": [("<=", 2)]},
    "global_lower_min": {"bounds": [(">=", 3)], "sense": "MINIMIZE"},
    "global_fractional_upper": {"bounds": [("<=", 2.5)]},
    "global_interval": {"bounds": [(">=", 4), ("<=", 6)]},
    "per_interval": {"bounds": [(">=", 1), ("<=", 2)], "per": True},
    "per_exact": {"bounds": [("=", 1)], "per": True, "sense": "MINIMIZE"},
    "when_upper": {"bounds": [("<=", 1)], "when": True},
    "per_and_when": {"bounds": [(">=", 1), ("<=", 1)], "per": True, "when": True},
    "source_bound_per": {"bounds": [("<=", "cap")], "per": True},
    "source_bound_per_when": {"bounds": [(">=", 1), ("<=", "cap")], "per": True, "when": True},
    "source_bound_global_min": {"bounds": [("<=", "cap")], "sense": "MINIMIZE"},
    "pins_with_group_bounds": {
        "bounds": [("<=", 2)],
        "per": True,
        "pins": [(1, [4]), (0, [0, 8])],
    },
    "pins_with_source_bound": {
        "bounds": [(">=", 1), ("<=", "cap")],
        "per": True,
        "pins": [(1, [2]), (0, [1])],
    },
}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(_CASES))
def test_direct_s1_matches_independent_oracle(decidb_cli, oracle_solver, name):
    case = _CASES[name]
    rows, columns = decidb_cli.execute(f"SET decide_direct_solve='require'; {_sql(case)}")
    assert columns == ["id", "g", "score", "x"]
    assert len(rows) == len(_ROWS)

    _build_oracle(oracle_solver, case)
    result = oracle_solver.solve()
    comparison = compare_solutions(
        rows,
        columns,
        result,
        [(i, g, float(score)) for i, g, score, _, _ in _ROWS],
        ["x"],
        coeff_fn=lambda row: {"x": float(row[columns.index("score")])},
    )
    assert comparison.status in ("identical", "optimal")


@pytest.mark.var_boolean
@pytest.mark.error_infeasible
@pytest.mark.correctness
def test_direct_s1_infeasible_agrees_with_oracle(decidb_cli, oracle_solver):
    # Group "b" has three rows; requiring four of them cannot be satisfied.
    case = {"bounds": [(">=", 4)], "per": True}
    _build_oracle(oracle_solver, case)
    assert oracle_solver.solve().status == SolverStatus.INFEASIBLE
    with pytest.raises(DecidBCliError, match=r"(?i)infeasible"):
        decidb_cli.execute(f"SET decide_direct_solve='require'; {_sql(case)}")


# ---------------------------------------------------------------------------
# VAL-07: seeded differential fuzz, solver path vs direct path
# ---------------------------------------------------------------------------

_OBJECTIVES = {
    "SUM(score * x)": "sum(score * x)",
    "SUM(score * x) + SUM(id * x)": "sum(score * x) + sum(id * x)",
    "SUM(score * x) - SUM(cap * x)": "sum(score * x) - sum(coalesce(cap, 0) * x)",
}

# Near misses: norm(e, p) is bound as a SUM(e) tagged with its order, so a path that reads only the aggregate's
# name takes these for S1 shapes and answers a different question.
_NORM_REDUCERS = ["norm(x, 'inf')", "norm(x, 1)", "norm(x, 0)"]
_NORM_OBJECTIVES = {
    "SUM(score * x) - norm(score * x, 1)": "sum(score * x) - sum(abs(score * x))",
}

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
        bound = rng.choice([str(rng.randint(0, 4)), f"{rng.randint(0, 3)}.5", "cap", "COALESCE(cap, 2)"])
        if op == "=" and "cap" in bound:
            bound = str(rng.randint(0, 3))
        reducer = rng.choice(_NORM_REDUCERS) if near_miss == "clause" and index == 0 else "SUM(x)"
        aggregate = f"{reducer} WHEN flag" if style == "local" else reducer
        scope = " ".join(part for part in ("WHEN flag" if style == "top" else "", per) if part)
        clauses.append(f"{aggregate} {op} {bound} {scope}".strip())
    if rng.random() < 0.25:
        value = rng.choice([0, 1])
        clauses.append(f"x = {value} WHEN id = {rng.randint(0, 5)}")
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
