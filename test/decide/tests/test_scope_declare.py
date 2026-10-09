"""Keyed declarations: `DECIDE per K: x(TYPE)`.

A key lists columns and/or relations of the FROM clause (a relation stands for all
its stored columns). Rows with equal key values share one decision, NULL included;
`per ()` is one decision for the whole query and no `per` is one per row. Each
declarator has its own key. Behaviour: anr_language_extension.md §2.

Model size is read from DECIDB_DUMP_MODEL (`num_vars`: one solver column per decision).
Optima are judged against `_scope_oracle`, which builds the same model from Python data,
and DeciDB's answer is checked to show one value per class and to satisfy every clause.

Covers:
  - a column key, a relation key (= all its stored columns), a key across two relations
  - `per ()` is one decision (the same model as `scalar`); no `per` is one per row
  - a key column absent from SELECT, WHERE and the clause survives column pruning, on a
    base table and on VALUES, and through a serialization round trip
  - a key is a set: order and repetition do not matter
  - NULL is a key value
  - both clause orders
  - several declarators, each with its own key
  - the old `T.x` keeps dropping the columns its clause reads as data; a `per` key does not
  - result types, EXPLAIN, and every error a key raises
"""

import re
import textwrap

import pytest

from ._output_helpers import assert_no_internal_leak
from ._scope_oracle import GLOBAL, Decision, Objective, RowClause, ScopeModel, SumClause
from solver.types import ObjSense, VarType

# Shipments joined to their depots: five result rows, three depots in two regions.
SHIPMENT_COLUMNS = ("shipmentID", "depotID", "customerID", "demand")
DEPOT_COLUMNS = ("depotID", "region", "country", "capacity")
SHIPMENTS = (
    (1, "D1", 1, 10),
    (2, "D1", 2, 20),
    (3, "D2", 3, 30),
    (4, "D2", 1, 15),
    (5, "D3", 2, 25),
)
DEPOTS = (
    ("D1", "East", "US", 40),
    ("D2", "East", "CA", 30),
    ("D3", "West", "US", 20),
)


def _literal(value) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, str):
        return "'" + value.replace("'", "''") + "'"
    return str(value)


def _values(rows, alias: str, columns) -> str:
    tuples = ", ".join("(" + ", ".join(_literal(v) for v in row) + ")" for row in rows)
    return f"(VALUES {tuples}) {alias}({', '.join(columns)})"


def _from(depots=DEPOTS, join="JOIN") -> str:
    shipments = _values(SHIPMENTS, "S", SHIPMENT_COLUMNS)
    return f"FROM {shipments} {join} {_values(depots, 'D', DEPOT_COLUMNS)} USING (depotID)"


def _joined(depots=DEPOTS) -> list[dict]:
    """The result rows of `_from(depots)`, in shipmentID order."""
    by_id = {depot[0]: dict(zip(DEPOT_COLUMNS, depot)) for depot in depots}
    rows = []
    for shipment in SHIPMENTS:
        row = dict(zip(SHIPMENT_COLUMNS, shipment))
        row.update(by_id[row["depotID"]])
        rows.append(row)
    return rows


_NUM_VARS = re.compile(r"^num_vars: (\d+)$", re.M)


def _num_vars(dump: str) -> int:
    match = _NUM_VARS.search(dump)
    assert match, f"no num_vars line in the model dump:\n{dump}"
    return int(match.group(1))


def _answer(cli, sql: str) -> list[dict]:
    rows, columns = cli.execute(sql)
    return [dict(zip(columns, row)) for row in rows]


def _aligned(answer: list[dict], model: ScopeModel, id_column: str) -> list[dict]:
    assert [row[id_column] for row in answer] == [row[id_column] for row in model.rows], (
        "DeciDB's rows are not in the oracle's order")
    return answer


@pytest.fixture
def scope_solver(_raw_oracle_solver):
    """Gurobi, solving afresh each time: the oracle models are tiny, and a cached optimum
    would hide a change to `_scope_oracle`."""
    if _raw_oracle_solver is None:
        pytest.skip("Gurobi not available — the scope oracle requires it")
    return _raw_oracle_solver


