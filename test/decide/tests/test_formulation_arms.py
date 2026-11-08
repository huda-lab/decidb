"""Branches of the formulation layer (`src/decidb/formulation/`) that a query reaches and no other test did.

Each test names the branch it reaches. Integer models are judged against `_brute_force` (every assignment of a small
domain, no solver); a continuous one against the oracle solver; a refusal pins its text. Both backends run each
query where the backend changes the path: the lowering (`DECIDB_NATIVE_CONSTRUCTS=off`) is the only path HiGHS has.
"""

import re

import pytest

from decidb_cli import DeciDBCliError
from solver.types import ObjSense, SolverStatus, VarType

from ._brute_force import judge, judge_rows, optimum

_INFINITY = "CAST('Infinity' AS DOUBLE)"
_MINUS_INFINITY = "CAST('-Infinity' AS DOUBLE)"
_INFEASIBLE = r"DECIDE optimization is infeasible\. Prefix the query with DIAGNOSE"
_UNBOUNDED = r"DECIDE optimization is unbounded\. Prefix the query with DIAGNOSE"


def _lowered(cli):
    """The Big-M encoding, whatever the host's solver can state natively."""
    return cli.with_env({"DECIDB_NATIVE_CONSTRUCTS": "off"})


def _native(cli):
    """Every construct the backend declares, stated natively (HiGHS declares none)."""
    return cli.with_env({"DECIDB_NATIVE_CONSTRUCTS": "force"})


def _answer(cli, sql, **options):
    rows, cols = cli.execute(sql, **options)
    return [dict(zip(cols, row)) for row in rows]


def _refused(cli, sql, message):
    with pytest.raises(DeciDBCliError) as excinfo:
        cli.execute(sql)
    assert re.search(message, excinfo.value.message), excinfo.value.message


# ---------------------------------------------------------------------------
# The model builder (ilp_model_builder.cpp)
# ---------------------------------------------------------------------------


@pytest.mark.correctness
@pytest.mark.quadratic
def test_product_of_two_forms_that_each_carry_a_constant(
    decidb_cli_gurobi, oracle_solver
):
    """`(x + 1) * (y + 2)`: AddProductRow moves the cross terms of both constants (`right.constant != 0`) to the
    linear part."""
    weights = [2.0, 1.0, 4.0]
    values = ", ".join(f"({i + 1}, {w})" for i, w in enumerate(weights))
    rows = _answer(
        decidb_cli_gurobi,
        f"""
        SELECT id, x, y FROM (VALUES {values}) t(id, w) DECIDE x(REAL), y(REAL)
        SUCH THAT x <= 4 AND y <= 4
        MINIMIZE SUM((x + 1) * (y + 2) - w * x - w * y)
        ORDER BY id""",
    )
    oracle_solver.create_model("product_of_forms_with_constants")
    linear, quadratic = {}, {}
    for i, w in enumerate(weights):
        for name in ("x", "y"):
            oracle_solver.add_variable(
                f"{name}_{i}", VarType.CONTINUOUS, lb=0.0, ub=4.0
            )
        quadratic[(f"x_{i}", f"y_{i}")] = 1.0
        linear[f"x_{i}"] = 2.0 - w  # (x + 1)(y + 2) = xy + 2x + y + 2
        linear[f"y_{i}"] = 1.0 - w
    oracle_solver.set_quadratic_objective(
        linear, quadratic, ObjSense.MINIMIZE, constant=2.0 * len(weights)
    )
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    actual = sum(
        (r["x"] + 1) * (r["y"] + 2) - w * r["x"] - w * r["y"]
        for r, w in zip(rows, weights)
    )
    assert actual == pytest.approx(result.objective_value, rel=1e-4, abs=1e-4)


