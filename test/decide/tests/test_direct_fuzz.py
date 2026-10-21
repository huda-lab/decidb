"""Seeded differential fuzz: random small S1 and A1 queries, run on the solver path (`off`) and the direct path
(`require`).

Where `test_direct_s1_uniform_matroid.py` lists the cases a person thought of, this finds the interactions nobody
listed. The comparator and its rules are in `_direct_differential.py`.
"""

import random
import pytest
from ._direct_differential import compare_direct_with_solver
from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)


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


def _empty_source(source):
    """The same VALUES with every row filtered out: the solver answers (or rejects) an empty source too."""
    return f"(SELECT * FROM {source} WHERE false) empty_t"


# Constant bounds that contradict the decision's built-in domain. The solver rejects them before reading a row, so it
# raises on an empty source as well.
_S1_IMPOSSIBLE_BOUNDS = ["x <= -1", "x >= 2", "x < 0", "x > 1"]


# Strict bounds against a fraction, with no WHEN. Each shifts to a whole bound inside {0, 1}, so S1 admits it: 0.5 gives
# the lower bound 1, 1.5 and 0.5 give the upper bounds 1 and 0.
_S1_STRICT_FRACTION_BOUNDS = ["x > 0.5", "x < 1.5", "x < 0.5", "x > -0.5"]

# Lower bounds below 0 with no WHEN. A Boolean's box starts at [0, 1] and a written bound only narrows it, so each
# always holds and S1 reads it as no constraint.
_S1_ALWAYS_HOLD_BOUNDS = ["x >= -3", "x > -3", "x >= -1", "x >= -0.5"]


def _fuzz_rowbound(rng):
    """One per-row bound on x: a fixed 0/1 value, a restated domain, or any constant bound on one row."""
    kind = rng.choice(["fixed", "fixed", "restate", "constant", "column"])
    if kind == "restate":
        return rng.choice(_DOMAIN_RESTATEMENTS)
    if kind == "column":
        # The per-row `cap` column is NULL on some rows, which both paths must reject the same way.
        when = " WHEN flag" if rng.random() < 0.4 else ""
        return f"x {rng.choice(['<=', '<', '>=', '>', '=', '<>'])} cap{when}"
    row = rng.randint(0, 5)
    if kind == "fixed":
        return f"x = {rng.choice([0, 1])} WHEN id = {row}"
    op = rng.choice(["<=", "<", ">=", ">", "=", "<>"])
    return f"x {op} {rng.choice(['-1', '-0.5', '0', '0.5', '1', '1.5', '2'])} WHEN id = {row}"


def _fuzz_query(rng, with_objective=True):
    """Returns the query and whether it was built as an S1 shape rather than a near miss.

    Without an objective the query reports its row count and a constant: which rows a feasibility problem selects is
    the backend's choice, so no score of the selection can be compared. Every random choice is drawn either way.
    """
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
    # Drawn last, so adding them left every earlier choice of a seed as it was.
    if rng.random() < 0.1:
        source = _empty_source(source)
    if rng.random() < 0.08:
        clauses.append(rng.choice(_S1_IMPOSSIBLE_BOUNDS))
    if rng.random() < 0.12:
        clauses.append(rng.choice(_S1_STRICT_FRACTION_BOUNDS))
    if rng.random() < 0.3:
        # A negation in the count body (a constant term stays on the solver). The comparison and the bound are left as
        # drawn. Seven slots, as when the offset bodies were drawn too, so every later draw of a seed is unchanged.
        body = rng.choice(["-x"] * 7)
        clauses = [
            clause.replace("SUM(x)", f"SUM({body})", 1) if clause.startswith("SUM(x)") else clause for clause in clauses
        ]
    if rng.random() < 0.12:
        clauses.append(rng.choice(_S1_ALWAYS_HOLD_BOUNDS))
    if not with_objective:
        sql = (
            f"WITH r AS (SELECT * FROM {source} DECIDE x(BOOL) SUCH THAT {' AND '.join(clauses)}) "
            "SELECT count(*), 0 FROM r"
        )
        return sql, near_miss != "clause"
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


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("seed", _FUZZ_SEEDS)
def test_direct_s1_no_objective_seeded_fuzz_matches_solver_path(decidb_cli, seed):
    compare_direct_with_solver(
        decidb_cli, lambda rng: _fuzz_query(rng, with_objective=False), random.Random(seed), _FUZZ_CASES_PER_SEED
    )


# ---------------------------------------------------------------------------
# A1: separable box-constrained problem
# ---------------------------------------------------------------------------


_A1_IMPOSSIBLE_BOUNDS = {
    "REAL": ["x <= -1", "x <= -0.5"],
    "INT": ["x <= -1", "x < 0"],
    "BOOL": ["x >= 2", "x > 1", "x BETWEEN 2 AND 3", "x <= -1"],
}


