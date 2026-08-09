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


# ---------------------------------------------------------------------------
# Test 9: Composition with the rewrite-heavy features
#
# A scalar cannot appear inside a reducer (Test 5), and objectives must be
# aggregates — so ABS / POWER / bilinear reach a scalar only through *per-row
# constraints*. That is the surface tested here. Each case is built so the
# scalar's single column is what the optimum turns on, which is what would
# expose an off-by-one in `VarIndexer`'s four-block
# `[row | entity | scalar | global auxiliary]` layout: the rewrites append
# auxiliary variables to the global block, immediately above the scalar block.
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_scalar_with_abs_per_row_constraint(decidb_cli, duckdb_conn,
                                            oracle_solver, perf_tracker):
    """`ABS(x - cap) <= 2` couples every row's `x` to the one shared `cap`.

    ABS linearization adds two auxiliary columns per row. The scalar sits
    directly below the global auxiliary block, so a misplaced block boundary
    would either read the scalar as an auxiliary or shift the auxiliaries onto
    the scalar's column.
    """
    sql = """
        SELECT c_custkey, x, cap
        FROM customer
        WHERE c_custkey <= 20
        DECIDE x(INT), scalar cap(INT)
        SUCH THAT ABS(x - cap) <= 2 AND x <= 10 AND cap <= 10 AND cap >= 3
        MAXIMIZE SUM(x) - 5 * cap
    """
    rows, cols = decidb_cli.execute(sql)
    n = len(rows)
    assert n > 1, "fixture produced too few rows"

    oracle_solver.create_model("scalar_abs")
    oracle_solver.add_variable("cap", VarType.INTEGER, lb=3.0, ub=10.0)
    obj = {"cap": -5.0}
    for i in range(n):
        xi, di = f"x_{i}", f"d_{i}"
        oracle_solver.add_variable(xi, VarType.INTEGER, lb=0.0, ub=10.0)
        # d = |x - cap|, bounded by 2
        oracle_solver.add_variable(di, VarType.CONTINUOUS, lb=0.0, ub=2.0)
        oracle_solver.add_constraint({xi: 1.0, "cap": -1.0, di: -1.0}, "<=", 0.0,
                                     name=f"abs_pos_{i}")
        oracle_solver.add_constraint({xi: -1.0, "cap": 1.0, di: -1.0}, "<=", 0.0,
                                     name=f"abs_neg_{i}")
        obj[xi] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    oracle_obj = oracle_solver.solve().objective_value

    cap_idx, x_idx = cols.index("cap"), cols.index("x")
    caps = {int(r[cap_idx]) for r in rows}
    assert len(caps) == 1, f"cap must be one value, got {caps}"
    cap_value = caps.pop()
    for r in rows:
        assert abs(int(r[x_idx]) - cap_value) <= 2, "ABS constraint violated"

    decidb_obj = sum(int(r[x_idx]) for r in rows) - 5.0 * cap_value
    assert abs(decidb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={oracle_obj}"


@pytest.mark.correctness
def test_scalar_with_quadratic_constraint(decidb_cli_gurobi, oracle_solver,
                                          perf_tracker):
    """`POWER(x - cap, 2) <= 4` is the QCQP form of the ABS case above.

    Gurobi only — HiGHS has no quadratic constraints. The two formulations
    describe the same feasible set over integers, so the optimum must match
    `test_scalar_with_abs_per_row_constraint`'s.
    """
    sql = """
        SELECT c_custkey, x, cap
        FROM customer
        WHERE c_custkey <= 20
        DECIDE x(INT), scalar cap(INT)
        SUCH THAT POWER(x - cap, 2) <= 4 AND x <= 10 AND cap <= 10 AND cap >= 3
        MAXIMIZE SUM(x) - 5 * cap
    """
    rows, cols = decidb_cli_gurobi.execute(sql)
    n = len(rows)
    assert n > 1

    oracle_solver.create_model("scalar_qcqp")
    oracle_solver.add_variable("cap", VarType.INTEGER, lb=3.0, ub=10.0)
    obj = {"cap": -5.0}
    for i in range(n):
        xi, di = f"x_{i}", f"d_{i}"
        oracle_solver.add_variable(xi, VarType.INTEGER, lb=0.0, ub=10.0)
        oracle_solver.add_variable(di, VarType.CONTINUOUS, lb=0.0, ub=2.0)
        oracle_solver.add_constraint({xi: 1.0, "cap": -1.0, di: -1.0}, "<=", 0.0,
                                     name=f"q_pos_{i}")
        oracle_solver.add_constraint({xi: -1.0, "cap": 1.0, di: -1.0}, "<=", 0.0,
                                     name=f"q_neg_{i}")
        obj[xi] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    oracle_obj = oracle_solver.solve().objective_value

    cap_idx, x_idx = cols.index("cap"), cols.index("x")
    cap_value = int(rows[0][cap_idx])
    for r in rows:
        assert (int(r[x_idx]) - cap_value) ** 2 <= 4, "quadratic constraint violated"
    decidb_obj = sum(int(r[x_idx]) for r in rows) - 5.0 * cap_value
    assert abs(decidb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={oracle_obj}"


@pytest.mark.bilinear
@pytest.mark.correctness
def test_scalar_with_bilinear_per_row_constraint(decidb_cli, oracle_solver,
                                                 perf_tracker):
    """`b * cap <= 4` is a Boolean x scalar product, McCormick-linearized.

    The instance is built so the scalar's value trades against how many rows can
    switch on: `cap = 6` blocks every `b`, `cap = 4` admits three. Raising `cap`
    past 4 costs more objective than it gains, so the optimum pins `cap` at 4
    and is not at either end of its domain.
    """
    sql = """
        SELECT c_custkey, b, cap
        FROM customer
        WHERE c_custkey <= 20
        DECIDE b(BOOL), scalar cap(INT)
        SUCH THAT b * cap <= 4 AND cap <= 6 AND SUM(b) <= 3
        MAXIMIZE SUM(b) + cap
    """
    rows, cols = decidb_cli.execute(sql)
    n = len(rows)
    assert n >= 3

    oracle_solver.create_model("scalar_bilinear")
    oracle_solver.add_variable("cap", VarType.INTEGER, lb=0.0, ub=6.0)
    obj = {"cap": 1.0}
    budget = {}
    for i in range(n):
        bi, pi = f"b_{i}", f"p_{i}"
        oracle_solver.add_variable(bi, VarType.BINARY)
        # p = b * cap via McCormick with cap in [0, 6]
        oracle_solver.add_variable(pi, VarType.CONTINUOUS, lb=0.0, ub=6.0)
        oracle_solver.add_constraint({pi: 1.0, bi: -6.0}, "<=", 0.0, name=f"mc1_{i}")
        oracle_solver.add_constraint({pi: 1.0, "cap": -1.0}, "<=", 0.0, name=f"mc2_{i}")
        oracle_solver.add_constraint({pi: -1.0, "cap": 1.0, bi: 6.0}, "<=", 6.0,
                                     name=f"mc3_{i}")
        oracle_solver.add_constraint({pi: 1.0}, "<=", 4.0, name=f"prod_cap_{i}")
        obj[bi] = 1.0
        budget[bi] = 1.0
    oracle_solver.add_constraint(budget, "<=", 3.0, name="b_budget")
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    oracle_obj = oracle_solver.solve().objective_value

    cap_idx, b_idx = cols.index("cap"), cols.index("b")
    cap_value = int(rows[0][cap_idx])
    for r in rows:
        assert int(r[b_idx]) * cap_value <= 4, "bilinear constraint violated"
    decidb_obj = sum(int(r[b_idx]) for r in rows) + cap_value
    assert abs(decidb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={oracle_obj}"


@pytest.mark.correctness
def test_scalar_with_not_equal(decidb_cli, perf_tracker):
    """`cap <> 8` on a query-wide decision goes through the Big-M disjunction,
    which adds a global auxiliary binary — the block immediately above the
    scalar's.

    The objective pushes `cap` to its ceiling, and `<> 8` is the only thing
    stopping it, so the answer is `cap = 7` exactly when the indicator is wired
    to the scalar's column and not to a neighbour's.
    """
    rows, cols = decidb_cli.execute("""
        SELECT c_custkey, x, cap
        FROM customer
        WHERE c_custkey <= 20
        DECIDE x(INT), scalar cap(INT)
        SUCH THAT x <= cap AND cap <= 8 AND cap <> 8
        MAXIMIZE SUM(x)
    """)
    assert len(rows) > 1
    cap_idx, x_idx = cols.index("cap"), cols.index("x")
    caps = {int(r[cap_idx]) for r in rows}
    assert caps == {7}, f"expected cap pinned to 7 by the <> disjunction, got {caps}"
    assert all(int(r[x_idx]) == 7 for r in rows)


# ---------------------------------------------------------------------------
# Test 10: A scalar on the RHS of a reduced constraint does not bind yet
# ---------------------------------------------------------------------------

@pytest.mark.error_binder
@pytest.mark.error
def test_scalar_as_aggregate_rhs_rejected(decidb_cli):
    """`SUM(x) <= cap` — the paper's §3.1 shape — is rejected today.

    This is not a scalar-scope limitation: the same binder check refuses any
    non-reduced RHS on a reduced constraint, with or without `PER`, and it is
    filed as group B of `context/descriptions/todo.md` (B1/B2/B4). It is pinned
    here because it is the one shape the paper writes with `scalar`
    (`demand - sum(ship) <= max_shortfall per regionID`), so this test is what
    will flip when group B lands.
    """
    decidb_cli.assert_error("""
            SELECT c_custkey, x, cap
            FROM customer WHERE c_custkey <= 20
            DECIDE x(INT), scalar cap(INT)
            SUCH THAT SUM(x) <= cap AND cap <= 12
            MAXIMIZE SUM(x) - 20 * cap
        """, match=r"SUM cannot be compared to an expression that is not a scalar or aggregate")


@pytest.mark.error_binder
@pytest.mark.error
def test_scalar_as_aggregate_rhs_with_per_rejected(decidb_cli):
    """Same shape with `PER`, which is exactly the paper's §3.1 constraint.

    Blocked on the same group-B work; `PER` adds nothing to the rejection.
    """
    decidb_cli.assert_error("""
            SELECT c_custkey, c_nationkey, x, cap
            FROM customer WHERE c_custkey <= 40
            DECIDE x(INT), scalar cap(INT)
            SUCH THAT SUM(x) <= cap PER c_nationkey AND cap <= 12
            MAXIMIZE SUM(x) - 20 * cap
        """, match=r"SUM cannot be compared to an expression that is not a scalar or aggregate")


# ---------------------------------------------------------------------------
# Test 11: A scalar in the diagnostics relation
# ---------------------------------------------------------------------------

@pytest.mark.query_diagnostics
def test_unbounded_scalar_reports_a_single_instance(decidb_cli):
    """An unbounded scalar has one instance, so the report carries only
    `grows_toward` — no `affected_rows` / `affected_entities` cell, and no
    categorical grouping rule, because there is no subset of rows to describe.
    """
    script = (
        ".mode csv\n"
        "PRAGMA diagnose_decide='auto';\n"
        "SELECT c_custkey, x, cap FROM customer WHERE c_custkey <= 20\n"
        "DECIDE x(INT), scalar cap(INT)\n"
        "SUCH THAT x <= 5\n"
        "MAXIMIZE SUM(x) + cap;\n"
        "SELECT * FROM decide_diagnostics();\n"
    )
    result = decidb_cli.execute_script(script)
    combined = result.stdout + result.stderr

    assert "unbounded" in combined.lower()
    assert "cap" in combined
    assert "grows_toward" in combined
    assert "affected_rows" not in combined, \
        "a query-wide decision has no row subset to report"
    assert "affected_entities" not in combined, \
        "a query-wide decision has no entity subset to report"
