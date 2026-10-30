"""A coefficient's magnitude is the solver's problem, not the user's.

HiGHS enforces a magnitude window on every constraint-matrix entry inside
``passModel``, at both ends and with different consequences. An entry at or below
``small_matrix_value`` (1e-9) is deleted from the matrix and ``passModel`` returns
``kWarning``; an entry at or above ``large_matrix_value`` (1e15) makes it return
``kError``. Gurobi has no such window. The loader treated every non-``kOk`` status as
fatal, so both edges surfaced as ``INTERNAL Error: Failed to pass model to HiGHS``
with a stack trace, and an ordinary query worked or crashed depending on which solver
happened to be installed.

Neither edge is a malformed query. A per-unit rate of 1e-9 is data — a rate, a cost in
the wrong unit. The heavy end is reached by coefficients DeciDB generates itself: the
Big-M closing ``<>`` / ``MIN`` / ``MAX`` is the decision's own span, so ``x <= 4e15``
puts a 4e15 entry in the matrix while every number the user typed is in range. The
infeasible engine reaches the low end the same way — its tier-1 weights are
``ref / rms(Aᵢ)``, so a row with 1e9-scale coefficients earns a 1e-9 weight, and that
weight becomes a matrix entry in the stage-2 budget row.

Each out-of-window row is now multiplied through by its own power of two, bounds
included, before the matrix is packed. Scaling a row by a positive constant leaves the
constraint's meaning exactly intact, so it cannot change an answer; rounding a
sub-tolerance entry to zero — which is what HiGHS itself does — can, and
``test_a_tiny_coefficient_on_a_floor_is_not_rounded_away`` is the case that proves it.

Covers:
  - test_a_tiny_coefficient_is_not_dropped: the original crash, now a result
  - test_a_tiny_coefficient_on_a_floor_is_not_rounded_away: rounding would say infeasible
  - test_a_large_data_coefficient_loads: the heavy edge, from ordinary data
  - test_a_generated_big_m_above_the_ceiling_loads: the heavy edge, from our own Big-M
  - test_the_backends_agree_across_the_window: no answer depends on the host's solver
  - test_a_heavy_row_diagnosis_names_a_repair: the stage-2 budget row loads
  - test_a_bound_past_the_solvers_infinity_is_answered: a limit HiGHS cannot hold
  - test_a_bound_past_infinity_on_the_open_side_still_solves: and still constrains
  - test_a_bound_no_scaling_can_reach_is_refused: 1e40 is not the 1e30 sentinel
  - test_a_row_spanning_the_window_is_refused_in_sql_terms: scaling has a limit
  - test_a_bound_too_large_to_scale_is_refused_in_sql_terms: so does the bound it carries
  - test_a_column_or_objective_outside_the_window_is_solved: a column bound or objective no row scaling reaches
  - test_a_scaled_columns_open_side_stays_open: the column's "no bound" side is not scaled into a bound
  - test_an_objective_spanning_the_window_is_refused_in_sql_terms: the objective's scaling has a limit too
  - test_a_repair_model_outside_the_window_names_the_repair: and under DIAGNOSE, the repair model loads
  - test_a_repair_model_highs_refuses_is_reported_as_such: one it refuses is no answer about the repair
"""

import pytest

from solver.types import VarType, ObjSense, SolverStatus


# `SUM(w * x)` over two rows, as both DeciDB SQL and an oracle model. The weight and the
# bound are what each test varies; everything else is held still so the only thing under
# test is where the coefficient lands relative to HiGHS's window.
def _weighted_sum_sql(weight, box, sense, rhs, obj):
    return f"""
        SELECT id, x FROM (VALUES (1,{weight}),(2,{weight})) t(id,w)
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= {box} AND SUM(w * x) {sense} {rhs}
        {obj} SUM(x)
    """


def _weighted_sum_oracle(oracle, name, weight, box, sense, rhs, obj):
    oracle.create_model(name)
    for i in range(2):
        oracle.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=box)
    oracle.add_constraint({f"x_{i}": weight for i in range(2)}, sense, rhs, name="row")
    oracle.set_objective({f"x_{i}": 1.0 for i in range(2)}, obj)
    return oracle.solve()