def _a1_rows(rng):
    """Dyadic values only, so a product or a quotient is exact and no bound sits within the solvers' tolerance."""
    rows = []
    for i in range(rng.randint(1, 6)):
        t = rng.randrange(-16, 48) / 4.0
        cap = rng.randrange(0, 21) / 2.0
        lo = rng.randrange(-6, 7) / 2.0
        w = rng.choice([-2.0, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0])
        s = rng.randrange(-12, 13) / 4.0
        flag = rng.choice(["true", "false"])
        rows.append(f"({i + 1}, {t}::DOUBLE, {cap}::DOUBLE, {lo}::DOUBLE, {w}::DOUBLE, {flag}, {s}::DOUBLE)")
    return f"(VALUES {', '.join(rows)}) t(id, t, c, lo, w, f, s)"


def _a1_bound(rng, domain):
    upper = rng.choice([3, 4.5, 6, 10, 20]) if domain != "BOOL" else 1
    lower = rng.choice([-3, -2, -1, 0, 1, 2]) if domain != "BOOL" else rng.choice([0, 0, 1])
    options = [
        "x <= c",
        "x <= c",
        "x >= lo",
        "x BETWEEN lo AND c",
        f"x <= {upper}",
        f"x >= {lower}",
        f"x BETWEEN {lower} AND {upper}",
        "x <= c WHEN (f)",
        "x >= lo WHEN (f)",
        f"x <= {upper} WHEN (f)",
        "2*x <= c",
        "-x >= -c",
        "c >= x",
        "w*x <= c",
        "x + 1 <= c",
        "x = lo WHEN (f)",
        f"x >= {lower} AND x <= {upper}",
    ]
    if domain == "INT":
        # Strict bounds against whole constants and against columns are where the solver and direct solve agree;
        # a strict bound against a fractional literal is covered by the table in test_direct_a1_separable_box.py.
        options += [f"x < {int(upper) + 1}", f"x > {lower - 1}", "x < c", "x > lo", "2*x < c"]
    if domain in ("BOOL", "INT"):
        options += ["x <= f", "x >= f WHEN (id > 1)"]
    return rng.choice(options)


_A1_OBJECTIVES = {
    # name -> (the SUM body, the same per-row term for re-evaluating the answer, an optional trailing WHEN)
    "linear": ("s*x", "s*x", ""),
    "square": ("POWER(x - t, 2)", "POWER(x - t, 2)", ""),
    "square_linear": ("POWER(x - t, 2) + s*x", "POWER(x - t, 2) + s*x", ""),
    "scaled_square": ("3*POWER(x - t, 2) - 2*x", "3*POWER(x - t, 2) - 2*x", ""),
    "inner_weight": ("POWER(w*x - t, 2)", "POWER(w*x - t, 2)", ""),
    "square_when": ("POWER(x - t, 2) + s*x", "CASE WHEN f THEN POWER(x - t, 2) + s*x ELSE 0 END", " WHEN (f)"),
}


def _a1_query(rng):
    """Returns the query and whether it was built as an A1 shape rather than a near miss."""
    near_miss = rng.choice([None] * 9 + ["aggregate", "not_equal", "two_squares"])
    domain = rng.choice(["REAL", "REAL", "INT", "INT", "BOOL"])
    source = _a1_rows(rng)
    clauses = [_a1_bound(rng, domain) for _ in range(rng.randint(1, 3))]
    if near_miss == "aggregate":
        clauses.append(f"SUM(x) <= {rng.randint(1, 9)}")
    elif near_miss == "not_equal" and domain != "REAL":
        clauses.append(f"x <> {rng.randint(0, 3)}")
    kind = rng.choice(["none", *_A1_OBJECTIVES, *_A1_OBJECTIVES])
    sense = rng.choice(["MINIMIZE", "MAXIMIZE"])
    squared = kind in ("square", "square_linear", "scaled_square", "inner_weight", "square_when")
    if domain == "INT" and (squared or near_miss == "two_squares"):
        # Maximizing a convex square over whole numbers is a non-convex integer program, which the solver does not
        # finish when a row is unbounded. Direct solve answers it, but there is nothing to compare it with.
        sense = "MINIMIZE"
    if kind == "none":
        objective, per_row = "", "0"
    else:
        body, per_row, when = _A1_OBJECTIVES[kind]
        objective = f"{sense} SUM({body}){when}"
        if near_miss == "two_squares":
            objective += " + SUM(POWER(x - c, 2))"
    # Drawn last, so adding them left every earlier choice of a seed as it was.
    if rng.random() < 0.1:
        source = _empty_source(source)
    if rng.random() < 0.08:
        clauses.append(rng.choice(_A1_IMPOSSIBLE_BOUNDS[domain]))
    sql = (
        f"WITH r AS (SELECT * FROM {source} DECIDE x({domain}) SUCH THAT {' AND '.join(clauses)} {objective}) "
        f"SELECT count(*), sum({per_row}) FROM r"
    )
    return sql, near_miss is None


_A1_FUZZ_SEEDS = [21, 22, 23, 24, 25, 26, 27, 28]


_A1_FUZZ_CASES_PER_SEED = 30


@pytest.mark.var_real
@pytest.mark.var_integer
@pytest.mark.var_boolean
@pytest.mark.cons_perrow
@pytest.mark.correctness
@pytest.mark.parametrize("seed", _A1_FUZZ_SEEDS)
def test_direct_a1_seeded_fuzz_matches_solver_path(decidb_cli, seed):
    compare_direct_with_solver(decidb_cli, _a1_query, random.Random(seed), _A1_FUZZ_CASES_PER_SEED)
