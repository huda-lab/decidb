"""CASE inside DECIDE: over data it is a per-row coefficient, over a decision it is refused.

A CASE that reads no decision is data, as COALESCE is: DuckDB evaluates it per row and the
clause reads its value. Each answer test solves the same model with the CASE's value computed
in Python and compares DeciDB's objective with the oracle's.

A CASE that reads a decision has no reading in the model. The shape check refuses it after
canonicalization with one message that names the alternatives (a scope in front, `per`, or a
CTE), quotes the clause and points at the CASE; it never surfaces as an internal error.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType

from ._output_helpers import assert_no_internal_leak

# (id, nat, bal): `nat` picks the CASE's branch, `bal` is a weight.
_ROWS = [(1, 1, 10.0), (2, 5, 20.0), (3, 1, 5.0), (4, 7, 8.0), (5, 5, 12.0)]
_TABLE = "(VALUES (1, 1, 10.0), (2, 5, 20.0), (3, 1, 5.0), (4, 7, 8.0), (5, 5, 12.0)) t(id, nat, bal)"

_CASE = "CASE WHEN nat = 1 THEN 1 ELSE 2 END"


def _case(nat):
    return 1 if nat == 1 else 2


def _names():
    return [f"x_{i}" for i in range(len(_ROWS))]


def _solve(oracle_solver, name, var_type, constraints, objective, sense, ub=None, extra=()):
    """`constraints` and `objective` are over x_0..x_n (and `extra` variables, given as (name, type, lb, ub))."""
    oracle_solver.create_model(name)
    for n in _names():
        oracle_solver.add_variable(n, var_type, lb=0.0, ub=ub)
    for n, t, lb, u in extra:
        oracle_solver.add_variable(n, t, lb=lb, ub=u)
    for i, (coeffs, op, rhs) in enumerate(constraints):
        oracle_solver.add_constraint(coeffs, op, float(rhs), name=f"c{i}")
    oracle_solver.set_objective(objective, sense)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    return result.objective_value


def _x(rows, cols):
    ci = {c: i for i, c in enumerate(cols)}
    by_id = {int(r[ci["id"]]): float(r[ci["x"]]) for r in rows}
    return [by_id[i] for i, _, _ in _ROWS]


def _dot(weights, values):
    return sum(w * v for w, v in zip(weights, values))


def _all(coefficient=lambda row: 1.0):
    return {n: float(coefficient(row)) for n, row in zip(_names(), _ROWS)}


@pytest.mark.correctness
class TestCaseOverDataIsACoefficient:
    """A CASE over data answers as the per-row value it computes."""

    def test_case_in_such_that_answers(self, decidb_cli, oracle_solver):
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(BOOL)
            SUCH THAT SUM(x * CASE WHEN nat = 1 THEN 1 ELSE 0 END) <= 1
            MAXIMIZE SUM(x * bal)
        """
        )
        expected = _solve(
            oracle_solver,
            "case_such_that",
            VarType.BINARY,
            [(_all(lambda r: 1 if r[1] == 1 else 0), "<=", 1)],
            _all(lambda r: r[2]),
            ObjSense.MAXIMIZE,
        )
        assert _dot([r[2] for r in _ROWS], _x(rows, cols)) == pytest.approx(expected)

    def test_simple_case_form_answers(self, decidb_cli, oracle_solver):
        """`CASE expr WHEN val THEN ... END` binds to the same class as the searched form."""
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(BOOL)
            SUCH THAT SUM(x * CASE nat WHEN 1 THEN 1 ELSE 0 END) <= 1
            MAXIMIZE SUM(x * bal)
        """
        )
        expected = _solve(
            oracle_solver,
            "case_simple",
            VarType.BINARY,
            [(_all(lambda r: 1 if r[1] == 1 else 0), "<=", 1)],
            _all(lambda r: r[2]),
            ObjSense.MAXIMIZE,
        )
        assert _dot([r[2] for r in _ROWS], _x(rows, cols)) == pytest.approx(expected)

    @pytest.mark.parametrize("sense", [ObjSense.MAXIMIZE, ObjSense.MINIMIZE])
    def test_case_in_the_objective_answers(self, decidb_cli, oracle_solver, sense):
        word, cmp = ("MAXIMIZE", "<=") if sense == ObjSense.MAXIMIZE else ("MINIMIZE", ">=")
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(BOOL)
            SUCH THAT SUM(x) {cmp} 3
            {word} SUM(x * CASE WHEN nat = 1 THEN 100 ELSE 1 END)
        """
        )
        weight = lambda r: 100 if r[1] == 1 else 1
        expected = _solve(oracle_solver, f"case_obj_{word}", VarType.BINARY, [(_all(), cmp, 3)], _all(weight), sense)
        assert _dot([weight(r) for r in _ROWS], _x(rows, cols)) == pytest.approx(expected)

    def test_case_inside_avg_answers(self, decidb_cli, oracle_solver):
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(BOOL)
            SUCH THAT SUM(x) <= 3
            MAXIMIZE AVG(x * {_CASE})
        """
        )
        n = len(_ROWS)
        expected = _solve(
            oracle_solver,
            "case_avg",
            VarType.BINARY,
            [(_all(), "<=", 3)],
            _all(lambda r: _case(r[1]) / n),
            ObjSense.MAXIMIZE,
        )
        assert _dot([_case(r[1]) / n for r in _ROWS], _x(rows, cols)) == pytest.approx(expected)

    @pytest.mark.parametrize("agg", ["MIN", "MAX"])
    def test_case_inside_min_max_answers(self, decidb_cli, oracle_solver, agg):
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(BOOL)
            SUCH THAT SUM(x) <= 3
            MAXIMIZE {agg}(x * {_CASE})
        """
        )
        values = [_case(r[1]) * v for r, v in zip(_ROWS, _x(rows, cols))]
        # t is the extremum: t <= every member for MIN; for MAX, t <= the member z picks.
        extra = [("t", VarType.CONTINUOUS, 0.0, 2.0)]
        if agg == "MIN":
            constraints = [({"t": 1.0, n: -float(_case(r[1]))}, "<=", 0) for n, r in zip(_names(), _ROWS)]
        else:
            extra += [(f"z_{i}", VarType.BINARY, 0.0, 1.0) for i in range(len(_ROWS))]
            constraints = [
                ({"t": 1.0, n: -float(_case(r[1])), f"z_{i}": 2.0}, "<=", 2)
                for i, (n, r) in enumerate(zip(_names(), _ROWS))
            ]
            constraints.append(({f"z_{i}": 1.0 for i in range(len(_ROWS))}, "=", 1))
        constraints.append((_all(), "<=", 3))
        expected = _solve(
            oracle_solver, f"case_{agg}", VarType.BINARY, constraints, {"t": 1.0}, ObjSense.MAXIMIZE, extra=extra
        )
        assert (min(values) if agg == "MIN" else max(values)) == pytest.approx(expected)

    def test_case_inside_per_grouped_constraint_answers(self, decidb_cli, oracle_solver):
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(BOOL)
            SUCH THAT per nat: SUM(x * {_CASE}) by (nat) <= 2
            MAXIMIZE SUM(x * bal)
        """
        )
        constraints = []
        for nat in sorted({r[1] for r in _ROWS}):
            constraints.append(({n: float(_case(r[1])) for n, r in zip(_names(), _ROWS) if r[1] == nat}, "<=", 2))
        expected = _solve(
            oracle_solver, "case_per", VarType.BINARY, constraints, _all(lambda r: r[2]), ObjSense.MAXIMIZE
        )
        assert _dot([r[2] for r in _ROWS], _x(rows, cols)) == pytest.approx(expected)

    def test_case_outside_a_reducer_answers(self, decidb_cli, oracle_solver):
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(INT)
            SUCH THAT x + {_CASE} <= 5
            MAXIMIZE SUM(x)
        """
        )
        constraints = [({n: 1.0}, "<=", 5 - _case(r[1])) for n, r in zip(_names(), _ROWS)]
        expected = _solve(oracle_solver, "case_per_row", VarType.INTEGER, constraints, _all(), ObjSense.MAXIMIZE)
        assert sum(_x(rows, cols)) == pytest.approx(expected)

    def test_a_query_wide_case_is_an_objective_constant(self, decidb_cli, oracle_solver):
        """A CASE over no row at all is a constant added to the objective, which moves no decision."""
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(BOOL)
            SUCH THAT SUM(x) <= 3
            MAXIMIZE SUM(x * bal) + CASE WHEN 1 = 1 THEN 1 ELSE 2 END
        """
        )
        expected = _solve(
            oracle_solver, "case_constant", VarType.BINARY, [(_all(), "<=", 3)], _all(lambda r: r[2]), ObjSense.MAXIMIZE
        )
        assert _dot([r[2] for r in _ROWS], _x(rows, cols)) == pytest.approx(expected)


@pytest.mark.error
class TestCaseOverADecisionIsRefused:
    """A CASE that reads a decision is refused by the shape check, with the alternatives named."""

    _MESSAGE = (
        "CASE over a DECIDE variable is not supported. Write the condition as a scope in front of the clause "
        "(when category = 'A': sum(x) >= 1), split rows into classes with per, or pre-compute conditional values "
        "in a CTE or subquery before the DECIDE clause."
    )

    @pytest.mark.parametrize(
        "constraint,objective",
        [
            ("SUM(CASE WHEN nat = 1 THEN x ELSE 0 END) >= 1", "SUM(x * bal)"),
            ("x <= 1", "SUM(CASE WHEN nat = 1 THEN x ELSE 0 END)"),
            ("x <= CASE WHEN nat = 1 THEN 1 - x ELSE 1 END", "SUM(x * bal)"),
        ],
        ids=["reducer argument", "objective", "per-row side"],
    )
    def test_case_over_a_decision_is_refused(self, decidb_cli, constraint, objective):
        result = decidb_cli.execute_raw(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(BOOL)
            SUCH THAT {constraint}
            MAXIMIZE {objective}
        """
        )
        assert self._MESSAGE in result.stderr, result.stderr
        assert "LINE " in result.stderr, result.stderr
        assert_no_internal_leak(result, "CASE over a decision")

    def test_the_refusal_quotes_the_clause_as_written(self, decidb_cli):
        result = decidb_cli.execute_raw(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(BOOL)
            SUCH THAT SUM(CASE WHEN nat = 1 THEN x ELSE 0 END) >= 1
            MAXIMIZE SUM(x * bal)
        """
        )
        assert (
            "In the SUCH THAT clause `SUM(CASE WHEN nat = 1 THEN x ELSE 0 END) >= 1`." in result.stderr
        ), result.stderr

    def test_case_in_constraint_does_not_emit_stack_trace(self, decidb_cli):
        """No C++ trace, the symptom that motivated the refusal, whatever the CASE reads."""
        sql = f"""
            SELECT id, x FROM {_TABLE} DECIDE x(BOOL)
            SUCH THAT SUM(x * CASE WHEN nat = 1 THEN x ELSE 0 END) >= 1
            MAXIMIZE SUM(x * bal)
        """
        assert_no_internal_leak(decidb_cli.execute_raw(sql), "CASE rejection")


def _per_row_upper(bound_of):
    return [(_all(), "<=", bound_of(r)) for r in _ROWS]


@pytest.mark.correctness
class TestCaseAnswersEverywhere:
    """A CASE over data answers wherever the clause reads data (BUG-42's positions)."""

    # (constraints, objective, the oracle's constraints over x_0..x_n). A per-row bound of an aggregate clause holds
    # on every row, so the oracle states it once per row.
    _ANSWERED = {
        "aggregate bound": (f"SUM(x) <= {_CASE}", _per_row_upper(lambda r: _case(r[1]))),
        "aggregate bound, left side": (f"{_CASE} >= SUM(x)", _per_row_upper(lambda r: _case(r[1]))),
        "aggregate bound, in arithmetic": (f"SUM(x) <= 1 + {_CASE}", _per_row_upper(lambda r: 1 + _case(r[1]))),
        "aggregate bound, in a call": (f"SUM(x) <= abs({_CASE})", _per_row_upper(lambda r: _case(r[1]))),
        "aggregate bound, in a cast": (f"SUM(x) <= CAST({_CASE} AS INT)", _per_row_upper(lambda r: _case(r[1]))),
        "aggregate bound, in a lambda": (
            "SUM(x) <= list_reduce([1, 2], (a, b) -> CASE WHEN a > b THEN a ELSE b END)",
            [(_all(), "<=", 2)],
        ),
        "aggregate bound, in a BETWEEN": (
            f"SUM(x) BETWEEN 0 AND {_CASE}",
            _per_row_upper(lambda r: _case(r[1])) + [(_all(), ">=", 0)],
        ),
        "aggregate bound, in a later conjunct": (
            f"SUM(x) <= 5 AND SUM(x) >= {_CASE}",
            [(_all(), "<=", 5)] + [(_all(), ">=", _case(r[1])) for r in _ROWS],
        ),
        "per-row bound": (f"x <= {_CASE}", [({n: 1.0}, "<=", _case(r[1])) for n, r in zip(_names(), _ROWS)]),
        "reducer argument": (f"SUM(x * {_CASE}) <= 3", [(_all(lambda r: _case(r[1])), "<=", 3)]),
        "bound of a clause with when": (
            f"x <= 4 AND when id > 2: SUM(x) <= {_CASE}",
            [({n: 1.0}, "<=", 4) for n in _names()]
            + [({n: 1.0 for n, q in zip(_names(), _ROWS) if q[0] > 2}, "<=", _case(r[1])) for r in _ROWS if r[0] > 2],
        ),
        "reducer argument under by": (
            f"SUM(x * {_CASE}) by (nat) <= 3",
            [
                ({n: float(_case(r[1])) for n, r in zip(_names(), _ROWS) if r[1] == nat}, "<=", 3)
                for nat in sorted({r[1] for r in _ROWS})
            ],
        ),
    }

    @pytest.mark.parametrize("position", list(_ANSWERED))
    def test_case_over_data_answers(self, decidb_cli, oracle_solver, position):
        constraint, oracle_constraints = self._ANSWERED[position]
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(INT)
            SUCH THAT {constraint}
            MAXIMIZE SUM(x)
        """
        )
        expected = _solve(
            oracle_solver, f"case_{position}", VarType.INTEGER, oracle_constraints, _all(), ObjSense.MAXIMIZE
        )
        assert sum(_x(rows, cols)) == pytest.approx(expected), position

    def test_case_in_the_objective_answers(self, decidb_cli, oracle_solver):
        rows, cols = decidb_cli.execute(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(INT)
            SUCH THAT SUM(x) <= 3
            MAXIMIZE SUM(x * {_CASE})
        """
        )
        expected = _solve(
            oracle_solver,
            "case_objective_coefficient",
            VarType.INTEGER,
            [(_all(), "<=", 3)],
            _all(lambda r: _case(r[1])),
            ObjSense.MAXIMIZE,
        )
        assert _dot([_case(r[1]) for r in _ROWS], _x(rows, cols)) == pytest.approx(expected)

    # Positions a refusal that has nothing to do with CASE reaches first: a reducer's FILTER is not supported yet,
    # and a per-row value outside a reducer is no term of an objective.
    _REFUSED_FIRST = {
        "aggregate bound, in a reducer's FILTER": (
            f"SUM(x) <= SUM(id) FILTER (WHERE {_CASE} = 1)",
            "SUM(x)",
            "DECIDE clause does not support 'sum(id) FILTER",
        ),
        "decision reducer's FILTER": (
            f"SUM(x) FILTER (WHERE {_CASE} = 1) <= 3",
            "SUM(x)",
            "DECIDE clause does not support 'sum(x) FILTER",
        ),
        "objective term": ("SUM(x) <= 3", f"SUM(x) + {_CASE}", "does not reference a decision"),
        "objective under when": ("SUM(x) <= 3", f"when id > 2: SUM(x) + {_CASE}", "does not reference a decision"),
    }

    @pytest.mark.parametrize("position", list(_REFUSED_FIRST))
    def test_another_refusal_reaches_the_case_first(self, decidb_cli, position):
        constraint, objective, message = self._REFUSED_FIRST[position]
        result = decidb_cli.execute_raw(
            f"""
            SELECT id, x FROM {_TABLE} DECIDE x(INT)
            SUCH THAT {constraint}
            MAXIMIZE {objective}
        """
        )
        assert message in result.stderr, f"{position}: {result.stderr[:600]}"

    # Positions that hold SQL over the rows, not the clause's algebra: a CASE there is the user's own filter or key.
    _ACCEPTED = {
        "when condition": f"when {_CASE} = 1: SUM(x) <= 3",
        "by key": f"SUM(x) by ({_CASE}) <= 3",
        "scalar subquery bound": f"SUM(x) <= (SELECT {_CASE} FROM {_TABLE} WHERE id = 1)",
    }

    @pytest.mark.parametrize("position", list(_ACCEPTED))
    def test_case_in_sql_positions_is_still_accepted(self, decidb_cli, position):
        sql = f"""
            SELECT id, x FROM {_TABLE} DECIDE x(BOOL)
            SUCH THAT {self._ACCEPTED[position]}
            MAXIMIZE SUM(x * bal)
        """
        rows, _ = decidb_cli.execute(sql)
        assert rows, position