@pytest.mark.edge_case
@pytest.mark.correctness
def test_a_tiny_coefficient_is_not_dropped(decidb_cli_highs, oracle_solver):
    """`SUM(w * x) <= 1` with `w = 1e-9` is an ordinary query with an ordinary answer.

    This is the query the bug was reported on. It crashed because HiGHS deleted both
    matrix entries and said so with a `kWarning` the loader read as fatal.
    """
    rows, _ = decidb_cli_highs.execute(
        _weighted_sum_sql("0.000000001", 5, "<=", 1, "MAXIMIZE"))
    result = _weighted_sum_oracle(
        oracle_solver, "tiny_cap", 1e-9, 5.0, "<=", 1.0, ObjSense.MAXIMIZE)

    assert result.status == SolverStatus.OPTIMAL
    assert sum(r[1] for r in rows) == pytest.approx(result.objective_value, rel=1e-9)


@pytest.mark.edge_case
@pytest.mark.correctness
def test_a_tiny_coefficient_on_a_floor_is_not_rounded_away(decidb_cli_highs, oracle_solver):
    """The case that rules out rounding a sub-tolerance entry to zero.

    `1e-9·x₁ + 1e-9·x₂ >= 1` is satisfiable — `x` reaches 1e10, so the row needs a
    total of 1e9. Round both coefficients to zero and the row becomes `0 >= 1`, and a
    query with a perfectly good answer is reported infeasible. Scaling the row up
    instead keeps the answer, which is why the fix scales rather than rounds.

    The split between the two variables is degenerate — either can carry the whole
    total — so the objective is what the oracle pins, not the individual values.
    """
    rows, _ = decidb_cli_highs.execute(
        _weighted_sum_sql("1e-9", "1e10", ">=", 1, "MINIMIZE"), expect_large_numbers=True
    )
    result = _weighted_sum_oracle(
        oracle_solver, "tiny_floor", 1e-9, 1e10, ">=", 1.0, ObjSense.MINIMIZE)

    assert result.status == SolverStatus.OPTIMAL
    assert sum(r[1] for r in rows) == pytest.approx(result.objective_value, rel=1e-6)


@pytest.mark.edge_case
@pytest.mark.correctness
def test_a_large_data_coefficient_loads(decidb_cli_highs, oracle_solver):
    """`w = 1e16` is above HiGHS's ceiling, and is still just a number in a column."""
    rows, _ = decidb_cli_highs.execute(
        _weighted_sum_sql("1e16", 5, "<=", "1e20", "MAXIMIZE"), expect_large_numbers=True
    )
    result = _weighted_sum_oracle(
        oracle_solver, "large_cap", 1e16, 5.0, "<=", 1e20, ObjSense.MAXIMIZE)

    assert result.status == SolverStatus.OPTIMAL
    assert sum(r[1] for r in rows) == pytest.approx(result.objective_value, rel=1e-9)


# `MAX(x) >= 3` closes over a Big-M taken from `x`'s own span, so the bound below puts a
# 4e15 entry in the matrix while nothing in the query is written above 4e15. Oracling it
# would mean re-deriving the indicator formulation under test, so the assertion here is
# that the two backends agree — which is the property the bug actually broke.
_BIG_M_SQL = """
    SELECT id, x FROM (VALUES (1),(2)) t(id)
    DECIDE x(REAL)
    SUCH THAT x >= 0 AND x <= {box} AND MAX(x) >= 3
    MAXIMIZE SUM(x)
"""


@pytest.mark.edge_case
@pytest.mark.min_max
def test_a_generated_big_m_above_the_ceiling_loads(decidb_cli_highs):
    """A Big-M over the ceiling is DeciDB's number, not the user's, and must load.

    A bound of 9e14 always worked and 1e15 did not, which is HiGHS's
    `large_matrix_value` showing through as a cliff in DECIDE's own syntax.
    """
    below, _ = decidb_cli_highs.execute(_BIG_M_SQL.format(box="900000000000000"), expect_large_numbers=True)
    above, _ = decidb_cli_highs.execute(_BIG_M_SQL.format(box="4000000000000000"), expect_large_numbers=True)

    assert [r[1] for r in below] == pytest.approx([9e14, 9e14], rel=1e-9)
    assert [r[1] for r in above] == pytest.approx([4e15, 4e15], rel=1e-9)


