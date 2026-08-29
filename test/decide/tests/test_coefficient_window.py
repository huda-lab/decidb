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
        _weighted_sum_sql("1e-9", "1e10", ">=", 1, "MINIMIZE"))
    result = _weighted_sum_oracle(
        oracle_solver, "tiny_floor", 1e-9, 1e10, ">=", 1.0, ObjSense.MINIMIZE)

    assert result.status == SolverStatus.OPTIMAL
    assert sum(r[1] for r in rows) == pytest.approx(result.objective_value, rel=1e-6)


@pytest.mark.edge_case
@pytest.mark.correctness
def test_a_large_data_coefficient_loads(decidb_cli_highs, oracle_solver):
    """`w = 1e16` is above HiGHS's ceiling, and is still just a number in a column."""
    rows, _ = decidb_cli_highs.execute(
        _weighted_sum_sql("1e16", 5, "<=", "1e20", "MAXIMIZE"))
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
    below, _ = decidb_cli_highs.execute(_BIG_M_SQL.format(box="900000000000000"))
    above, _ = decidb_cli_highs.execute(_BIG_M_SQL.format(box="4000000000000000"))

    assert [r[1] for r in below] == pytest.approx([9e14, 9e14], rel=1e-9)
    assert [r[1] for r in above] == pytest.approx([4e15, 4e15], rel=1e-9)


@pytest.mark.edge_case
@pytest.mark.correctness
@pytest.mark.parametrize("sql", [
    _weighted_sum_sql("0.000000001", 5, "<=", 1, "MAXIMIZE"),
    _weighted_sum_sql("1e-9", "1e10", ">=", 1, "MINIMIZE"),
    _weighted_sum_sql("1e16", 5, "<=", "1e20", "MAXIMIZE"),
    _BIG_M_SQL.format(box="4000000000000000"),
], ids=["tiny-cap", "tiny-floor", "large-cap", "big-m"])
def test_the_backends_agree_across_the_window(decidb_cli_highs, decidb_cli_gurobi, sql):
    """Which solver is installed must not decide whether a query has an answer.

    Row scaling multiplies by a power of two, which is exact in binary floating point,
    so the two backends solve bit-identical constraints and the totals match to the
    solvers' own tolerances rather than approximately.
    """
    highs_rows, _ = decidb_cli_highs.execute(sql)
    gurobi_rows, _ = decidb_cli_gurobi.execute(sql)

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
    rows, cols = decidb_cli_highs.execute("""
        DIAGNOSE SELECT id, x AS contrib
        FROM (VALUES (1,1e9),(2,1e9),(3,1e9)) t(id,w)
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 5000 AND SUM(w * x) <= 1e15 AND SUM(x) >= 30000
        MAXIMIZE SUM(x)
    """)

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
    decidb_cli_highs.assert_error(sql, match=r"(?i)infeasible")
    decidb_cli_gurobi.assert_error(sql, match=r"(?i)infeasible")


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
    highs_rows, _ = decidb_cli_highs.execute(sql)
    gurobi_rows, _ = decidb_cli_gurobi.execute(sql)

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
    decidb_cli_highs.assert_error(f"""
        SELECT id, x FROM (VALUES (1),(2)) t(id)
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 5 AND {clause}
        MAXIMIZE SUM(x)
    """, match=r"sets a limit of .* that HiGHS cannot hold")


@pytest.mark.edge_case
@pytest.mark.error
def test_a_row_spanning_the_window_is_refused_in_sql_terms(decidb_cli_highs):
    """No single factor holds both ends of a row spanning more than the window.

    Scaling is all the loader is allowed to do — it may move a row, never reshape it —
    so this one is refused, naming the clause as written and the numbers that conflict
    rather than reporting a solver-internal status.
    """
    decidb_cli_highs.assert_error("""
        SELECT id, x, y FROM (VALUES (1,1e-12,1e16)) t(id,a,b)
        DECIDE x(REAL), y(REAL)
        SUCH THAT x >= 0 AND x <= 5 AND y >= 0 AND y <= 5 AND SUM(a*x + b*y) <= 1e18
        MAXIMIZE SUM(x + y)
    """, match=r"mixes numbers too far apart in size.*1e-12.*1e\+16")


@pytest.mark.edge_case
@pytest.mark.error
def test_a_bound_too_large_to_scale_is_refused_in_sql_terms(decidb_cli_highs):
    """The bound rides along with the row, and has a ceiling of its own.

    Scaling `1e-12` up into the window multiplies a limit of `1e19` past HiGHS's
    `infinite_bound`, at which point it stops being a limit at all. Refused for that
    reason specifically, rather than reported as the same spread problem.
    """
    decidb_cli_highs.assert_error("""
        SELECT id, x FROM (VALUES (1,1e-12)) t(id,w)
        DECIDE x(REAL)
        SUCH THAT x >= 0 AND x <= 5 AND SUM(w*x) <= 1e19
        MAXIMIZE SUM(x)
    """, match=r"sets a limit of 1e\+19 .* coefficients as small as 1e-12")