@pytest.mark.correctness
@pytest.mark.quadratic
def test_strict_greater_than_over_a_quadratic_clause(
    decidb_cli_gurobi, decidb_cli_highs, oracle_solver
):
    """`SUM(x * x) > 20` over whole numbers: BuildQuadraticConstraint steps `> K` to `>= K + 1` (the
    COMPARE_GREATERTHAN case). HiGHS refuses a quadratic clause."""
    sql = """
        SELECT id, x FROM (VALUES (1), (2), (3)) t(id) DECIDE x(INT)
        SUCH THAT x <= 5 AND SUM(x * x) > 20
        MINIMIZE SUM(x)
        ORDER BY id"""
    rows = _answer(decidb_cli_gurobi, sql)
    oracle_solver.create_model("quadratic_strict_greater")
    names = [f"x_{i}" for i in range(3)]
    for name in names:
        oracle_solver.add_variable(name, VarType.INTEGER, lb=0.0, ub=5.0)
    oracle_solver.add_quadratic_constraint(
        {}, {(n, n): 1.0 for n in names}, ">=", 21.0, name="strict"
    )
    oracle_solver.set_objective({n: 1.0 for n in names}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert sum(r["x"] ** 2 for r in rows) > 20
    assert sum(r["x"] for r in rows) == pytest.approx(result.objective_value, abs=1e-6)
    _refused(
        decidb_cli_highs,
        sql,
        r"squares or multiplies decision variables, which needs Gurobi",
    )


@pytest.mark.correctness
@pytest.mark.quadratic
def test_quadratic_clause_whose_linear_terms_cancel_across_rows(
    decidb_cli_gurobi, oracle_solver
):
    """`SUM(c * z + z * z)` with `c` summing to 0 over the rows: the linear coefficient of the one query-wide `z` is
    0 after the sum, and BuildQuadraticConstraint leaves it out (`pair.second != 0.0`).
    """
    cs = [1, -1, 0]
    values = ", ".join(f"({i + 1}, {c})" for i, c in enumerate(cs))
    rows = _answer(
        decidb_cli_gurobi,
        f"""
        SELECT id, z FROM (VALUES {values}) t(id, c) DECIDE per (): z(REAL)
        SUCH THAT z <= 5 AND SUM(c * z + z * z) <= 10
        MAXIMIZE z""",
    )
    oracle_solver.create_model("quadratic_linear_terms_cancel")
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0, ub=5.0)
    oracle_solver.add_quadratic_constraint(
        {"z": float(sum(cs))}, {("z", "z"): float(len(cs))}, "<=", 10.0
    )
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert rows[0]["z"] == pytest.approx(result.objective_value, abs=1e-4)


@pytest.mark.correctness
@pytest.mark.quadratic
def test_quadratic_clause_whose_square_terms_cancel_across_rows(
    decidb_cli_gurobi, oracle_solver
):
    """`SUM(c * z * z)` with `c` summing to 0: the Q entry of `z * z` is 0 after the sum, and
    BuildQuadraticConstraint leaves it out (`entry.second != 0.0`), so the clause holds for every `z`.
    """
    cs = [1, -1, 0]
    values = ", ".join(f"({i + 1}, {c})" for i, c in enumerate(cs))
    rows = _answer(
        decidb_cli_gurobi,
        f"""
        SELECT id, z FROM (VALUES {values}) t(id, c) DECIDE per (): z(REAL)
        SUCH THAT z <= 5 AND SUM(c * z * z) <= 10
        MAXIMIZE z""",
    )
    oracle_solver.create_model("quadratic_square_terms_cancel")
    oracle_solver.add_variable("z", VarType.CONTINUOUS, lb=0.0, ub=5.0)
    oracle_solver.add_quadratic_constraint({}, {("z", "z"): float(sum(cs))}, "<=", 10.0)
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert rows[0]["z"] == pytest.approx(result.objective_value, abs=1e-4)


@pytest.mark.correctness
def test_aggregate_over_a_query_wide_decision_and_a_data_term(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`SUM(x + c)` over a query-wide `x`: the clause's columns are shared, so its folded terms are read
    (`clause_folded_terms`), and the data-only term has no decision to fold (`v == INVALID_INDEX`).
    """
    cs = [0, 2, 1]
    values = ", ".join(f"({i + 1}, {c})" for i, c in enumerate(cs))
    sql = f"""
        SELECT id, x FROM (VALUES {values}) t(id, c) DECIDE per (): x(INT)
        SUCH THAT x <= 5 AND SUM(x + c) <= 13
        MAXIMIZE x
        ORDER BY id"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        judge_rows(
            _answer(cli, sql),
            {"x": range(6)},
            lambda a: 3 * a["x"] + sum(cs) <= 13,
            lambda a: a["x"],
        )