@pytest.mark.edge_case
@pytest.mark.correctness
@pytest.mark.parametrize(
    "sql, large",
    [
        pytest.param(_weighted_sum_sql("0.000000001", 5, "<=", 1, "MAXIMIZE"), False, id="tiny-cap"),
        pytest.param(_weighted_sum_sql("1e-9", "1e10", ">=", 1, "MINIMIZE"), True, id="tiny-floor"),
        pytest.param(_weighted_sum_sql("1e16", 5, "<=", "1e20", "MAXIMIZE"), True, id="large-cap"),
        pytest.param(_BIG_M_SQL.format(box="4000000000000000"), True, id="big-m"),
    ],
)
def test_the_backends_agree_across_the_window(decidb_cli_highs, decidb_cli_gurobi, sql, large):
    """Which solver is installed must not decide whether a query has an answer.

    Row scaling multiplies by a power of two, which is exact in binary floating point,
    so the two backends solve bit-identical constraints and the totals match to the
    solvers' own tolerances rather than approximately.
    """
    highs_rows, _ = decidb_cli_highs.execute(sql, expect_large_numbers=large)
    gurobi_rows, _ = decidb_cli_gurobi.execute(sql, expect_large_numbers=large)

    assert sum(r[1] for r in highs_rows) == pytest.approx(
        sum(r[1] for r in gurobi_rows), rel=1e-6)


@pytest.mark.edge_case
@pytest.mark.query_diagnostics
def test_a_heavy_row_diagnosis_names_a_repair(decidb_cli_highs):
    """A 1e9-scale row makes the engine weight a repair knob at 1e-9.

    That weight is a matrix entry in the stage-2 budget row, so this query used to end
    at "the solver could not analyse the repair model" on HiGHS while Gurobi named the
    edit — a diagnosis that declined for a reason having nothing to do with the query.
    """
    rows, cols = decidb_cli_highs.execute(
        """
        DIAGNOSE SELECT id, x AS contrib
        FROM (VALUES (1,1e9),(2,1e9),(3,1e9)) t(id,w)
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 5000 AND SUM(w * x) <= 1e15 AND SUM(x) >= 30000
        MAXIMIZE SUM(x)
    """,
        expect_large_numbers=True,
    )

    clause = cols.index("clause")
    change = cols.index("suggested_change")
    assert not any("could not analyse" in str(r[change]) for r in rows)
    assert any(r[clause] == "x <= 5000" and r[change] == "x <= 10000" for r in rows), rows


# A row bound has a window of its own: HiGHS reads |bound| >= `infinite_bound` (1e20) as
# ±infinity, which on one side of a row is an error and on the other silently drops the
# constraint. A row carrying such a limit is therefore scaled DOWN even though its
# coefficients were never out of range, and the scaling is capped so that a bound which
# was representable stays representable — a rescue that deletes a row would be the same
# silent model change the dropped-coefficient case was.
_BOUND_SQL = """
    SELECT id, x FROM (VALUES (1),(2)) t(id)
    DECIDE x(REAL)
    SUCH THAT x >= 0 AND x <= 5 AND {clause}
    MAXIMIZE SUM(x)
"""


@pytest.mark.edge_case
@pytest.mark.error_infeasible
@pytest.mark.parametrize("clause", ["SUM(x) >= 1e25", "SUM(x) <= -1e25", "SUM(x) = 1e25"])
def test_a_bound_past_the_solvers_infinity_is_answered(
    decidb_cli_highs, decidb_cli_gurobi, clause
):
    """A finite limit no assignment can reach is an infeasible query, not a load failure.

    `SUM(x) >= 1e25` is unsatisfiable against `x <= 5`, and Gurobi has always said so.
    HiGHS cannot hold the bound as a number, so the row used to die at `passModel` with
    an internal error instead. Scaling the row down brings the bound back under
    `infinite_bound` and HiGHS answers the question it was asked.
    """
    sql = _BOUND_SQL.format(clause=clause)
    decidb_cli_highs.assert_error(sql, match=r"(?i)infeasible", expect_large_numbers=True)
    decidb_cli_gurobi.assert_error(sql, match=r"(?i)infeasible", expect_large_numbers=True)


