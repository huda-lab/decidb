"""Tests for query-wide (`scalar`) decision variables.

A `scalar` declaration yields exactly one solver column for the whole query,
independent of input cardinality — the third variable scope alongside row-scoped
(`x(INT)`) and table-scoped (`T.x(INT)`). See paper §3.1.

Covers:
  - Scalar as a shared bound, oracle verified (the paper's `max_shortfall` shape)
  - Objective coefficient is applied once, not once per row
  - One column regardless of input cardinality
  - The value is repeated on every output row
  - Reducers over a scalar are rejected (objective and constraint)
  - Grammar: untyped and table-qualified scalars are rejected with a fix
  - `scalar` remains usable as an ordinary identifier
  - Empty input behaves like any other empty DECIDE query
"""

import pytest
from solver.types import VarType, ObjSense


# ---------------------------------------------------------------------------
# Test 1: Scalar as a shared upper bound, oracle verified
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_scalar_shared_bound(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """One query-wide cap constrains every row's ship, and is penalised in the
    objective. This is the paper's §3.1 shape: `demand - sum(ship) <= cap`
    reduced to a per-row bound plus a scalar penalty.

    Because the cap is shared, the optimum trades a single cap value against the
    per-row gain it unlocks — a row-scoped cap would decouple the rows entirely.
    """
    sql = """
        SELECT c.c_custkey, ship, cap
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 200
        DECIDE ship(INT), scalar cap(INT)
        SUCH THAT ship <= cap AND ship <= 10 AND cap <= 8
        MAXIMIZE SUM(ship) - 20 * cap
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    data = duckdb_conn.execute("""
        SELECT CAST(c.c_custkey AS BIGINT)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 200
        ORDER BY c.c_custkey
    """).fetchall()
    num_rows = len(data)
    assert num_rows > 0, "fixture produced no rows"

    oracle_solver.create_model("scalar_shared_bound")
    oracle_solver.add_variable("cap", VarType.INTEGER, lb=0.0, ub=8.0)
    ship_names = []
    for i in range(num_rows):
        name = f"ship_{i}"
        ship_names.append(name)
        oracle_solver.add_variable(name, VarType.INTEGER, lb=0.0, ub=10.0)
        # ship_i - cap <= 0
        oracle_solver.add_constraint({name: 1.0, "cap": -1.0}, "<=", 0.0,
                                     name=f"ship_le_cap_{i}")

    obj = {name: 1.0 for name in ship_names}
    obj["cap"] = -20.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    oracle_obj = oracle_solver.solve().objective_value

    cap_values = {int(row[2]) for row in decidb_result}
    assert len(cap_values) == 1, f"cap must be one value for the query, got {cap_values}"
    cap_value = cap_values.pop()

    decidb_obj = sum(int(row[1]) for row in decidb_result) - 20.0 * cap_value
    assert abs(decidb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={oracle_obj}"


# ---------------------------------------------------------------------------
# Test 2: The scalar's objective coefficient is applied once, not per row
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_scalar_objective_coefficient_applied_once(decidb_cli, duckdb_conn,
                                                   oracle_solver, perf_tracker):
    """`MINIMIZE 2*cap - SUM(x)` discriminates the coefficient.

    With the coefficient applied once (correct) the objective is 2*cap - n*cap =
    -(n-2)*cap, minimised by driving cap UP to its bound. If the coefficient were
    fanned out over rows it would be 2n*cap - n*cap = +n*cap, minimised by driving
    cap DOWN to 0. The two answers are opposite ends of cap's domain.
    """
    sql = """
        SELECT c.c_custkey, x, cap
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 100
        DECIDE x(INT), scalar cap(INT)
        SUCH THAT x <= cap AND x <= 10 AND cap <= 10
        MINIMIZE 2 * cap - SUM(x)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    num_rows = len(decidb_result)
    assert num_rows > 2, "need more than 2 rows for the coefficient to discriminate"

    oracle_solver.create_model("scalar_objective_coefficient")
    oracle_solver.add_variable("cap", VarType.INTEGER, lb=0.0, ub=10.0)
    x_names = []
    for i in range(num_rows):
        name = f"x_{i}"
        x_names.append(name)
        oracle_solver.add_variable(name, VarType.INTEGER, lb=0.0, ub=10.0)
        oracle_solver.add_constraint({name: 1.0, "cap": -1.0}, "<=", 0.0,
                                     name=f"x_le_cap_{i}")

    obj = {name: -1.0 for name in x_names}
    obj["cap"] = 2.0
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    oracle_obj = oracle_solver.solve().objective_value

    cap_value = int(decidb_result[0][2])
    decidb_obj = 2.0 * cap_value - sum(int(row[1]) for row in decidb_result)
    assert abs(decidb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={oracle_obj}"


# ---------------------------------------------------------------------------
# Test 3: One column regardless of input cardinality
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_scalar_is_one_column_regardless_of_cardinality(decidb_cli, perf_tracker):
    """The same scalar problem over two very different row counts must give the
    same answer. A row-scoped variable would decouple per row and let each row
    pick its own minimum, so the shared-maximum answer would not survive."""

    def solve(limit):
        sql = f"""
            SELECT cap
            FROM lineitem
            WHERE l_linenumber <= 7 AND l_orderkey <= {limit}
            DECIDE scalar cap(INT)
            SUCH THAT cap >= l_linenumber
            MINIMIZE cap
        """
        rows, _ = decidb_cli.execute(sql)
        caps = {int(r[0]) for r in rows}
        assert len(caps) == 1, f"cap must be a single value, got {caps}"
        return caps.pop(), len(rows)

    small_cap, small_rows = solve(50)
    large_cap, large_rows = solve(5000)

    assert large_rows > small_rows * 5, \
        f"sizes not distinct enough to be meaningful: {small_rows} vs {large_rows}"
    # cap >= max(l_linenumber) over the selection; both selections reach 7.
    assert small_cap == large_cap == 7, \
        f"cap changed with cardinality: {small_cap} vs {large_cap}"


# ---------------------------------------------------------------------------
# Test 4: The value is repeated on every output row (paper §3.1)
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_scalar_value_repeated_on_every_row(decidb_cli, perf_tracker):
    """"A scalar assignment is repeated on every output row" (paper §3.1)."""
    sql = """
        SELECT l_orderkey, l_linenumber, cap
        FROM lineitem
        WHERE l_orderkey <= 100
        DECIDE scalar cap(INT)
        SUCH THAT cap >= l_linenumber AND cap <= 20
        MINIMIZE cap
    """
    rows, cols = decidb_cli.execute(sql)
    assert len(rows) > 1, "need multiple rows to show repetition"
    values = {int(r[2]) for r in rows}
    assert len(values) == 1, f"scalar must repeat one value on every row, got {values}"


# ---------------------------------------------------------------------------
# Test 5: Reducers over a scalar are rejected
# ---------------------------------------------------------------------------

@pytest.mark.error
class TestScalarReducerRejected:
    """A query-wide decision has nothing to aggregate over, so a reducer around
    it is rejected rather than silently given a row-count coefficient."""

    def test_sum_over_scalar_in_objective(self, decidb_cli):
        decidb_cli.assert_error("""
                SELECT cap FROM lineitem
                DECIDE scalar cap(INT)
                SUCH THAT cap >= l_linenumber
                MINIMIZE SUM(cap)
            """, match=r"query-wide decision.*use cap on its own")

    def test_sum_over_scalar_in_constraint(self, decidb_cli):
        decidb_cli.assert_error("""
                SELECT cap FROM lineitem
                DECIDE scalar cap(INT)
                SUCH THAT SUM(cap) >= 5
                MINIMIZE cap
            """, match=r"query-wide decision.*use cap on its own")

    def test_avg_over_scalar_in_objective(self, decidb_cli):
        decidb_cli.assert_error("""
                SELECT cap FROM lineitem
                DECIDE scalar cap(INT)
                SUCH THAT cap >= l_linenumber
                MINIMIZE AVG(cap)
            """, match=r"query-wide decision")

    def test_sum_over_expression_containing_scalar(self, decidb_cli):
        """The scalar need not be the whole reducer argument to be rejected."""
        decidb_cli.assert_error("""
                SELECT cap FROM lineitem
                DECIDE x(INT), scalar cap(INT)
                SUCH THAT x <= 5
                MINIMIZE SUM(x + cap)
            """, match=r"query-wide decision")


# ---------------------------------------------------------------------------
# Test 6: Grammar rejections carry the fix
# ---------------------------------------------------------------------------

@pytest.mark.error_parser
@pytest.mark.error
class TestScalarGrammar:

    def test_scalar_without_type(self, decidb_cli):
        decidb_cli.assert_error("""
                SELECT cap FROM lineitem
                DECIDE scalar cap
                SUCH THAT cap >= 1 MINIMIZE cap
            """, match=r"needs a type; write scalar cap\(INT\)")

    def test_scalar_cannot_be_table_qualified(self, decidb_cli):
        decidb_cli.assert_error("""
                SELECT cap FROM lineitem
                DECIDE scalar lineitem.cap(INT)
                SUCH THAT cap >= 1 MINIMIZE cap
            """, match=r"cannot name a table")


# ---------------------------------------------------------------------------
# Test 7: `scalar` is still an ordinary identifier
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_scalar_still_usable_as_identifier(decidb_cli, perf_tracker):
    """SCALAR is an unreserved keyword, so existing queries that use it as a
    column alias or CTE name keep working."""
    rows, _ = decidb_cli.execute("""
        WITH scalar AS (SELECT l_orderkey AS scalar FROM lineitem WHERE l_orderkey <= 3)
        SELECT scalar FROM scalar ORDER BY scalar LIMIT 3
    """)
    assert len(rows) > 0, "SCALAR should still lex as an identifier"


@pytest.mark.correctness
def test_scalar_identifier_and_keyword_in_one_query(decidb_cli, perf_tracker):
    """The keyword and an identically-named column coexist: `scalar cap(INT)`
    declares a decision while `scalar` also names an output column."""
    rows, cols = decidb_cli.execute("""
        SELECT l_linenumber AS scalar, cap
        FROM lineitem
        WHERE l_orderkey <= 50
        DECIDE scalar cap(INT)
        SUCH THAT cap >= l_linenumber
        MINIMIZE cap
    """)
    assert len(rows) > 0
    caps = {int(r[1]) for r in rows}
    assert len(caps) == 1, f"cap must be a single query-wide value, got {caps}"


# ---------------------------------------------------------------------------
# Test 8: Empty input
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_scalar_empty_input(decidb_cli, perf_tracker):
    """An empty selection returns an empty relation, exactly as it would for a
    row-scoped decision — the scalar gets no special case."""
    sql = """
        SELECT l_orderkey, cap
        FROM lineitem
        WHERE l_orderkey < 0
        DECIDE scalar cap(INT)
        SUCH THAT cap >= l_linenumber
        MINIMIZE cap
    """
    rows, cols = decidb_cli.execute(sql)
    assert rows == [], f"expected no rows, got {rows}"