@pytest.mark.correctness
def test_aggregate_over_more_columns_than_the_dense_accumulator_holds(
    decidb_cli_gurobi, decidb_cli_highs, oracle_solver
):
    """1.1 million rows put the row block past the dense accumulator's 2^20 columns (BeginForDecisions takes the
    sparse one), and a clause whose coefficients are all 0 flushes no pair (Flush, `pairs.empty()`). The clause
    `SUM(z + x) <= 999999` over 1.1 million rows holds `z` at 0."""
    rows = 1_100_000
    sql = f"""
        SELECT max(z) AS z, count(*) AS n FROM (
            SELECT i, x, z FROM (SELECT range AS i, 0 AS c FROM range({rows})) t
            DECIDE x(BOOL), per (): z(INT)
            SUCH THAT z <= 3 AND SUM(z + x) <= 999999 AND SUM(c * z + c * x) <= 5
            MAXIMIZE z)"""
    # The oracle states the same clause over the one number it needs of `x`: how many rows take 1.
    oracle_solver.create_model("sparse_accumulator")
    oracle_solver.add_variable("z", VarType.INTEGER, lb=0.0, ub=3.0)
    oracle_solver.add_variable("ones", VarType.INTEGER, lb=0.0, ub=float(rows))
    oracle_solver.add_constraint({"z": float(rows), "ones": 1.0}, "<=", 999999.0)
    oracle_solver.set_objective({"z": 1.0}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        # The bound 999999 beside coefficients of 1 spans six orders of magnitude: the number-range warning.
        answer = _answer(cli, sql, expect_number_range=True)
        assert answer == [{"z": int(result.objective_value), "n": rows}]


# ---------------------------------------------------------------------------
# `<>` lowered to a Big-M (ilp_linearization.cpp, linearization_not_equal.cpp)
# ---------------------------------------------------------------------------


def test_not_equal_over_two_keyed_decisions_finds_each_columns_owner(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`SUM(a) + SUM(b) <> 24` with `a` and `b` each keyed: the Big-M of the lowered clause asks which decision owns
    each keyed column (`VarIndexer::OwnerOf`), and a column of `a` is below the base of `b`.
    """
    sql = """
        SELECT k, a, b FROM (VALUES (1), (1), (2), (3)) t(k)
        DECIDE per k: a(INT), per k: b(INT)
        SUCH THAT a <= 3 AND b <= 3 AND SUM(a) + SUM(b) <> 24
        MAXIMIZE SUM(a) + SUM(b)
        ORDER BY k"""
    keys = (1, 2, 3)
    domains = {f"{n}{k}": range(4) for n in "ab" for k in keys}

    def total(v):
        return sum((2 if k == 1 else 1) * v[f"{n}{k}"] for n in "ab" for k in keys)

    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        rows = _answer(_lowered(cli), sql)
        by_key = {}
        for row in rows:
            assert by_key.setdefault(row["k"], row) == row
        assignment = {f"{n}{k}": by_key[k][n] for n in "ab" for k in keys}
        assert judge(assignment, lambda v: total(v) != 24, total) == optimum(
            domains, lambda v: total(v) != 24, total
        )


def test_not_equal_beside_a_product_of_other_decisions(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`y <> 7` beside `SUM(b * x)`: the Big-M of `y` looks for a bilinear link whose auxiliary is `y`
    (`BilinearLinkOf`) and passes the product's link, which is not."""
    sql = """
        SELECT id, b, x, y FROM (VALUES (1), (2)) t(id)
        DECIDE b(BOOL), x(INT), y(INT)
        SUCH THAT x <= 10 AND y <= 10 AND y >= 7 AND y <> 7 AND SUM(b * x) <= 7
        MAXIMIZE SUM(x) + SUM(b) - SUM(y)
        ORDER BY id"""
    domains = {
        "b0": range(2),
        "b1": range(2),
        "x0": range(11),
        "x1": range(11),
        "y0": range(7, 11),
        "y1": range(7, 11),
    }
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        judge_rows(
            _answer(_lowered(cli), sql),
            {k: v for k, v in domains.items()},
            lambda a: a["y0"] != 7
            and a["y1"] != 7
            and a["b0"] * a["x0"] + a["b1"] * a["x1"] <= 7,
            lambda a: a["x0"] + a["x1"] + a["b0"] + a["b1"] - a["y0"] - a["y1"],
        )


def test_not_equal_over_a_decision_open_below_is_refused(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`x <> 3` with `x` open below and bounded above: the `z == 1 => x >= 4` half is slackened by a Big-M that
    reads `x`'s lower end, which is open (FlatRowReach, `DecideIsOpenBelow`)."""
    sql = """
        SELECT id, x FROM (VALUES (1), (2), (3)) t(id) DECIDE x(INT)
        SUCH THAT x >= -1e30 AND x <= 10 AND x <> 3
        MAXIMIZE SUM(x)"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _refused(
            _lowered(cli),
            sql,
            r"Rewriting <> requires a finite bound on 'x'\. Add constraints 'x >= <lower>' and",
        )


def test_not_equal_over_a_product_open_below_names_its_factor(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`b * x <> 3` with `x` open below: the product's auxiliary is open because `x` is, and the refusal names
    `x`, the factor the user can bound (`BilinearLinkOf` finds the link; its other variable is blamed).
    """
    sql = """
        SELECT id, b, x FROM (VALUES (1), (2)) t(id) DECIDE b(BOOL), x(INT)
        SUCH THAT x >= -1e30 AND x <= 10 AND b * x <> 3
        MAXIMIZE SUM(x)"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _refused(
            _lowered(cli),
            sql,
            r"Rewriting <> requires a finite bound on 'x'\. Add constraints",
        )


def test_not_equal_over_a_coefficient_that_is_zero_on_a_row(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`c * x <> 3` with `c = 0` on one row: that row's term is left out of the clause, its row verdict (`0 <> 3`
    always holds) differs from the other rows', so the clause keeps its Big-M pair (ClassifyNEConstraint).
    """
    cs = [0, 2, 1]
    values = ", ".join(f"({i + 1}, {c})" for i, c in enumerate(cs))
    sql = f"""
        SELECT id, x FROM (VALUES {values}) t(id, c) DECIDE x(INT)
        SUCH THAT x <= 5 AND c * x <> 3
        MAXIMIZE SUM(x)
        ORDER BY id"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        judge_rows(
            _answer(_lowered(cli), sql),
            {f"x{i}": range(6) for i in range(3)},
            lambda a: all(c * a[f"x{i}"] != 3 for i, c in enumerate(cs)),
            lambda a: sum(a[f"x{i}"] for i in range(3)),
        )


@pytest.mark.parametrize("bound", ["x <> 2.5e0", f"x <> {_INFINITY}"])
def test_not_equal_to_a_value_no_whole_number_takes_is_a_tautology(
    decidb_cli_gurobi, tmp_path, bound
):
    """`x <> 2.5` over a whole-numbered `x` excludes nothing, nor does an infinite bound (`!NEIsIntegerValuedRhs`
    on a uniform bound): the clause emits no row."""
    sql = f"""
        SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT)
        SUCH THAT x <= 5 AND {bound}
        MAXIMIZE SUM(x)
        ORDER BY id"""
    cli = _lowered(decidb_cli_gurobi)
    assert not re.findall(
        r"^row \d+:", cli.dump_model(sql, tmp_path / "model.dump"), re.M
    )
    judge_rows(
        _answer(cli, sql),
        {"x0": range(6), "x1": range(6)},
        lambda a: True,
        lambda a: a["x0"] + a["x1"],
    )


def test_aggregate_not_equal_below_the_reachable_range_is_a_tautology(
    decidb_cli_gurobi, tmp_path
):
    """`SUM(x) <> -1` over `x >= 0`: the group's range lies above the excluded value (`grp_lo > rhs + 0.5`), so the
    group emits no row."""
    sql = """
        SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT)
        SUCH THAT x <= 5 AND SUM(x) <> -1
        MINIMIZE SUM(x)
        ORDER BY id"""
    cli = _lowered(decidb_cli_gurobi)
    assert not re.findall(
        r"^row \d+:", cli.dump_model(sql, tmp_path / "model.dump"), re.M
    )
    judge_rows(
        _answer(cli, sql),
        {"x0": range(6), "x1": range(6)},
        lambda a: True,
        lambda a: a["x0"] + a["x1"],
        maximize=False,
    )


# ---------------------------------------------------------------------------
# Hard MIN / MAX (linearization_minmax.cpp)
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("excluded_by", ["Infinity", "-Infinity"])
def test_hard_max_under_when_with_an_infinite_bound(
    decidb_cli_gurobi, decidb_cli_highs, excluded_by
):
    """`when id > 1: max(x) >= +/-inf`: ClassifyMinMaxGroups reads the bound's direction, and the row the `when`
    leaves out (group INVALID_INDEX) is skipped, both in the unsatisfiable group's plain rows and in the groups it
    drops. `+inf` is out of reach (infeasible); `-inf` holds for every assignment."""
    infinite = f"CAST('{excluded_by}' AS DOUBLE)"
    sql = f"""
        SELECT id, x FROM (VALUES (1), (2), (3)) t(id) DECIDE x(INT)
        SUCH THAT when id > 1: max(x) >= {infinite} AND x <= 4
        MINIMIZE SUM(x)
        ORDER BY id"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        if excluded_by == "Infinity":
            _refused(_lowered(cli), sql, _INFEASIBLE)
        else:
            judge_rows(
                _answer(_lowered(cli), sql),
                {f"x{i}": range(5) for i in range(3)},
                lambda a: True,
                lambda a: a["x0"] + a["x1"] + a["x2"],
                maximize=False,
            )


def test_hard_max_over_a_coefficient_that_is_zero_on_a_row(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`max(c * x) >= 4` with `c = 0` on a row: that row's member has no decision term (FillLink skips it)."""
    cs = [0, 2, 1]
    values = ", ".join(f"({i + 1}, {c})" for i, c in enumerate(cs))
    sql = f"""
        SELECT id, x FROM (VALUES {values}) t(id, c) DECIDE x(INT)
        SUCH THAT x <= 5 AND max(c * x) >= 4
        MINIMIZE SUM(x)
        ORDER BY id"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        judge_rows(
            _answer(_lowered(cli), sql),
            {f"x{i}": range(6) for i in range(3)},
            lambda a: max(c * a[f"x{i}"] for i, c in enumerate(cs)) >= 4,
            lambda a: sum(a[f"x{i}"] for i in range(3)),
            maximize=False,
        )


def test_native_max_over_an_expression_whose_other_term_is_negligible(
    decidb_cli_gurobi,
):
    """`max(x + 1e-16 * y)` stated natively: the member `x + 1e-16 * y` is no renaming of `x` unless the term of
    `y` is dropped as zero, and ExtremumArgumentColumn drops a coefficient under 1e-15.
    """
    sql = """
        SELECT id, x, y FROM (VALUES (1), (2)) t(id) DECIDE x(INT), y(INT)
        SUCH THAT x <= 5 AND y <= 3 AND max(x + 0.0000000000000001 * y) >= 4
        MINIMIZE SUM(x)
        ORDER BY id"""
    judge_rows(
        _answer(_native(decidb_cli_gurobi), sql),
        {f"{n}{i}": range(6 if n == "x" else 4) for n in "xy" for i in range(2)},
        lambda a: max(a[f"x{i}"] + 1e-16 * a[f"y{i}"] for i in range(2)) >= 4,
        lambda a: a["x0"] + a["x1"],
        maximize=False,
    )


def test_min_out_of_reach_is_infeasible_natively_and_lowered(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`min(x) <= -3` over `x >= 0`: the bound lies outside the family's reach, and a native MIN opens the demanded
    low end of its extremum column (`open_demanded_end` for MIN); the query is infeasible on either arm.
    """
    sql = """
        SELECT id, x FROM (VALUES (1), (2), (3)) t(id) DECIDE x(REAL)
        SUCH THAT x <= 5 AND min(x) <= -3
        MINIMIZE SUM(x)"""
    _refused(_native(decidb_cli_gurobi), sql, _INFEASIBLE)
    _refused(_lowered(decidb_cli_gurobi), sql, _INFEASIBLE)
    _refused(decidb_cli_highs, sql, _INFEASIBLE)


def test_objective_group_sums_over_two_open_decisions_name_the_first(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`MAXIMIZE max(per g: sum(x + y) by (g))` with `x` and `y` open: each row's range is open below by two
    decisions, and the first one met is the one named (SavedRowRange keeps the first `unbounded_var`).
    """
    sql = """
        SELECT g, id, x, y FROM (VALUES (1, 1), (1, 2), (2, 3)) t(g, id) DECIDE x(REAL), y(REAL)
        SUCH THAT x >= -1e30 AND y >= -1e30
        MAXIMIZE max(per g: sum(x + y) by (g))"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _refused(
            _lowered(cli),
            sql,
            r"Rewriting MIN/MAX requires a finite bound on 'x'\. Add constraints 'x >= <lower>' and",
        )


# ---------------------------------------------------------------------------
# Composed MIN / MAX (`SUM(a) + MAX(b) <= K`)
# ---------------------------------------------------------------------------


def _composed(cli, clause, domains, feasible):
    sql = f"""
        SELECT id, x, y FROM (VALUES (1), (2), (3)) t(id) DECIDE x(INT), y(INT)
        SUCH THAT x <= 5 AND y <= 5 AND {clause}
        MAXIMIZE SUM(x)
        ORDER BY id"""
    judge_rows(
        _answer(_lowered(cli), sql),
        domains,
        feasible,
        lambda a: a["x0"] + a["x1"] + a["x2"],
    )


_XY = {f"{n}{i}": range(6) for n in "xy" for i in range(3)}


def test_composed_clause_with_an_avg_term(decidb_cli_gurobi, decidb_cli_highs):
    """`avg(x) + max(y) <= 4`: the AVG term's coefficients are divided by its row count."""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _composed(
            cli,
            "avg(x) + max(y) <= 4",
            _XY,
            lambda a: (a["x0"] + a["x1"] + a["x2"]) / 3 + max(a["y0"], a["y1"], a["y2"])
            <= 4 + 1e-9,
        )


def test_composed_clause_with_a_data_term_in_its_sum(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`sum(x + id) + max(y) <= 14`: the `id` term has no decision; its sum moves to the bound side."""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _composed(
            cli,
            "sum(x + id) + max(y) <= 14",
            _XY,
            lambda a: sum(a[f"x{i}"] + i + 1 for i in range(3))
            + max(a["y0"], a["y1"], a["y2"])
            <= 14,
        )


def test_composed_clause_whose_extremum_has_a_negative_coefficient(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`sum(x) + max(0 - y) >= -3`: the extremum's argument has a negative coefficient, so its range takes the
    low end of `y` for the high end of the argument (ComposedTermRange, `c > 0.0` false).
    """
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _composed(
            cli,
            "sum(x) + max(0 - y) >= -3",
            _XY,
            lambda a: sum(a[f"x{i}"] for i in range(3))
            + max(-a["y0"], -a["y1"], -a["y2"])
            >= -3,
        )


def test_composed_clause_over_a_decision_open_below(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`sum(x) + max(y) <= 9` with `y` open below: the extremum's range is open below (`DecideIsOpenBelow`), and
    the easy direction needs no Big-M, so the query answers."""
    sql = """
        SELECT id, x, y FROM (VALUES (1), (2)) t(id) DECIDE x(INT), y(INT)
        SUCH THAT x <= 5 AND y >= -1e30 AND y <= 5 AND sum(x) + max(y) <= 9
        MAXIMIZE SUM(x)
        ORDER BY id"""
    domains = {"x0": range(6), "x1": range(6), "y0": range(-7, 6), "y1": range(-7, 6)}
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        judge_rows(
            _answer(_lowered(cli), sql),
            domains,
            lambda a: a["x0"] + a["x1"] + max(a["y0"], a["y1"]) <= 9,
            lambda a: a["x0"] + a["x1"],
        )


# ---------------------------------------------------------------------------
# ABS (linearization_bilinear_abs.cpp)
# ---------------------------------------------------------------------------

_ABS_WIDTH = {"x0": range(10), "x1": range(10)}
_ABS_ROWS = "(VALUES (1, 3), (2, 7)) t(id, w)"


def _abs_query(
    cli,
    clause,
    objective="MINIMIZE SUM(x)",
    decide="x(INT)",
    bounds="x >= 0 AND x <= 9",
):
    sql = f"""
        SELECT id, x FROM {_ABS_ROWS} DECIDE {decide}
        SUCH THAT {bounds} AND {clause}
        {objective}
        ORDER BY id"""
    return _answer(_lowered(cli), sql)


@pytest.mark.parametrize(
    "clause,holds",
    [
        ("SUM(ABS(x - w)) > 5", lambda d: d > 5),
        ("SUM(ABS(x - w)) = 6", lambda d: d == 6),
    ],
    ids=["strict greater", "equal"],
)
def test_hard_abs_clause_demands_its_auxiliary_by_comparison(
    decidb_cli_gurobi, decidb_cli_highs, clause, holds
):
    """A hard ABS clause sizes the auxiliary's ceiling from what the clause demands of it, whichever way the
    comparison pushes (`>` pushes up; `=` pushes both ways: DemandedAuxReach)."""
    ws = (3, 7)
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        judge_rows(
            _abs_query(cli, clause),
            _ABS_WIDTH,
            lambda a: holds(sum(abs(a[f"x{i}"] - w) for i, w in enumerate(ws))),
            lambda a: a["x0"] + a["x1"],
            maximize=False,
        )


def test_hard_abs_beside_a_negative_coefficient_under_strict_less_than(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`SUM(y) - SUM(ABS(x - id)) < 3`: the auxiliary's coefficient is negative, so the strict `<` pushes it up
    (`a < 0 && pushes_down`, with COMPARE_LESSTHAN among the downward comparisons)."""
    sql = """
        SELECT id, x, y FROM (VALUES (1), (2)) t(id) DECIDE x(INT), y(INT)
        SUCH THAT x <= 6 AND y <= 6 AND SUM(y) - SUM(ABS(x - id)) < 3
        MAXIMIZE SUM(y)
        ORDER BY id"""
    domains = {f"{n}{i}": range(7) for n in "xy" for i in range(2)}
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        judge_rows(
            _answer(_lowered(cli), sql),
            domains,
            lambda a: a["y0"] + a["y1"] - abs(a["x0"] - 1) - abs(a["x1"] - 2) < 3,
            lambda a: a["y0"] + a["y1"],
        )


def test_hard_abs_clause_with_a_zero_weight_on_a_row(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`SUM(c * ABS(x - w)) >= 4` with `c = 0` on one row: that row does not demand anything of its auxiliary."""
    sql = """
        SELECT id, x FROM (VALUES (1, 3, 0), (2, 7, 2)) t(id, w, c) DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 9 AND SUM(c * ABS(x - w)) >= 4
        MINIMIZE SUM(x)
        ORDER BY id"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        judge_rows(
            _answer(_lowered(cli), sql),
            _ABS_WIDTH,
            lambda a: 2 * abs(a["x1"] - 7) >= 4,
            lambda a: a["x0"] + a["x1"],
            maximize=False,
        )


def test_hard_abs_clause_beside_a_decision_open_below(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`SUM(ABS(x - w)) + SUM(y) >= 4` with `y` open below: the other terms' low end is open, so the demand on
    the auxiliary cannot be read from it (`std::isfinite(lo)`), and the query still answers.
    """
    sql = f"""
        SELECT id, x, y FROM {_ABS_ROWS} DECIDE x(INT), y(INT)
        SUCH THAT x >= 0 AND x <= 9 AND y >= -1e30 AND y <= 3 AND SUM(ABS(x - w)) + SUM(y) >= 4
        MINIMIZE SUM(x)
        ORDER BY id"""
    domains = {
        "x0": range(10),
        "x1": range(10),
        "y0": range(-10, 4),
        "y1": range(-10, 4),
    }
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        judge_rows(
            _answer(_lowered(cli), sql),
            domains,
            lambda a: abs(a["x0"] - 3) + abs(a["x1"] - 7) + a["y0"] + a["y1"] >= 4,
            lambda a: a["x0"] + a["x1"],
            maximize=False,
        )


def test_hard_abs_clause_with_an_infinite_bound_is_infeasible(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`SUM(ABS(x - w)) >= inf`: DemandedAuxReach cannot read an infinite bound and leaves the demand alone; the
    clause cannot be met."""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        sql = f"""
            SELECT id, x FROM {_ABS_ROWS} DECIDE x(INT)
            SUCH THAT x >= 0 AND x <= 9 AND SUM(ABS(x - w)) >= {_INFINITY}
            MINIMIZE SUM(x)"""
        _refused(_lowered(cli), sql, _INFEASIBLE)


def test_native_abs_over_a_coefficient_that_is_zero_on_a_row(decidb_cli_gurobi):
    """`ABS(c * x - 3)` stated natively with `c = 0` on one row: that row's argument has no decision term, so it is
    not a renaming of a column and earns a column of its own (EmitNativeAbs, `coef == 0.0`).
    """
    cs = [0, 2, 1]
    values = ", ".join(f"({i + 1}, {c})" for i, c in enumerate(cs))
    sql = f"""
        SELECT id, x FROM (VALUES {values}) t(id, c) DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 5
        MAXIMIZE SUM(ABS(c * x - 3))
        ORDER BY id"""
    judge_rows(
        _answer(_native(decidb_cli_gurobi), sql),
        {f"x{i}": range(6) for i in range(3)},
        lambda a: True,
        lambda a: sum(abs(c * a[f"x{i}"] - 3) for i, c in enumerate(cs)),
    )


def test_a_product_with_two_reducer_helpers_names_the_unbounded_member(
    decidb_cli_gurobi, decidb_cli_highs
):
    """`b * SUM(x) by (g) <= 3 AND b * SUM(y) by (g) <= 4` with `y` unbounded: each reducer is a helper decision, and
    the refusal of the second one's product names the member `y` (the loop over helpers passes the first).
    """
    sql = """
        SELECT id, x, y, b FROM (VALUES (1, 'a'), (2, 'a'), (3, 'b')) t(id, g)
        DECIDE x(INT), y(INT), per (): b(BOOL)
        SUCH THAT x <= 3 AND b * SUM(x) by (t.g) <= 3 AND b * SUM(y) by (t.g) <= 4
        MAXIMIZE SUM(x) + SUM(y) + 8 * b"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _refused(
            cli,
            sql,
            r"Bilinear term requires a finite upper bound on variable 'y'\. Add a constraint like",
        )


# ---------------------------------------------------------------------------
# Reducer helpers (linearization_reducer_value.cpp)
# ---------------------------------------------------------------------------

_HELPER_ROWS = "(VALUES (1, 'a'), (2, 'a'), (3, 'b')) t(id, g)"


@pytest.mark.parametrize(
    "bounds,clause,objective,message",
    [
        # MAX helper whose members are open below: the closing Big-M reads the family's low end.
        (
            "x >= -1e30 AND x <= 20",
            "x <= 0.5 * max(x) by (t.g) + 8",
            "MAXIMIZE SUM(x)",
            r"Rewriting MIN/MAX requires a finite bound on 'x'",
        ),
        # MIN helper whose own box is open below.
        (
            "x >= -1e30 AND x <= 20",
            "x >= 2 * min(x) by (t.g) - 1",
            "MINIMIZE SUM(x)",
            r"Rewriting MIN/MAX requires a finite bound on 'x'",
        ),
        # MIN helper whose members are open above: nothing bounds the maximum.
        ("x >= 0", "x <= min(x) by (t.g) + 5", "MAXIMIZE SUM(x)", _UNBOUNDED),
    ],
    ids=[
        "max, members open below",
        "min, helper open below",
        "min, members open above",
    ],
)
def test_reducer_helper_with_an_open_closing_side(
    decidb_cli_gurobi, decidb_cli_highs, bounds, clause, objective, message
):
    """A MIN/MAX helper's closing side needs a finite end on the helper's box and the family's far end; an open
    one is refused, or leaves the query unbounded, and says so."""
    sql = f"""
        SELECT id, g, x FROM {_HELPER_ROWS} DECIDE x(INT)
        SUCH THAT {bounds} AND {clause}
        {objective}"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _refused(_lowered(cli), sql, message)


# ---------------------------------------------------------------------------
# The L0 norm's Big-M (DecideTightPerRowBigM)
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "bounds",
    ["x >= -1e30", "x >= -1e30 AND x <= 20"],
    ids=["open both ends", "open below"],
)
def test_l0_norm_over_an_unbounded_decision_names_the_norm(
    decidb_cli_gurobi, decidb_cli_highs, bounds
):
    """`norm(x - w, 0)` links a 0/1 indicator to `x - w` with a Big-M, which needs a finite box on `x`. The refusal
    names the construct that asked, the norm, and the variable to bound (it said MIN/MAX before).
    """
    sql = f"""
        SELECT id, x FROM (VALUES (1, 3.0), (2, 7.0)) t(id, w) DECIDE x(REAL)
        SUCH THAT SUM(x) = 10 AND {bounds}
        MINIMIZE norm(x - w, 0)"""
    for cli in (decidb_cli_gurobi, decidb_cli_highs):
        _refused(
            cli,
            sql,
            r"Rewriting norm\(e, 0\) requires a finite bound on 'x'\. Add constraints 'x >= <lower>' and 'x <= <upper>'\.",
        )