@pytest.mark.edge_case
@pytest.mark.correctness
@pytest.mark.parametrize("clause", ["SUM(x) <= 1e25", "SUM(x) >= -1e25"])
def test_a_bound_past_infinity_on_the_open_side_still_solves(
    decidb_cli_highs, decidb_cli_gurobi, clause
):
    """The same magnitude pointing the other way is satisfiable, and still solves.

    These two never crashed — HiGHS reads the bound as ±infinity and drops the row rather
    than erroring — so they pin the other half of the rule: the fix must not turn a query
    that answered into one that refuses, whichever direction the oversized limit points.
    """
    sql = _BOUND_SQL.format(clause=clause)
    highs_rows, _ = decidb_cli_highs.execute(sql, expect_large_numbers=True)
    gurobi_rows, _ = decidb_cli_gurobi.execute(sql, expect_large_numbers=True)

    assert sum(r[1] for r in highs_rows) == pytest.approx(
        sum(r[1] for r in gurobi_rows), rel=1e-9)


@pytest.mark.edge_case
@pytest.mark.error
@pytest.mark.parametrize("clause", ["SUM(x) >= 1e40", "SUM(x) <= -1e40", "SUM(x) >= 1e30"])
def test_a_bound_no_scaling_can_reach_is_refused(decidb_cli_highs, clause):
    """A limit so far out that scaling it back drags the coefficients under the floor.

    These are the bounds that pin how the limit is READ. HiGHS spells "no bound on this
    side" as ±1e30, so a row's open side always carries that value — and a user limit of
    1e40, or of 1e30 exactly, is indistinguishable from the sentinel by magnitude alone.
    Guessing from the packed range row let a real limit be mistaken for "no bound" and
    passed straight through to the internal error it was supposed to prevent. The limit
    is read from the clause instead, and the sense says which side it lands on.
    """
    decidb_cli_highs.assert_error(
        f"""
        SELECT id, x FROM (VALUES (1),(2)) t(id)
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 5 AND {clause}
        MAXIMIZE SUM(x)
    """,
        match=r"sets a limit of .* that HiGHS cannot hold",
        expect_large_numbers=True,
    )


@pytest.mark.edge_case
@pytest.mark.error
def test_a_row_spanning_the_window_is_refused_in_sql_terms(decidb_cli_highs):
    """No single factor holds both ends of a row spanning more than the window.

    Scaling is all the loader is allowed to do — it may move a row, never reshape it —
    so this one is refused, naming the clause as written and the numbers that conflict
    rather than reporting a solver-internal status.
    """
    decidb_cli_highs.assert_error(
        """
        SELECT id, x, y FROM (VALUES (1,1e-12,1e16)) t(id,a,b)
        DECIDE x(REAL), y(REAL)
        SUCH THAT x >= 0 AND x <= 5 AND y >= 0 AND y <= 5 AND SUM(a*x + b*y) <= 1e18
        MAXIMIZE SUM(x + y)
    """,
        match=r"mixes numbers too far apart in size.*1e-12.*1e\+16",
        expect_large_numbers=True,
    )


@pytest.mark.edge_case
@pytest.mark.error
def test_a_bound_too_large_to_scale_is_refused_in_sql_terms(decidb_cli_highs):
    """The bound rides along with the row, and has a ceiling of its own.

    Scaling `1e-12` up into the window multiplies a limit of `1e19` past HiGHS's
    `infinite_bound`, at which point it stops being a limit at all. Refused for that
    reason specifically, rather than reported as the same spread problem.
    """
    decidb_cli_highs.assert_error(
        """
        SELECT id, x FROM (VALUES (1,1e-12)) t(id,w)
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 5 AND SUM(w*x) <= 1e19
        MAXIMIZE SUM(x)
    """,
        match=r"sets a limit of 1e\+19 .* coefficients as small as 1e-12",
        expect_large_numbers=True,
    )


# A column bound and the objective have windows of their own, and row scaling reaches neither. `passModel` refuses a
# column floor at or above `infinite_bound` and reads a linear objective coefficient there as an infinite cost;
# `passHessian` refuses a Hessian entry at or above `large_matrix_value` and drops one at or below `small_matrix_value`,
# so the objective HiGHS minimised was not the one written. A column is placed by LP column scaling (x = 2^e·x', its
# bounds divided and its entries multiplied), the objective by one factor over all its coefficients, each a power of
# two, and the answer is scaled back. Direct solve is off, or A1 would answer the quadratic objectives without HiGHS.
def _row_pair_oracle(oracle, lb, ub, linear, quadratic):
    """Two independent rows of `x`, each minimising `quadratic·x² + linear·x`."""
    oracle.create_model("row_pair")
    names = [f"x_{i}" for i in range(2)]
    for v in names:
        oracle.add_variable(v, VarType.CONTINUOUS, lb=lb, ub=ub)
    if quadratic:
        oracle.set_quadratic_objective(
            {v: linear for v in names}, {(v, v): quadratic for v in names}, ObjSense.MINIMIZE
        )
    else:
        oracle.set_objective({v: linear for v in names}, ObjSense.MINIMIZE)
    return oracle.solve(), [names]


