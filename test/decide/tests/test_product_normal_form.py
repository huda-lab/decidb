"""Products of decisions take one spelling, and the shape check judges that spelling.

Canonicalization respells every decision-bearing product as coefficient, then decision factors raised to whole
powers, merging equal factors and keeping a POWER of a degree-1 base whole. The spellings below are the same
polynomial, so each answers as the oracle's model of that polynomial. The shape check then refuses, once per kind,
what the polynomial vocabulary cannot read; each refusal quotes the clause and points at the term.
"""

import re

import pytest

from solver.types import ObjSense, SolverStatus, VarType

# (id, w, c): w is a data weight, c a linear coefficient.
_ROWS = [(1, 2.0, 3.0), (2, 1.0, 5.0), (3, 4.0, 1.0)]
_TABLE = "(VALUES (1, 2.0, 3.0), (2, 1.0, 5.0), (3, 4.0, 1.0)) t(id, w, c)"
_UB = 4.0


def _values(rows, cols, name):
    ci = {c: i for i, c in enumerate(cols)}
    by_id = {int(r[ci["id"]]): float(r[ci[name]]) for r in rows}
    return [by_id[i] for i, _, _ in _ROWS]


def _variables(oracle_solver, names, var_type=VarType.CONTINUOUS):
    for name in names:
        for i in range(len(_ROWS)):
            oracle_solver.add_variable(f"{name}_{i}", var_type, lb=0.0, ub=_UB if var_type != VarType.BINARY else 1.0)


# Each spelling as (SQL square term, the term's Q entries for row i as {(a, b): coeff}, the term evaluated in Python).
_SQUARES = {
    "coefficient first": ("2*x*x", lambda i, w: {("x", "x"): 2.0}, lambda w, x, y: 2 * x * x),
    "coefficient between": ("x*2*x", lambda i, w: {("x", "x"): 2.0}, lambda w, x, y: 2 * x * x),
    "cast exponent": ("POWER(x, CAST(2 AS INTEGER))", lambda i, w: {("x", "x"): 1.0}, lambda w, x, y: x * x),
    "power of a linear form": (
        "POWER(x - w, 2)",
        lambda i, w: {("x", "x"): 1.0},
        lambda w, x, y: (x - w) ** 2,
    ),
    "equal sums merge": (
        "(x+y)*(y+x)",
        lambda i, w: {("x", "x"): 1.0, ("x", "y"): 2.0, ("y", "y"): 1.0},
        lambda w, x, y: (x + y) ** 2,
    ),
    # A data weight on a square is the product group's weight (BUG-46): Gurobi solves it, as it does x*y.
    "data weight times a power": ("w*POWER(x, 2)", lambda i, w: {("x", "x"): w}, lambda w, x, y: w * x * x),
    "data weight times a product": ("w*x*x", lambda i, w: {("x", "x"): w}, lambda w, x, y: w * x * x),
    "power times a data weight": ("POWER(x, 2)*w", lambda i, w: {("x", "x"): w}, lambda w, x, y: w * x * x),
}

_DATA_WEIGHTS = ["data weight times a power", "data weight times a product", "power times a data weight"]


def _uses_y(term):
    return "y" in term


