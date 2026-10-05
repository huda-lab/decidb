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
    for op, constant, ids in case.get("rowbounds", ()):
        if ids is None:
            clauses.append(f"x {op} {constant}")
        else:
            clauses.extend(f"x {op} {constant} WHEN id = {i}" for i in ids)
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
    for op, constant, ids in case.get("rowbounds", ()):
        for i in range(len(_ROWS)) if ids is None else ids:
            oracle_solver.add_constraint({names[i]: 1.0}, op, float(constant))
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
    # Per-row bounds on x against a constant: (comparison, constant, ids or None for every row). Restating the
    # BOOL domain changes nothing; a fraction fixes the row to the value it allows; a bound beyond the domain under a
    # WHEN leaves its row free.
    "rowbounds_restate_domain": {
        "bounds": [("<=", 3)],
        "rowbounds": [("<=", 1, None), (">=", 0, None)],
    },
    "rowbounds_fraction_fixes": {
        "bounds": [("<=", 2)],
        "rowbounds": [(">=", 0.5, [2, 6]), ("<=", 0.5, [0])],
    },
    "rowbounds_beyond_domain_under_when": {
        "bounds": [("<=", 2)],
        "rowbounds": [("<=", 5, [0]), (">=", -3, [6]), ("<=", 1.5, [3])],
    },
    "rowbounds_with_groups": {
        "bounds": [(">=", 1), ("<=", 2)],
        "per": True,
        "rowbounds": [("=", 1, [2]), ("<=", 0.5, [0]), ("<=", 1, None)],
        "sense": "MINIMIZE",
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


# Per-row bounds whose values come from the row: (id, g, score, lo, hi) with `x >= lo AND x <= hi`. Row 1 is fixed to
# 1 (lo 0.5), row 2 to 0 (hi 0.5), row 4 to 1 (lo = hi = 1); rows 0, 3 and 5 are free, and row 2 scores well so a
# plain top-k would take it.
_PIN_COLUMN_ROWS = [
    (0, "a", 9.0, 0.0, 1.0),
    (1, "a", 5.0, 0.5, 2.0),
    (2, "a", 8.0, 0.0, 0.5),
    (3, "b", -2.0, -1.0, 5.0),
    (4, "b", 3.0, 1.0, 1.0),
    (5, "b", 6.0, 0.0, 1.0),
]
_PIN_COLUMN_CASES = {
    "global_upper": {"bounds": ["<= 3"], "sense": "MAXIMIZE"},
    "global_minimize": {"bounds": ["<= 3"], "sense": "MINIMIZE"},
    "per_group_interval": {"bounds": [">= 1", "<= 2"], "per": True, "sense": "MAXIMIZE"},
}


@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(_PIN_COLUMN_CASES))
def test_direct_s1_numeric_column_pins_match_independent_oracle(decidb_cli, oracle_solver, name):
    case = _PIN_COLUMN_CASES[name]
    values = ", ".join(
        f"({i}, '{g}', CAST({score} AS DOUBLE), CAST({lo} AS DOUBLE), CAST({hi} AS DOUBLE))"
        for i, g, score, lo, hi in _PIN_COLUMN_ROWS
    )
    scope = " PER g" if case.get("per") else ""
    clauses = " AND ".join(["x >= lo", "x <= hi"] + [f"SUM(x) {bound}{scope}" for bound in case["bounds"]])
    sql = f"""
        SELECT id, g, score, x FROM (
            FROM (VALUES {values}) t(id, g, score, lo, hi)
            DECIDE x(BOOL) SUCH THAT {clauses} {case['sense']} SUM(score * x)
        ) q ORDER BY id
    """
    rows, columns = decidb_cli.execute(f"SET decide_direct_solve='require'; {sql}")

    oracle_solver.create_model("s1_numeric_pin_oracle")
    names = [f"x_{i}" for i, *_ in _PIN_COLUMN_ROWS]
    for var in names:
        oracle_solver.add_variable(var, VarType.BINARY)
    for (i, _, _, lo, hi), var in zip(_PIN_COLUMN_ROWS, names):
        oracle_solver.add_constraint({var: 1.0}, ">=", float(lo))
        oracle_solver.add_constraint({var: 1.0}, "<=", float(hi))
    groups = {}
    for (i, g, *_), var in zip(_PIN_COLUMN_ROWS, names):
        groups.setdefault(g if case.get("per") else None, []).append(var)
    for members in groups.values():
        for bound in case["bounds"]:
            op, limit = bound.split()
            oracle_solver.add_constraint({var: 1.0 for var in members}, op, float(limit))
    sense = ObjSense.MAXIMIZE if case["sense"] == "MAXIMIZE" else ObjSense.MINIMIZE
    oracle_solver.set_objective({var: row[2] for var, row in zip(names, _PIN_COLUMN_ROWS)}, sense)
    result = oracle_solver.solve()
    comparison = compare_solutions(
        rows,
        columns,
        result,
        [(i, g, float(score)) for i, g, score, _, _ in _PIN_COLUMN_ROWS],
        ["x"],
        coeff_fn=lambda row: {"x": float(row[columns.index("score")])},
    )
    assert comparison.status in ("identical", "optimal")