def _cost_pair_oracle(oracle):
    """Two rows of `x + y >= 5` over `[0, 10]` boxes, `x` costing 1e25 and `y` 2e25."""
    oracle.create_model("cost_pair")
    for i in range(2):
        oracle.add_variable(f"x_{i}", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle.add_variable(f"y_{i}", VarType.CONTINUOUS, lb=0.0, ub=10.0)
        oracle.add_constraint({f"x_{i}": 1.0, f"y_{i}": 1.0}, ">=", 5.0, name=f"row_{i}")
    oracle.set_objective(
        {**{f"x_{i}": 1e25 for i in range(2)}, **{f"y_{i}": 2e25 for i in range(2)}}, ObjSense.MINIMIZE
    )
    return oracle.solve(), [[f"x_{i}" for i in range(2)], [f"y_{i}" for i in range(2)]]


@pytest.mark.edge_case
@pytest.mark.correctness
@pytest.mark.parametrize(
    "decide, build, large",
    [
        pytest.param(
            "x(REAL) SUCH THAT x >= 1e25 MINIMIZE SUM(x)",
            lambda o: _row_pair_oracle(o, 1e25, None, 1.0, 0.0),
            True,
            id="column-bound",
        ),
        pytest.param(
            "x(REAL) SUCH THAT x BETWEEN 0 AND 10 MINIMIZE SUM(1e16 * POWER(x - 1, 2))",
            lambda o: _row_pair_oracle(o, 0.0, 10.0, -2e16, 1e16),
            True,
            id="large-objective",
        ),
        pytest.param(
            "x(REAL) SUCH THAT x BETWEEN 0 AND 10 MINIMIZE SUM(1e-12 * POWER(x - 1, 2))",
            lambda o: _row_pair_oracle(o, 0.0, 10.0, -2e-12, 1e-12),
            False,
            id="small-objective",
        ),
        pytest.param(
            "x(REAL), y(REAL) SUCH THAT x BETWEEN 0 AND 10 AND y BETWEEN 0 AND 10 AND x + y >= 5 "
            "MINIMIZE SUM(1e25 * x + 2e25 * y)",
            _cost_pair_oracle,
            True,
            id="large-cost",
        ),
    ],
)
def test_a_column_or_objective_outside_the_window_is_solved(decidb_cli_highs, oracle_solver, decide, build, large):
    """HiGHS answers what the oracle answers once each is scaled into its window.

    - column-bound: the column scale. `x >= 1e25` is a floor `passModel` refused.
    - large-objective: the objective scaled down. A 2e16 Hessian entry was a `passHessian` refusal.
    - small-objective: the objective scaled up. A 2e-12 Hessian entry was dropped, and HiGHS answered x = 0.
    - large-cost: the linear coefficients cap the objective's scale. Costs read as infinite failed the solve.
    """
    cli = decidb_cli_highs.with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"})
    rows, _ = cli.execute(f"SELECT * FROM (VALUES (1),(2)) t(id) DECIDE {decide}", expect_large_numbers=large)
    result, columns = build(oracle_solver)

    assert result.status == SolverStatus.OPTIMAL
    for c, names in enumerate(columns):
        expected = [result.variable_values[v] for v in names]
        assert [r[1 + c] for r in rows] == pytest.approx(expected, rel=1e-6, abs=1e-6), rows


@pytest.mark.edge_case
@pytest.mark.correctness
@pytest.mark.parametrize(
    "clauses, lb, ub, sense",
    [
        pytest.param("x >= 9e29", 9e29, None, ("MAXIMIZE", ObjSense.MAXIMIZE), id="open-above"),
        pytest.param("x <= -9e29 AND x >= -1e30", -1e30, -9e29, ("MINIMIZE", ObjSense.MINIMIZE), id="open-below"),
    ],
)
def test_a_scaled_columns_open_side_stays_open(decidb_cli_highs, oracle_solver, clauses, lb, ub, sense):
    """A column scaled for one bound keeps "no bound" on its other side.

    The floor of 9e29 needs a scale of 2^34, which would bring the 1e30 "no bound" sentinel on the other side down to
    5.8e19, inside the window, as a real ceiling: HiGHS would answer x = 1e30 where the oracle finds no optimum.
    """
    oracle_solver.create_model("open_side")
    oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=lb, ub=ub)
    oracle_solver.set_objective({"x": 1.0}, sense[1])
    assert oracle_solver.solve().status == SolverStatus.UNBOUNDED

    decidb_cli_highs.assert_error(
        f"SELECT id, x FROM (VALUES (1),(2)) t(id) DECIDE x(REAL) SUCH THAT {clauses} {sense[0]} SUM(x)",
        match=r"DECIDE optimization is unbounded",
        expect_large_numbers=True,
    )