@pytest.mark.correctness
@pytest.mark.quadratic
class TestEverySpellingIsOnePolynomial:
    """Each spelling of a square answers as the oracle's model of the polynomial it denotes."""

    @pytest.mark.parametrize("spelling", list(_SQUARES))
    def test_square_under_minimize(self, decidb_cli_gurobi, oracle_solver, spelling):
        term, q_of, value = _SQUARES[spelling]
        names = ["x", "y"] if _uses_y(term) else ["x"]
        decide = ", ".join(f"{n}(REAL)" for n in names)
        bounds = " AND ".join(f"{n} <= {_UB}" for n in names)
        rows, cols = decidb_cli_gurobi.execute(
            f"""
            SELECT id, {", ".join(names)} FROM {_TABLE} DECIDE {decide}
            SUCH THAT {bounds}
            MINIMIZE SUM({term} - c * x)
        """
        )
        oracle_solver.create_model(f"square_min_{spelling}")
        _variables(oracle_solver, names)
        linear, quadratic, constant = {}, {}, 0.0
        for i, (_, w, c) in enumerate(_ROWS):
            linear[f"x_{i}"] = -c
            for (a, b), coeff in q_of(i, w).items():
                quadratic[(f"{a}_{i}", f"{b}_{i}")] = coeff
            if spelling == "power of a linear form":
                linear[f"x_{i}"] += -2.0 * w
                constant += w * w
        oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MINIMIZE, constant=constant)
        result = oracle_solver.solve()
        assert result.status == SolverStatus.OPTIMAL
        xs = _values(rows, cols, "x")
        ys = _values(rows, cols, "y") if "y" in names else [0.0] * len(_ROWS)
        actual = sum(value(w, x, y) - c * x for (_, w, c), x, y in zip(_ROWS, xs, ys))
        assert actual == pytest.approx(result.objective_value, rel=1e-4, abs=1e-4)

    @pytest.mark.parametrize("spelling", ["coefficient first", "coefficient between", "cast exponent"] + _DATA_WEIGHTS)
    def test_square_under_a_limit(self, decidb_cli_gurobi, oracle_solver, spelling):
        term, q_of, value = _SQUARES[spelling]
        rows, cols = decidb_cli_gurobi.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(REAL)
            SUCH THAT x <= {_UB} AND SUM({term}) <= 10
            MAXIMIZE SUM(c * x)
        """
        )
        oracle_solver.create_model(f"square_limit_{spelling}")
        _variables(oracle_solver, ["x"])
        quadratic = {}
        for i, (_, w, _) in enumerate(_ROWS):
            for (a, b), coeff in q_of(i, w).items():
                quadratic[(f"{a}_{i}", f"{b}_{i}")] = coeff
        oracle_solver.add_quadratic_constraint({}, quadratic, "<=", 10.0, name="limit")
        oracle_solver.set_objective({f"x_{i}": c for i, (_, _, c) in enumerate(_ROWS)}, ObjSense.MAXIMIZE)
        result = oracle_solver.solve()
        assert result.status == SolverStatus.OPTIMAL
        xs = _values(rows, cols, "x")
        assert sum(c * x for (_, _, c), x in zip(_ROWS, xs)) == pytest.approx(result.objective_value, rel=1e-4)
        assert sum(value(w, x, 0.0) for (_, w, _), x in zip(_ROWS, xs)) <= 10 + 1e-6

    @pytest.mark.parametrize(
        "term,weight", [("3*x*x", lambda w: 3.0), ("POWER(w*x, 2)", lambda w: w * w)], ids=["data coefficient", "power"]
    )
    def test_square_answers_on_highs(self, decidb_cli_highs, oracle_solver, term, weight):
        """A coefficient on a square, or a POWER of a data-weighted decision, is a diagonal Q HiGHS solves."""
        rows, cols = decidb_cli_highs.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(REAL)
            SUCH THAT x <= {_UB}
            MINIMIZE SUM({term} - c * x)
        """
        )
        oracle_solver.create_model(f"square_highs_{term}")
        _variables(oracle_solver, ["x"])
        oracle_solver.set_quadratic_objective(
            {f"x_{i}": -c for i, (_, _, c) in enumerate(_ROWS)},
            {(f"x_{i}", f"x_{i}"): weight(w) for i, (_, w, _) in enumerate(_ROWS)},
            ObjSense.MINIMIZE,
        )
        result = oracle_solver.solve()
        assert result.status == SolverStatus.OPTIMAL
        xs = _values(rows, cols, "x")
        actual = sum(weight(w) * x * x - c * x for (_, w, c), x in zip(_ROWS, xs))
        assert actual == pytest.approx(result.objective_value, rel=1e-4, abs=1e-4)

    def test_square_under_a_query_wide_factor(self, decidb_cli_gurobi, oracle_solver):
        """A factor on the reducer known only when the query runs is multiplied into the square's weight. Its sign is
        not known before then, so the square is solved as a data weight is, by Gurobi."""
        rows, cols = decidb_cli_gurobi.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(REAL)
            SUCH THAT x <= {_UB}
            MINIMIZE SUM(POWER(x, 2)) * (SELECT 0.5) - SUM(c * x)
        """
        )
        oracle_solver.create_model("square_query_wide_factor")
        _variables(oracle_solver, ["x"])
        oracle_solver.set_quadratic_objective(
            {f"x_{i}": -c for i, (_, _, c) in enumerate(_ROWS)},
            {(f"x_{i}", f"x_{i}"): 0.5 for i in range(len(_ROWS))},
            ObjSense.MINIMIZE,
        )
        result = oracle_solver.solve()
        assert result.status == SolverStatus.OPTIMAL
        xs = _values(rows, cols, "x")
        actual = sum(0.5 * x * x - c * x for (_, _, c), x in zip(_ROWS, xs))
        assert actual == pytest.approx(result.objective_value, rel=1e-4, abs=1e-4)

    def test_power_one_is_the_decision(self, decidb_cli, oracle_solver):
        """POWER(x, 1) is x, as SQL evaluates it."""
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(REAL)
            SUCH THAT x <= {_UB} AND SUM(POWER(x, 1)) <= 5
            MAXIMIZE SUM(c * x)
        """
        )
        oracle_solver.create_model("power_1")
        _variables(oracle_solver, ["x"])
        oracle_solver.add_constraint({f"x_{i}": 1.0 for i in range(len(_ROWS))}, "<=", 5.0, name="limit")
        oracle_solver.set_objective({f"x_{i}": c for i, (_, _, c) in enumerate(_ROWS)}, ObjSense.MAXIMIZE)
        result = oracle_solver.solve()
        assert result.status == SolverStatus.OPTIMAL
        xs = _values(rows, cols, "x")
        assert sum(c * x for (_, _, c), x in zip(_ROWS, xs)) == pytest.approx(result.objective_value, rel=1e-6)

    @pytest.mark.parametrize(
        "constraint,objective",
        [("POWER(x, 0) + x <= 3", "SUM(c * x)"), ("SUM(x) <= 5", "SUM(c * x + POWER(x, 0))")],
        ids=["per-row", "objective"],
    )
    def test_power_zero_is_one(self, decidb_cli, oracle_solver, constraint, objective):
        """POWER(x, 0) is 1, as SQL evaluates it."""
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(REAL)
            SUCH THAT x <= {_UB} AND {constraint}
            MAXIMIZE {objective}
        """
        )
        oracle_solver.create_model("power_0")
        _variables(oracle_solver, ["x"])
        if constraint.startswith("SUM"):
            oracle_solver.add_constraint({f"x_{i}": 1.0 for i in range(len(_ROWS))}, "<=", 5.0, name="limit")
        else:
            for i in range(len(_ROWS)):
                oracle_solver.add_constraint({f"x_{i}": 1.0}, "<=", 2.0, name=f"row_{i}")
        oracle_solver.set_objective({f"x_{i}": c for i, (_, _, c) in enumerate(_ROWS)}, ObjSense.MAXIMIZE)
        result = oracle_solver.solve()
        assert result.status == SolverStatus.OPTIMAL
        xs = _values(rows, cols, "x")
        assert sum(c * x for (_, _, c), x in zip(_ROWS, xs)) == pytest.approx(result.objective_value, rel=1e-6)

    @pytest.mark.parametrize("rows", [3, 5, 6], ids=["fewer rows", "as many rows", "more rows"])
    def test_power_zero_in_a_reducer_counts_rows(self, decidb_cli, oracle_solver, rows):
        """SUM(POWER(x, 0)) is the row count: the clause holds with 5 rows or fewer and is infeasible with more.

        The product keeps x at coefficient 0, so the reducer still reads a decision (ruling H)."""
        sql = f"""
            SELECT id, x FROM range(1, {rows + 1}) t(id) DECIDE x(REAL)
            SUCH THAT x <= {_UB} AND SUM(POWER(x, 0)) <= 5
            MAXIMIZE SUM(x)
        """
        oracle_solver.create_model(f"power_0_rows_{rows}")
        names = [f"x_{i}" for i in range(rows)]
        for name in names:
            oracle_solver.add_variable(name, VarType.CONTINUOUS, lb=0.0, ub=_UB)
        oracle_solver.add_constraint({name: 0.0 for name in names}, "<=", 5.0 - rows, name="row_count")
        oracle_solver.set_objective({name: 1.0 for name in names}, ObjSense.MAXIMIZE)
        result = oracle_solver.solve()
        if result.status == SolverStatus.INFEASIBLE:
            refused = decidb_cli.execute_raw(sql)
            assert "infeasible" in refused.stderr, refused.stderr
            return
        assert result.status == SolverStatus.OPTIMAL
        out, cols = decidb_cli.execute(sql)
        xi = cols.index("x")
        assert sum(float(r[xi]) for r in out) == pytest.approx(result.objective_value, rel=1e-6)


def _canonical(decidb_cli, constraint):
    """The plan's text, without box glyphs or whitespace, for a query with this constraint."""
    out = decidb_cli.execute_raw(
        f"SET decide_direct_solve='off'; EXPLAIN SELECT id, x FROM {_TABLE} DECIDE x(REAL) "
        f"SUCH THAT x <= {_UB} AND {constraint} MAXIMIZE SUM(x)"
    ).stdout
    return "".join("".join(" " if ch in "│┌┐└┘├┤┬┴┼─" else ch for ch in out).split())