# Scaled objectives: (sign, factor, divides, column) terms, as `+ 2 * SUM(score * x)` or `- SUM(b * x) / 4`. The oracle
# gets each row's coefficient straight from the row and the terms.
_SCALE_ORACLE_ROWS = [(0, 9.0, 1.0), (1, 5.0, -3.0), (2, -2.0, 4.0), (3, 8.0, 2.0), (4, 1.0, -1.0), (5, 6.0, 0.5)]
_SCALE_ORACLE_CASES = {
    "positive_factor": ("MAXIMIZE", [(1, 2, False, "score")]),
    "negative_factor": ("MAXIMIZE", [(1, -1.5, False, "score")]),
    "divisor": ("MAXIMIZE", [(1, 4, True, "score")]),
    "negative_divisor_minimize": ("MINIMIZE", [(1, -2, True, "score")]),
    "two_parts_with_different_factors": ("MAXIMIZE", [(1, 2, False, "score"), (-1, 3, False, "b")]),
    "factor_and_divisor": ("MINIMIZE", [(1, 0.5, False, "score"), (1, 2, True, "b")]),
}


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(_SCALE_ORACLE_CASES))
def test_direct_s1_scaled_objective_matches_independent_oracle(decidb_cli, oracle_solver, name):
    sense, terms = _SCALE_ORACLE_CASES[name]
    parts = []
    for sign, factor, divides, column in terms:
        body = f"SUM({column} * x)"
        parts.append(("- " if sign < 0 else "+ ") + (f"{body} / {factor}" if divides else f"{factor} * {body}"))
    objective = " ".join(parts).lstrip("+ ")
    values = ", ".join(f"({i}, CAST({score} AS DOUBLE), CAST({b} AS DOUBLE))" for i, score, b in _SCALE_ORACLE_ROWS)
    sql = f"""
        SELECT id, score, b, x FROM (
            FROM (VALUES {values}) t(id, score, b)
            DECIDE x(BOOL) SUCH THAT SUM(x) >= 1 AND SUM(x) <= 3 {sense} {objective}
        ) q ORDER BY id
    """
    rows, columns = decidb_cli.execute(f"SET decide_direct_solve='require'; {sql}")

    def coefficient(row):
        value = {"score": row[1], "b": row[2]}
        return sum(
            sign * (value[column] / factor if divides else factor * value[column])
            for sign, factor, divides, column in terms
        )

    oracle_solver.create_model("s1_scaled_objective_oracle")
    names = [f"x_{i}" for i, *_ in _SCALE_ORACLE_ROWS]
    for var in names:
        oracle_solver.add_variable(var, VarType.BINARY)
    oracle_solver.add_constraint({var: 1.0 for var in names}, ">=", 1.0)
    oracle_solver.add_constraint({var: 1.0 for var in names}, "<=", 3.0)
    oracle_solver.set_objective(
        {var: coefficient(row) for var, row in zip(names, _SCALE_ORACLE_ROWS)},
        ObjSense.MAXIMIZE if sense == "MAXIMIZE" else ObjSense.MINIMIZE,
    )
    comparison = compare_solutions(
        rows,
        columns,
        oracle_solver.solve(),
        [(i, float(score), float(b)) for i, score, b in _SCALE_ORACLE_ROWS],
        ["x"],
        coeff_fn=lambda row: {"x": coefficient(row)},
    )
    assert comparison.status in ("identical", "optimal")


@pytest.mark.var_boolean
@pytest.mark.error_infeasible
@pytest.mark.correctness
@pytest.mark.parametrize(
    "rowbound",
    [
        (">=", 2, [3]),
        ("<=", -1, [3]),
        ("=", 0.5, [1]),
        ("=", 2, [0, 1]),
    ],
)
def test_direct_s1_impossible_rowbound_agrees_with_oracle(decidb_cli, oracle_solver, rowbound):
    # A bound no 0/1 value can meet, on rows a WHEN selects, is infeasible.
    case = {"bounds": [("<=", 5)], "rowbounds": [rowbound]}
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
    "-2 * SUM(score * x)": "-2 * sum(score * x)",
    "SUM(score * x) / 4 - 3 * SUM(id * x)": "sum(score * x) / 4 - 3 * sum(id * x)",
    "0.5 * SUM(score * x) + SUM(id * x) / -2": "0.5 * sum(score * x) + sum(id * x) / -2",
}

# Near misses: norm(e, p) is bound as a SUM(e) tagged with its order, so a path that reads only the aggregate's
# name takes these for S1 shapes and answers a different question.
_NORM_REDUCERS = ["norm(x, 'inf')", "norm(x, 1)", "norm(x, 0)"]
_NORM_OBJECTIVES = {
    "SUM(score * x) - norm(score * x, 1)": "sum(score * x) - sum(abs(score * x))",
}

# Spellings of the BOOL domain with no WHEN. Other unconditional bounds are left out: the solver reads some of them as
# a different problem, and direct solve leaves those to it (see test_direct_solve.py).
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
        bound = rng.choice([str(rng.randint(0, 4)), f"{rng.randint(0, 3)}.5", "cap", "COALESCE(cap, 2)"])
        if op == "=" and "cap" in bound:
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