@pytest.mark.edge_case
@pytest.mark.error
def test_an_objective_spanning_the_window_is_refused_in_sql_terms(decidb_cli_highs):
    """No single factor holds an objective whose own coefficients span more than the window.

    The objective is moved, never reshaped, as a row is: `POWER(w*x - 1, 2)` with `w` at 1e-6 and 1e7 puts Hessian
    entries 1e-12 and 1e14 side by side, and the query is refused naming the objective rather than solved with the small
    entry dropped.
    """
    cli = decidb_cli_highs.with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"})
    cli.assert_error(
        """
        SELECT id, x FROM (VALUES (1,1e-6),(2,1e7)) t(id,w)
        DECIDE x(REAL)
        SUCH THAT x BETWEEN 0 AND 10
        MINIMIZE SUM(POWER(w * x - 1, 2))
    """,
        match=r"The objective mixes numbers too far apart in size for HiGHS to load",
        expect_large_numbers=True,
    )


@pytest.mark.edge_case
@pytest.mark.query_diagnostics
def test_a_repair_model_outside_the_window_names_the_repair(decidb_cli_highs):
    """A repair model whose objective is outside HiGHS's window is scaled into it as the query's own is.

    `x BETWEEN 0 AND 5 AND x >= 8` is infeasible on its bounds alone, so the query's own model never reaches HiGHS; the
    repair model carries the objective, whose 2e16 Hessian entry HiGHS refused, and the diagnosis then said "the solver
    could not analyse the repair model". `x >= 5` is the repair.
    """
    rows, cols = decidb_cli_highs.execute(
        """
        DIAGNOSE SELECT id, x FROM (VALUES (1),(2)) t(id)
        DECIDE x(REAL)
        SUCH THAT x BETWEEN 0 AND 5 AND x >= 8
        MINIMIZE SUM(1e16 * POWER(x - 1, 2))
    """,
        expect_large_numbers=True,
    )
    clause = cols.index("clause")
    change = cols.index("suggested_change")
    assert any(r[clause] == "x >= 8" and r[change] == "x >= 5" for r in rows), rows


@pytest.mark.edge_case
@pytest.mark.query_diagnostics
def test_a_repair_model_highs_refuses_is_reported_as_such(decidb_cli_highs):
    """A repair model HiGHS refuses is no answer about the repair: the diagnosis says the solver could not analyse it.

    The repair is found with the objective cleared; the objective spanning the window is refused only when the repair
    model is solved again with it, and a candidate without its objective cannot be compared with another. Read as an
    answer, that refusal would report "no loosening of the clauses you wrote restores feasibility", which `x >= 5`
    refutes.
    """
    cli = decidb_cli_highs.with_env({"DECIDB_TEST_DIRECT_SOLVE": "off"})
    rows, cols = cli.execute(
        """
        DIAGNOSE SELECT id, x FROM (VALUES (1,1e-6),(2,1e7)) t(id,w)
        DECIDE x(REAL)
        SUCH THAT x BETWEEN 0 AND 5 AND x >= 8
        MINIMIZE SUM(POWER(w * x - 1, 2))
    """,
        expect_large_numbers=True,
    )
    change = cols.index("suggested_change")
    assert any("could not analyse the repair model" in str(r[change]) for r in rows), rows
    assert not any("no loosening" in str(r[change]) for r in rows), rows