@pytest.mark.explain
class TestOneSpellingOfAWholeExponent:
    """A whole exponent is written as its number however it was spelled, so `POWER(x, CAST(2 AS INTEGER))` and
    `POWER(x, 2)` are one term and merge as like terms do. A square in a limit needs Gurobi."""

    @pytest.mark.parametrize(
        "constraint, canonical",
        [
            ("POWER(x, CAST(2 AS INTEGER)) + POWER(x, 2) <= 5", "≡2*power(x,2)<=5"),
            ("SUM(POWER(x, CAST(2 AS INTEGER))) + SUM(POWER(x, 2)) <= 5", "≡2*sum(power(x,2))<=5"),
            ("SUM(POWER(x, CAST(2 AS INTEGER)) + POWER(x, 2)) <= 5", "≡sum(power(x,2)+power(x,2))<=5"),
        ],
        ids=["per row", "over reducers", "inside a reducer"],
    )
    def test_a_cast_exponent_is_the_number(self, decidb_cli_gurobi, canonical, constraint):
        packed = _canonical(decidb_cli_gurobi, constraint)
        assert canonical in packed, packed


@pytest.mark.correctness
class TestBooleanProducts:
    """A product of BOOL decisions takes the general path: a square of b is b * b, linearized like b * d."""

    @pytest.mark.parametrize(
        "term,q",
        [
            ("b*b", lambda i: {("b", "b"): 1.0}),
            ("(b+d)*(d+b)", lambda i: {("b", "b"): 1.0, ("b", "d"): 2.0, ("d", "d"): 1.0}),
        ],
        ids=["self product", "equal sums merge"],
    )
    def test_boolean_square_answers(self, decidb_cli, oracle_solver, term, q):
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, b, d FROM {_TABLE} DECIDE b(BOOL), d(BOOL)
            SUCH THAT SUM(d) <= 2
            MAXIMIZE SUM(c * b + w * d - {term})
        """
        )
        oracle_solver.create_model(f"bool_{term}")
        _variables(oracle_solver, ["b", "d"], VarType.BINARY)
        linear, quadratic = {}, {}
        for i, (_, w, c) in enumerate(_ROWS):
            linear[f"b_{i}"] = c
            linear[f"d_{i}"] = w
            for (a, b), coeff in q(i).items():
                quadratic[(f"{a}_{i}", f"{b}_{i}")] = -coeff
        oracle_solver.add_constraint({f"d_{i}": 1.0 for i in range(len(_ROWS))}, "<=", 2.0, name="d_limit")
        oracle_solver.set_quadratic_objective(linear, quadratic, ObjSense.MAXIMIZE)
        result = oracle_solver.solve()
        assert result.status == SolverStatus.OPTIMAL
        bs, ds = _values(rows, cols, "b"), _values(rows, cols, "d")
        square = (lambda b, d: b * b) if term == "b*b" else (lambda b, d: (b + d) ** 2)
        actual = sum(c * b + w * d - square(b, d) for (_, w, c), b, d in zip(_ROWS, bs, ds))
        assert actual == pytest.approx(result.objective_value, abs=1e-6)


@pytest.mark.error
class TestShapeCheckRefusals:
    """One refusal per kind the shape check judges, each quoting the clause and pointing at the term."""

    @pytest.mark.parametrize(
        "constraint,message",
        [
            (
                "SUM(x*x*y) <= 10",
                r"Invalid Input Error: DECIDE: the SUCH THAT clause `SUM\(x\*x\*y\) <= 10` has a term " r"of degree 3",
            ),
            ("SUM(POWER(x, 3)) <= 10", r"has a term of degree 3"),
            (
                "SUM(POWER(x, 0.5)) <= 10",
                r"POWER over a DECIDE variable needs an exponent that is a whole number known "
                r"before the query runs, found '0\.5'",
            ),
            ("SUM(EXP(x)) <= 10", r"DECIDE: function 'exp' cannot take a DECIDE variable\."),
            ("SUM(x / y) <= 10", r"DECIDE: cannot divide by a DECIDE variable\."),
            ("SUM(x % 2) <= 10", r"DECIDE: operator '%' cannot take a DECIDE variable\."),
            ("SUM(CASE WHEN w > 1 THEN x ELSE y END) <= 10", r"CASE over a DECIDE variable is not supported"),
        ],
        ids=["degree", "degree of a power", "exponent", "scalar function", "division", "operator", "case"],
    )
    def test_refusal_quotes_the_clause_and_points_at_the_term(self, decidb_cli, constraint, message):
        result = decidb_cli.execute_raw(
            f"""
            SELECT id, x, y FROM {_TABLE} DECIDE x(REAL), y(REAL)
            SUCH THAT x <= {_UB} AND y <= {_UB} AND {constraint}
            MAXIMIZE SUM(x + y)
        """
        )
        assert result.returncode != 0
        assert re.search(message, result.stderr), result.stderr
        assert f"In the SUCH THAT clause `{constraint}`" in result.stderr or "has a term of degree" in result.stderr
        assert "LINE " in result.stderr and "^" in result.stderr, result.stderr

    def test_data_weight_on_a_square_is_refused_on_highs(self, decidb_cli_highs):
        """A data weight's sign is known only when the query runs, so the square may be non-convex: HiGHS cannot
        take it, and the gate says so before a row is read."""
        result = decidb_cli_highs.execute_raw(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(REAL)
            SUCH THAT x <= {_UB}
            MINIMIZE SUM(w * POWER(x, 2) - c * x)
        """
        )
        assert result.returncode != 0
        assert (
            "the objective multiplies a squared decision term by a value whose sign is known only when the query runs"
            in result.stderr
        ), result.stderr

    def test_objective_refusal_quotes_the_objective_as_written(self, decidb_cli):
        """The objective is quoted from the parser's OBJECTIVE span, spacing and case as typed."""
        result = decidb_cli.execute_raw(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(REAL)
            SUCH THAT x <= {_UB}
            MINIMIZE SUM(POWER(x,3))
        """
        )
        assert "DECIDE: the objective `SUM(POWER(x,3))` has a term of degree 3" in result.stderr, result.stderr
        assert "^" in result.stderr, result.stderr