# ---------------------------------------------------------------------------
# One decision per class of the key
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_column_key_is_one_decision_per_value(decidb_cli, scope_solver, tmp_path):
    """`per D.depotID`: one reserve per depot, shared by the depot's shipments, and each
    of the depot's rows bounds it."""
    sql = f"""
        SELECT shipmentID, reserve {_from()}
        DECIDE per D.depotID: reserve(INT)
        SUCH THAT reserve <= D.capacity AND reserve <= S.demand
        MAXIMIZE SUM(reserve)
        ORDER BY shipmentID"""
    model = ScopeModel(_joined(), [Decision("reserve", ("depotID",))])
    clauses = [RowClause(lambda r: {"reserve": 1}, "<=", lambda r: r["capacity"]),
               RowClause(lambda r: {"reserve": 1}, "<=", lambda r: r["demand"])]
    objective = Objective(ObjSense.MAXIMIZE, rows=lambda r: {"reserve": 1})

    assert model.class_count("reserve") == 3
    assert _num_vars(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == 3
    answer = _aligned(_answer(decidb_cli, sql), model, "shipmentID")
    assert model.judge(answer, clauses, objective) == pytest.approx(
        model.solve(scope_solver, clauses, objective))


@pytest.mark.correctness
def test_relation_key_is_all_its_stored_columns(decidb_cli, scope_solver, tmp_path):
    """`per D` is `per D.depotID, D.region, D.country, D.capacity`: the same model."""
    def query(key):
        return f"""
            SELECT shipmentID, open {_from()}
            DECIDE per {key}: open(BOOL)
            SUCH THAT SUM(open) <= 3
            MAXIMIZE SUM(open * S.demand)
            ORDER BY shipmentID"""
    relation = decidb_cli.dump_model(query("D"), tmp_path / "relation.dump")
    columns = decidb_cli.dump_model(query("D.depotID, D.region, D.country, D.capacity"),
                                    tmp_path / "columns.dump")
    assert relation == columns

    model = ScopeModel(_joined(), [Decision("open", DEPOT_COLUMNS, VarType.BINARY, ub=1.0)])
    clauses = [SumClause(lambda r: {"open": 1}, "<=", 3)]
    objective = Objective(ObjSense.MAXIMIZE, rows=lambda r: {"open": r["demand"]})
    assert _num_vars(relation) == model.class_count("open") == 3
    answer = _aligned(_answer(decidb_cli, query("D")), model, "shipmentID")
    assert model.judge(answer, clauses, objective) == pytest.approx(
        model.solve(scope_solver, clauses, objective))


@pytest.mark.correctness
def test_relation_key_on_a_base_table(decidb_cli, duckdb_conn, tmp_path):
    """A base table registers its key columns through the scan; `per n` still equals
    listing every column of nation."""
    def query(key):
        return f"""
            SELECT c.c_custkey, keep
            FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
            WHERE c.c_custkey <= 30
            DECIDE per {key}: keep(BOOL)
            SUCH THAT SUM(keep) <= 10
            MAXIMIZE SUM(keep * c.c_acctbal)"""
    relation = decidb_cli.dump_model(query("n"), tmp_path / "relation.dump")
    columns = decidb_cli.dump_model(query("n.n_nationkey, n.n_name, n.n_regionkey, n.n_comment"),
                                    tmp_path / "columns.dump")
    assert relation == columns
    nations, = duckdb_conn.execute(
        "SELECT COUNT(DISTINCT c_nationkey) FROM customer WHERE c_custkey <= 30").fetchone()
    assert _num_vars(relation) == nations


@pytest.mark.correctness
def test_key_across_two_relations(decidb_cli, scope_solver, tmp_path):
    """`per D.region, S.customerID`: one stock per (region, customer) pair among the rows."""
    sql = f"""
        SELECT shipmentID, stock {_from()}
        DECIDE per D.region, S.customerID: stock(INT)
        SUCH THAT stock <= S.demand
        MAXIMIZE SUM(stock)
        ORDER BY shipmentID"""
    model = ScopeModel(_joined(), [Decision("stock", ("region", "customerID"))])
    clauses = [RowClause(lambda r: {"stock": 1}, "<=", lambda r: r["demand"])]
    objective = Objective(ObjSense.MAXIMIZE, rows=lambda r: {"stock": 1})

    assert model.class_count("stock") == 4
    assert _num_vars(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == 4
    answer = _aligned(_answer(decidb_cli, sql), model, "shipmentID")
    assert model.judge(answer, clauses, objective) == pytest.approx(
        model.solve(scope_solver, clauses, objective))


@pytest.mark.correctness
def test_per_empty_is_one_decision_and_no_per_is_one_per_row(decidb_cli, scope_solver, tmp_path):
    """`per (): cap` is one decision for the query, with its objective coefficient
    counted once -- the same model as `scalar cap` -- while `ship` stays one per row."""
    def query(cap):
        return f"""
            SELECT shipmentID, cap, ship {_from()}
            DECIDE {cap}(INT), ship(INT)
            SUCH THAT ship <= cap AND ship <= S.demand AND cap <= 18
            MAXIMIZE SUM(ship) - cap
            ORDER BY shipmentID"""
    keyed = decidb_cli.dump_model(query("per (): cap"), tmp_path / "per.dump")
    assert keyed == decidb_cli.dump_model(query("scalar cap"), tmp_path / "scalar.dump")

    model = ScopeModel(_joined(), [Decision("cap", GLOBAL), Decision("ship")])
    clauses = [RowClause(lambda r: {"ship": 1, "cap": -1}, "<=", lambda r: 0),
               RowClause(lambda r: {"ship": 1}, "<=", lambda r: r["demand"]),
               RowClause(lambda r: {"cap": 1}, "<=", lambda r: 18)]
    objective = Objective(ObjSense.MAXIMIZE, rows=lambda r: {"ship": 1}, once={"cap": -1})
    assert _num_vars(keyed) == model.column_count() == 1 + len(SHIPMENTS)
    answer = _aligned(_answer(decidb_cli, query("per (): cap")), model, "shipmentID")
    assert model.judge(answer, clauses, objective) == pytest.approx(
        model.solve(scope_solver, clauses, objective))


# ---------------------------------------------------------------------------
# A key column the query never reads survives column pruning
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_key_column_survives_pruning_on_a_base_table(decidb_cli, duckdb_conn, scope_solver, tmp_path):
    """n_regionkey appears only in the key -- not in SELECT, WHERE or the clause -- yet
    keeps one decision per region."""
    sql = """
        SELECT c.c_custkey, keep
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE c.c_custkey <= 30
        DECIDE per n.n_regionkey: keep(BOOL)
        SUCH THAT SUM(keep) <= 10
        MAXIMIZE SUM(keep * c.c_acctbal)
        ORDER BY c.c_custkey"""
    rows = [dict(zip(("c_custkey", "n_regionkey", "c_acctbal"), (key, region, float(balance))))
            for key, region, balance in duckdb_conn.execute("""
                SELECT c.c_custkey, n.n_regionkey, c.c_acctbal
                FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
                WHERE c.c_custkey <= 30 ORDER BY c.c_custkey""").fetchall()]
    model = ScopeModel(rows, [Decision("keep", ("n_regionkey",), VarType.BINARY, ub=1.0)])
    clauses = [SumClause(lambda r: {"keep": 1}, "<=", 10)]
    objective = Objective(ObjSense.MAXIMIZE, rows=lambda r: {"keep": r["c_acctbal"]})

    assert 1 < model.class_count("keep") < len(rows)
    assert _num_vars(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == model.class_count("keep")
    answer = _aligned(_answer(decidb_cli, sql), model, "c_custkey")
    assert model.judge(answer, clauses, objective) == pytest.approx(
        model.solve(scope_solver, clauses, objective))


@pytest.mark.correctness
def test_key_column_survives_pruning_on_values(decidb_cli, scope_solver, tmp_path):
    """The same on VALUES, whose columns are registered by their position: D.region is
    read only by the key."""
    sql = f"""
        SELECT shipmentID, x {_from()}
        DECIDE per D.region: x(INT)
        SUCH THAT x <= S.demand
        MAXIMIZE SUM(x)
        ORDER BY shipmentID"""
    model = ScopeModel(_joined(), [Decision("x", ("region",))])
    clauses = [RowClause(lambda r: {"x": 1}, "<=", lambda r: r["demand"])]
    objective = Objective(ObjSense.MAXIMIZE, rows=lambda r: {"x": 1})

    assert _num_vars(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == model.class_count("x") == 2
    answer = _aligned(_answer(decidb_cli, sql), model, "shipmentID")
    assert model.judge(answer, clauses, objective) == pytest.approx(
        model.solve(scope_solver, clauses, objective))


@pytest.mark.correctness
def test_keyed_plan_survives_a_serialization_round_trip(decidb_cli, tmp_path):
    """Key scopes and keyed variables survive the plan round trip: the copy builds the
    same model and gives the same answer."""
    sql = f"""
        SELECT shipmentID, ship, open, r, cap {_from()}
        DECIDE ship(INT), per D: open(BOOL), per D.region: r(INT), per (): cap(INT)
        SUCH THAT ship <= S.demand * open AND ship <= cap AND r <= D.capacity AND cap <= 22
        MAXIMIZE SUM(ship) + SUM(r) - 10 * SUM(open) - cap
        ORDER BY shipmentID"""
    copied = decidb_cli.with_verify_serializer()
    assert copied.dump_model(sql, tmp_path / "copied.dump") == decidb_cli.dump_model(sql, tmp_path / "plain.dump")
    assert copied.execute(sql) == decidb_cli.execute(sql)


# ---------------------------------------------------------------------------
# What a key is
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_key_is_a_set(decidb_cli, tmp_path):
    """Order and repetition do not change a key."""
    def query(key):
        return f"""
            SELECT shipmentID, stock {_from()}
            DECIDE per {key}: stock(INT)
            SUCH THAT stock <= S.demand
            MAXIMIZE SUM(stock)"""
    written = decidb_cli.dump_model(query("D.region, S.customerID"), tmp_path / "written.dump")
    reordered = decidb_cli.dump_model(query("S.customerID, D.region, D.region"), tmp_path / "reordered.dump")
    assert written == reordered


@pytest.mark.correctness
def test_null_key_value_is_one_class(decidb_cli, scope_solver, tmp_path):
    """Two depots with no region share one decision: NULL is a key value."""
    depots = (("D1", "East", "US", 40), ("D2", None, "CA", 30), ("D3", None, "US", 20))
    sql = f"""
        SELECT shipmentID, r {_from(depots)}
        DECIDE per D.region: r(INT)
        SUCH THAT r <= D.capacity
        MAXIMIZE SUM(r)
        ORDER BY shipmentID"""
    model = ScopeModel(_joined(depots), [Decision("r", ("region",))])
    clauses = [RowClause(lambda r: {"r": 1}, "<=", lambda r: r["capacity"])]
    objective = Objective(ObjSense.MAXIMIZE, rows=lambda r: {"r": 1})

    assert _num_vars(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == model.class_count("r") == 2
    answer = _aligned(_answer(decidb_cli, sql), model, "shipmentID")
    assert model.judge(answer, clauses, objective) == pytest.approx(
        model.solve(scope_solver, clauses, objective))


@pytest.mark.correctness
def test_both_clause_orders(decidb_cli, tmp_path):
    """The declaration may come after WHERE or before FROM."""
    after_where = f"""
        SELECT shipmentID, reserve {_from()} WHERE S.demand > 10
        DECIDE per D.depotID: reserve(INT)
        SUCH THAT reserve <= S.demand
        MAXIMIZE SUM(reserve)
        ORDER BY shipmentID"""
    before_from = f"""
        SELECT shipmentID, reserve
        DECIDE per D.depotID: reserve(INT)
        {_from()} WHERE S.demand > 10
        SUCH THAT reserve <= S.demand
        MAXIMIZE SUM(reserve)
        ORDER BY shipmentID"""
    assert (decidb_cli.dump_model(after_where, tmp_path / "after.dump") ==
            decidb_cli.dump_model(before_from, tmp_path / "before.dump"))
    assert decidb_cli.execute(after_where) == decidb_cli.execute(before_from)


# ---------------------------------------------------------------------------
# Several declarators, each with its own key
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_four_keys_in_one_clause(decidb_cli, scope_solver, tmp_path):
    """Per row, per depot, per region and per query in one DECIDE list."""
    sql = f"""
        SELECT shipmentID, ship, open, r, cap {_from()}
        DECIDE ship(INT), per D: open(BOOL), per D.region: r(INT), per (): cap(REAL)
        SUCH THAT ship <= S.demand * open AND ship <= cap AND r <= D.capacity AND cap <= 22
        MAXIMIZE SUM(ship) + SUM(r) - 10 * SUM(open) - cap
        ORDER BY shipmentID"""
    model = ScopeModel(_joined(), [
        Decision("ship"),
        Decision("open", DEPOT_COLUMNS, VarType.BINARY, ub=1.0),
        Decision("r", ("region",)),
        Decision("cap", GLOBAL, VarType.CONTINUOUS),
    ])
    clauses = [RowClause(lambda r: {"ship": 1, "open": -r["demand"]}, "<=", lambda r: 0),
               RowClause(lambda r: {"ship": 1, "cap": -1}, "<=", lambda r: 0),
               RowClause(lambda r: {"r": 1}, "<=", lambda r: r["capacity"]),
               RowClause(lambda r: {"cap": 1}, "<=", lambda r: 22)]
    objective = Objective(ObjSense.MAXIMIZE, rows=lambda r: {"ship": 1, "r": 1, "open": -10},
                          once={"cap": -1})

    assert model.column_count() == len(SHIPMENTS) + 3 + 2 + 1
    assert _num_vars(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == model.column_count()
    answer = _aligned(_answer(decidb_cli, sql), model, "shipmentID")
    assert model.judge(answer, clauses, objective) == pytest.approx(
        model.solve(scope_solver, clauses, objective))


@pytest.mark.correctness
def test_a_key_does_not_carry_over(decidb_cli, tmp_path):
    """`per D: open(BOOL), ship(INT)`: the key belongs to `open` alone; `ship` is per row."""
    sql = f"""
        SELECT shipmentID {_from()}
        DECIDE per D: open(BOOL), ship(INT)
        SUCH THAT ship <= S.demand * open
        MAXIMIZE SUM(ship) - 10 * SUM(open)"""
    assert _num_vars(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == len(DEPOTS) + len(SHIPMENTS)


@pytest.mark.correctness
def test_two_declarators_with_one_key(decidb_cli, scope_solver, tmp_path):
    """Writing the same key twice gives each name its own decision per depot."""
    sql = f"""
        SELECT shipmentID, a, b {_from()}
        DECIDE per D.depotID: a(INT), per D.depotID: b(INT)
        SUCH THAT a + b <= S.demand
        MAXIMIZE SUM(a) + 2 * SUM(b)
        ORDER BY shipmentID"""
    model = ScopeModel(_joined(), [Decision("a", ("depotID",)), Decision("b", ("depotID",))])
    clauses = [RowClause(lambda r: {"a": 1, "b": 1}, "<=", lambda r: r["demand"])]
    objective = Objective(ObjSense.MAXIMIZE, rows=lambda r: {"a": 1, "b": 2})

    assert _num_vars(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == 2 * len(DEPOTS)
    answer = _aligned(_answer(decidb_cli, sql), model, "shipmentID")
    assert model.judge(answer, clauses, objective) == pytest.approx(
        model.solve(scope_solver, clauses, objective))


# ---------------------------------------------------------------------------
# The old table-scoped spelling keeps its own key rule
# ---------------------------------------------------------------------------

TRIM = "(VALUES (1, 'a', 5), (1, 'a', 7), (2, 'b', 3)) T(k, name, cost)"


@pytest.mark.correctness
def test_old_table_scope_drops_data_columns_but_a_per_key_does_not(decidb_cli, tmp_path):
    """`T.x` keeps dropping from its key the columns its clause reads as data (cost
    here), until the old spelling is removed; `per T` means every column of T."""
    def count(declaration, name):
        sql = f"""
            SELECT T.k, x FROM {TRIM}
            DECIDE {declaration}
            SUCH THAT x <= 4
            MAXIMIZE SUM(cost * x)"""
        return _num_vars(decidb_cli.dump_model(sql, tmp_path / f"{name}.dump"))
    assert count("T.x(INT)", "table") == 2
    assert count("per T: x(INT)", "relation") == 3
    assert count("per T.k: x(INT)", "column") == 2
    # In one query: the per scope is neither shared with T.x's nor trimmed with it.
    assert count("T.x(INT), per T: y(INT)", "mixed") == 2 + 3


@pytest.mark.correctness
def test_per_relation_equals_old_table_scope_when_no_column_is_data(decidb_cli, tmp_path):
    def query(declaration):
        return f"""
            SELECT T.k, x FROM {TRIM}
            DECIDE {declaration}
            SUCH THAT x <= 4
            MAXIMIZE SUM(x)"""
    assert (decidb_cli.dump_model(query("T.x(INT)"), tmp_path / "table.dump") ==
            decidb_cli.dump_model(query("per T: x(INT)"), tmp_path / "relation.dump"))


@pytest.mark.correctness
def test_qualified_key_names(decidb_cli, tmp_path):
    """A key element is spelled as SQL spells a column or a relation, schema-qualified
    included."""
    def dump(key, name):
        return decidb_cli.dump_model(f"""
            SELECT n_nationkey, x FROM nation
            DECIDE per {key}: x(INT)
            SUCH THAT x <= 3
            MAXIMIZE SUM(x)""", tmp_path / f"{name}.dump")
    column = dump("nation.n_regionkey", "column")
    assert _num_vars(column) == 5
    assert dump("main.nation.n_regionkey", "schema_column") == column
    relation = dump("nation", "relation")
    assert _num_vars(relation) == 25
    assert dump("main.nation", "schema_relation") == relation


@pytest.mark.correctness
def test_unqualified_using_column_is_the_kept_side(decidb_cli, tmp_path):
    """After an outer join, an unqualified USING column in a key means the column of the
    side that keeps every row -- the right one after RIGHT JOIN, the left one after LEFT
    JOIN -- as it does anywhere else in the query."""
    def dump(key, depots, join, name):
        return decidb_cli.dump_model(f"""
            SELECT shipmentID, r {_from(depots, join)}
            DECIDE per {key}: r(INT)
            SUCH THAT r <= 7
            MAXIMIZE SUM(r)""", tmp_path / f"{name}.dump")
    # Two depots have no shipment: RIGHT JOIN keeps them with a NULL shipment side.
    more_depots = DEPOTS + (("D4", "West", "US", 10), ("D5", "East", "CA", 5))
    right = dump("depotID", more_depots, "RIGHT JOIN", "right")
    assert right == dump("D.depotID", more_depots, "RIGHT JOIN", "right_depot")
    assert _num_vars(right) == 5
    assert _num_vars(dump("S.depotID", more_depots, "RIGHT JOIN", "right_shipment")) == 4
    # Only D1 exists: LEFT JOIN keeps every shipment with a NULL depot side.
    left = dump("depotID", DEPOTS[:1], "LEFT JOIN", "left")
    assert left == dump("S.depotID", DEPOTS[:1], "LEFT JOIN", "left_shipment")
    assert _num_vars(left) == 3
    assert _num_vars(dump("D.depotID", DEPOTS[:1], "LEFT JOIN", "left_depot")) == 2


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_result_types(decidb_cli):
    """A keyed decision returns the type of its declaration: INT as BIGINT, BOOL as
    INTEGER, REAL as DOUBLE."""
    sql = f"""
        SELECT typeof(a), typeof(b), typeof(c) {_from()}
        DECIDE per D.depotID: a(INT), per D: b(BOOL), per (): c(REAL)
        SUCH THAT a <= 2 AND c <= 3
        MAXIMIZE SUM(a) + SUM(b) + c
        LIMIT 1"""
    rows, _ = decidb_cli.execute(sql)
    assert rows == [("BIGINT", "INTEGER", "DOUBLE")]


@pytest.mark.explain
def test_explain_a_keyed_query(decidb_cli):
    result = decidb_cli.execute_raw(f"""
        EXPLAIN SELECT shipmentID, reserve {_from()}
        DECIDE per D.depotID: reserve(INT)
        SUCH THAT reserve <= S.demand
        MAXIMIZE SUM(reserve)""")
    assert "Variables: reserve" in result.stdout, result.stderr
    assert_no_internal_leak(result, "EXPLAIN of a keyed declaration")


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

def _decide(declarations: str, source: str = None) -> str:
    return f"""
        SELECT shipmentID {source or _from()}
        DECIDE {declarations}
        SUCH THAT x <= 1
        MAXIMIZE SUM(x)"""


DECISION = "is a decision; when, per and by may only use data"
NEITHER = "is neither a column nor a relation of the FROM clause"


def _binder_error(sql, message, id):
    return pytest.param(sql, message, id=id, marks=pytest.mark.error_binder)


KEY_ERRORS = [
    _binder_error(_decide("per depot: x(INT)"), f"DECIDE: depot {NEITHER}", "unknown-name"),
    _binder_error(_decide("per D.depot: x(INT)"), f"DECIDE: D.depot {NEITHER}", "unknown-column"),
    _binder_error(_decide("per ship: x(INT), ship(INT)"), f"DECIDE: ship {DECISION}", "decision-declared-after"),
    _binder_error(_decide("ship(INT), per ship: x(INT)"), f"DECIDE: ship {DECISION}", "decision-declared-before"),
    _binder_error(_decide("per x: x(INT)"), f"DECIDE: x {DECISION}", "its-own-name"),
    # A table-scoped decision is a decision by its qualified spelling too ...
    _binder_error(_decide("D.open(BOOL), per D.open: x(INT)"), f"DECIDE: D.open {DECISION}",
                  "table-scoped-decision"),
    # ... but a qualified name over a per decision or a query-wide one names nothing.
    _binder_error(_decide("per D: open(BOOL), per D.open: x(INT)"), f"DECIDE: D.open {NEITHER}",
                  "qualified-per-decision"),
    _binder_error(_decide("per (): cap(INT), per S.cap: x(INT)"), f"DECIDE: S.cap {NEITHER}",
                  "qualified-query-wide-decision"),
    # A decision named like a column is the conflict error, wherever the key sits.
    _binder_error(_decide("per D.region: x(INT), region(INT)"),
                  "DECIDE variable 'region' conflicts with an existing column name.", "conflict-after-qualified-key"),
    _binder_error(_decide("per region: x(INT), region(INT)"),
                  "DECIDE variable 'region' conflicts with an existing column name.", "conflict-after-key"),
    pytest.param(_decide("per S.demand + 1: x(INT)"),
                 "a per key lists columns or relations; put expressions in by (...)", id="expression",
                 marks=pytest.mark.error_parser),
    _binder_error(_decide("per depotID: x(INT)", _from(join="FULL OUTER JOIN")),
                  "DECIDE: depotID is merged by a FULL OUTER JOIN USING; write S.depotID or D.depotID in the per key",
                  "full-outer-using"),
    _binder_error(_decide("per depotID: x(INT)", _from().replace("USING (depotID)", "ON S.depotID = D.depotID")),
                  'Ambiguous reference to column name "depotID"', "ambiguous-column"),
    _binder_error(f"""
        SELECT shipmentID {_from()}
        DECIDE x(INT)
        SUCH THAT x <= (SELECT MAX(y) FROM {_values(SHIPMENTS, 'S2', SHIPMENT_COLUMNS)}
                        DECIDE per D.region: y(INT) SUCH THAT y <= 3 MAXIMIZE SUM(y))
        MAXIMIZE SUM(x)""", f"DECIDE: D.region {NEITHER}", "outer-query-column"),
    _binder_error("""
        SELECT n_nationkey FROM nation n
        DECIDE per n.rowid: x(INT)
        SUCH THAT x <= 1
        MAXIMIZE SUM(x)""", "DECIDE: n.rowid is not a stored column", "rowid"),
]


@pytest.mark.error
@pytest.mark.parametrize("sql, message", KEY_ERRORS)
def test_key_errors(decidb_cli, sql, message):
    result = decidb_cli.execute_raw(sql)
    assert message in result.stderr, result.stderr
    assert_no_internal_leak(result, "a per key error")


@pytest.mark.error
@pytest.mark.error_binder
def test_generated_column_is_not_a_key_column(decidb_cli, tmp_path):
    """A generated column only repeats the stored columns it is computed from: a key may
    not name one, and `per g` leaves it out."""
    script = textwrap.dedent("""
        CREATE TEMP TABLE g(a INTEGER, b INTEGER GENERATED ALWAYS AS (a * 2));
        INSERT INTO g (a) VALUES (1), (1), (2);
        SELECT a, x FROM g DECIDE per g.b: x(INT) SUCH THAT x <= 1 MAXIMIZE SUM(x);
        .mode csv
        SELECT a, x FROM g DECIDE per g: x(INT) SUCH THAT x <= a MAXIMIZE SUM(x) ORDER BY a;
    """)
    result = decidb_cli.execute_script(script)
    combined = result.stdout + result.stderr
    assert "DECIDE: g.b is not a stored column" in combined, combined
    assert "a,x\n1,1\n1,1\n2,2" in result.stdout.replace("\r\n", "\n"), result.stdout
    dump = decidb_cli.dump_model(
        "CREATE TEMP TABLE g(a INTEGER, b INTEGER GENERATED ALWAYS AS (a * 2)); "
        "INSERT INTO g (a) VALUES (1), (1), (2); "
        "SELECT a, x FROM g DECIDE per g: x(INT) SUCH THAT x <= a MAXIMIZE SUM(x)", tmp_path / "g.dump")
    assert _num_vars(dump) == 2


@pytest.mark.error
@pytest.mark.error_binder
def test_per_decision_in_an_old_qualified_reducer_is_refused(decidb_cli):
    result = decidb_cli.execute_raw(f"""
        SELECT shipmentID {_from()}
        DECIDE per D: open(BOOL)
        SUCH THAT SUM(D: capacity * open) <= 50
        MAXIMIZE SUM(open)""")
    assert ("DECIDE: 'open' is declared with per; using it inside SUM(D: ...) is not supported yet"
            in result.stderr), result.stderr
    assert_no_internal_leak(result, "a per decision inside SUM(D: ...)")


@pytest.mark.error
@pytest.mark.error_binder
def test_aggregate_constraint_names_a_per_decision_as_written(decidb_cli):
    """A keyed decision beside a reducer is refused, naming it the way it was declared."""
    result = decidb_cli.execute_raw(f"""
        SELECT shipmentID {_from()}
        DECIDE per D.depotID: open(INT), x(INT)
        SUCH THAT SUM(x) + open <= 5 AND x <= 1
        MAXIMIZE SUM(x)""")
    assert ("decision 'open' (declared per D.depotID) cannot appear outside a reducer in an aggregate "
            "constraint. Put it inside SUM/AVG/MIN/MAX, or declare it per () when one query-wide value "
            "is intended." in result.stderr), result.stderr
    assert_no_internal_leak(result, "a per decision beside a reducer")
