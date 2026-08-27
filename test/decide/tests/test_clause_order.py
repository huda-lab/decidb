"""Tests for the two accepted DECIDE clause orders.

The declaration may sit between ``SELECT`` and ``FROM`` — the paper's order,
Figure 1 — with the constraints and objective after the joins, or the whole
clause may sit in one block after ``WHERE``. Both parse to the same
``PGDecideClause``, so nothing downstream of the parser distinguishes them
(see ``03_expressivity/decide/done.md`` → "Two clause orders").

Covers:
  - Split order solves correctly, oracle verified
  - The two orders return the same rows and the same physical plan
  - All three declaration scopes are accepted in the split slot
  - The ``in_decide_clause`` lexer flag is cleared between the slots, so
    ``CASE WHEN`` in an intervening ``JOIN ... ON`` or ``WHERE`` still lexes
    as ordinary SQL
  - Errors: declaring in both slots, a declaration with no ``SUCH THAT``, and
    a ``SUCH THAT`` with no declaration
"""

import re

import pytest
from solver.types import VarType, ObjSense


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _normalize_plan(text: str) -> str:
    """Strip the echoed query and box-drawing padding from EXPLAIN output.

    Leaves only the operator/attribute words, so two plans that differ solely
    in how the source text wrapped still compare equal.
    """
    body = text[text.find("Physical Plan"):] if "Physical Plan" in text else text
    return " ".join(re.findall(r"[A-Za-z_][A-Za-z_0-9.]*|[<>=+*/-]|\d+", body))


# ---------------------------------------------------------------------------
# Test 1: The split order solves correctly, oracle verified
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_split_order_oracle(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """The paper's clause order on a join — declaration before FROM, constraints
    and objective after the join — is verified against an independent model.

    The split order is not merely parsed: it has to reassemble into the same
    clause the single-block order builds, so an oracle on this shape is what
    rules out the declaration reaching the binder detached from its constraints.
    """
    sql = """
        SELECT c.c_custkey, keep
        DECIDE keep(BOOL)
        FROM customer c
        JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 300
        SUCH THAT SUM(keep) <= 20
        MAXIMIZE SUM(c.c_acctbal * keep)
    """
    decidb_result, _ = decidb_cli.execute(sql)

    data = duckdb_conn.execute("""
        SELECT CAST(c.c_custkey AS BIGINT), CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 300
        ORDER BY c.c_custkey
    """).fetchall()
    assert len(data) > 20, "fixture must exceed the cardinality cap to bind"

    oracle_solver.create_model("split_clause_order")
    obj = {}
    budget = {}
    for custkey, acctbal in data:
        name = f"keep_{custkey}"
        oracle_solver.add_variable(name, VarType.BINARY)
        obj[name] = acctbal
        budget[name] = 1.0
    oracle_solver.add_constraint(budget, "<=", 20.0, name="count_cap")
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    oracle_obj = oracle_solver.solve().objective_value

    acctbal_by_key = {int(k): float(v) for k, v in data}
    decidb_obj = sum(acctbal_by_key[int(row[0])] * int(row[1]) for row in decidb_result)
    assert abs(decidb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={oracle_obj}"


# ---------------------------------------------------------------------------
# Test 2: The two orders agree, row for row
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_orders_return_the_same_rows(decidb_cli, perf_tracker):
    """One problem written both ways produces one answer.

    The objective is strictly ordered by ``l_extendedprice`` under a quantity
    budget, so the optimum is unique and any difference in how the two orders
    reassembled would show up as a different selection, not an alternate optimum.
    """
    split = """
        SELECT l_orderkey, l_linenumber, x
        DECIDE x(BOOL)
        FROM lineitem
        WHERE l_orderkey < 200
        SUCH THAT SUM(x * l_quantity) <= 150
        MAXIMIZE SUM(x * l_extendedprice)
    """
    single_block = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem
        WHERE l_orderkey < 200
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 150
        MAXIMIZE SUM(x * l_extendedprice)
    """
    split_rows, split_cols = decidb_cli.execute(split)
    single_rows, single_cols = decidb_cli.execute(single_block)

    assert split_cols == single_cols
    assert len(split_rows) > 0, "fixture produced no rows"
    assert sorted(split_rows) == sorted(single_rows)


# ---------------------------------------------------------------------------
# Test 3: The two orders produce the same physical plan
# ---------------------------------------------------------------------------

@pytest.mark.explain
def test_orders_produce_the_same_plan(decidb_cli):
    """Nothing downstream of the parser distinguishes the orders, so the plans
    must match — the strongest available statement of "same clause"."""
    split = """
        SELECT c_custkey, x
        DECIDE x(BOOL)
        FROM customer
        WHERE c_custkey <= 50
        SUCH THAT SUM(x) <= 5 AND x <= 1
        MAXIMIZE SUM(x * c_acctbal)
    """
    single_block = """
        SELECT c_custkey, x
        FROM customer
        WHERE c_custkey <= 50
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 5 AND x <= 1
        MAXIMIZE SUM(x * c_acctbal)
    """
    split_plan = _normalize_plan(decidb_cli.execute_raw(f"EXPLAIN {split}").stdout)
    single_plan = _normalize_plan(decidb_cli.execute_raw(f"EXPLAIN {single_block}").stdout)

    assert "DECIDE" in split_plan
    assert split_plan == single_plan


# ---------------------------------------------------------------------------
# Test 4: All three declaration scopes are accepted in the split slot
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_scalar_declaration_in_split_slot(decidb_cli, perf_tracker):
    """A query-wide declaration parses in the pre-FROM slot and still yields one
    shared column."""
    rows, cols = decidb_cli.execute("""
        SELECT c_custkey, ship, cap
        DECIDE ship(INT), scalar cap(INT)
        FROM customer
        WHERE c_custkey <= 20
        SUCH THAT ship <= cap AND cap <= 4
        MAXIMIZE SUM(ship) - 2 * cap
    """)
    assert len(rows) > 1
    cap_idx = cols.index("cap")
    assert len({row[cap_idx] for row in rows}) == 1, "scalar must be one value"


@pytest.mark.correctness
def test_entity_scoped_declaration_in_split_slot(decidb_cli, perf_tracker):
    """A table-scoped declaration parses in the pre-FROM slot and still shares one
    variable per entity across the join's repeated rows."""
    rows, cols = decidb_cli.execute("""
        SELECT n.n_nationkey, keep
        DECIDE n.keep(BOOL)
        FROM customer c
        JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 200
        SUCH THAT SUM(n: keep) <= 2
        MAXIMIZE SUM(n: n.n_nationkey * keep)
    """)
    assert len(rows) > 0
    key_idx, keep_idx = cols.index("n_nationkey"), cols.index("keep")
    per_entity = {}
    for row in rows:
        per_entity.setdefault(row[key_idx], set()).add(row[keep_idx])
    for nkey, values in per_entity.items():
        assert len(values) == 1, f"nation {nkey} got inconsistent values {values}"
    assert sum(next(iter(v)) for v in per_entity.values()) <= 2


# ---------------------------------------------------------------------------
# Test 5: The lexer flag is cleared between the two slots
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_case_when_in_join_on_between_slots(decidb_cli, perf_tracker):
    """``CASE WHEN`` in a ``JOIN ... ON`` that sits between the two slots must
    lex as ordinary SQL, not as the DECIDE postfix ``WHEN``.

    This is the hazard the split order introduced: the declaration arms the
    DECIDE lexer context, and everything between it and ``SUCH THAT`` is
    plain SQL that must not see it.
    """
    rows, _ = decidb_cli.execute("""
        SELECT c.c_custkey, x
        DECIDE x(BOOL)
        FROM customer c
        JOIN nation n
          ON (CASE WHEN c.c_acctbal > 0 THEN c.c_nationkey ELSE -1 END) = n.n_nationkey
        WHERE c.c_custkey <= 100
        SUCH THAT SUM(x) <= 3
        MAXIMIZE SUM(x * c.c_acctbal)
    """)
    assert len(rows) > 0
    assert sum(row[1] for row in rows) <= 3


@pytest.mark.correctness
def test_case_when_in_where_between_slots(decidb_cli, perf_tracker):
    """Same hazard, in a ``WHERE`` between the two slots."""
    rows, _ = decidb_cli.execute("""
        SELECT c_custkey, x
        DECIDE x(BOOL)
        FROM customer
        WHERE (CASE WHEN c_acctbal > 0 THEN 1 ELSE 0 END) = 1 AND c_custkey <= 100
        SUCH THAT SUM(x) <= 3
        MAXIMIZE SUM(x * c_acctbal)
    """)
    assert len(rows) > 0
    assert sum(row[1] for row in rows) <= 3


# ---------------------------------------------------------------------------
# Test 6: Malformed splits are parser errors
# ---------------------------------------------------------------------------

@pytest.mark.error_parser
@pytest.mark.error
class TestClauseOrderErrors:
    """A declaration belongs in one slot or the other, never both or neither."""

    def test_declaration_in_both_slots_rejected(self, decidb_cli):
        """Declaring in both slots is rejected.

        The parser names the duplicate declaration and tells the user to choose
        either the pre-FROM or the trailing slot. This test pins the rejection;
        the parser unit test pins the exact message.
        """
        decidb_cli.assert_error("""
                SELECT c_custkey, x
                DECIDE x(BOOL)
                FROM customer
                DECIDE x(BOOL)
                SUCH THAT SUM(x) <= 3
                MAXIMIZE SUM(x * c_acctbal)
            """, match=r"(?i)parser error")

    def test_split_declaration_without_such_that_rejected(self, decidb_cli):
        """A pre-FROM declaration with no constraints names the fix."""
        decidb_cli.assert_error("""
                SELECT c_custkey, x
                DECIDE x(BOOL)
                FROM customer WHERE c_custkey <= 10
            """, match=r"DECIDE requires a SUCH THAT clause")

    def test_such_that_without_declaration_rejected(self, decidb_cli):
        """Constraints with nothing declared name the fix."""
        decidb_cli.assert_error("""
                SELECT c_custkey
                FROM customer WHERE c_custkey <= 10
                SUCH THAT SUM(c_acctbal) <= 3
                MAXIMIZE SUM(c_acctbal)
            """, match=r"SUCH THAT requires a DECIDE clause")


# ---------------------------------------------------------------------------
# Test 7: The split order over the remaining source shapes
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_split_order_over_subquery_source(decidb_cli, perf_tracker):
    """`FROM (SELECT ...) t` with the declaration in the pre-FROM slot.

    The declaration names a source that the parser has not read yet — it sits
    textually before the subquery it scopes over. Entity scoping over a subquery
    regressed once before (`04_testing/entity_scope/done.md`), so the
    table-scoped form is what is exercised here.
    """
    rows, cols = decidb_cli.execute("""
        SELECT t.c_custkey, x
        DECIDE t.x(BOOL)
        FROM (SELECT c_custkey, c_acctbal FROM customer WHERE c_custkey <= 60) t
        SUCH THAT SUM(x) <= 5
        MAXIMIZE SUM(t.c_acctbal * x)
    """)
    assert len(rows) == 60
    x_idx = cols.index("x")
    assert sum(int(r[x_idx]) for r in rows) == 5, "budget should bind exactly"


@pytest.mark.correctness
def test_split_order_over_cte_source(decidb_cli, perf_tracker):
    """Same shape via a `WITH` CTE, where the declaration also precedes its source."""
    rows, cols = decidb_cli.execute("""
        WITH cust AS (SELECT c_custkey, c_acctbal FROM customer WHERE c_custkey <= 60)
        SELECT cust.c_custkey, x
        DECIDE x(BOOL)
        FROM cust
        SUCH THAT SUM(x) <= 5
        MAXIMIZE SUM(cust.c_acctbal * x)
    """)
    assert len(rows) == 60
    x_idx = cols.index("x")
    assert sum(int(r[x_idx]) for r in rows) == 5


@pytest.mark.correctness
def test_split_order_three_table_fanout_join(decidb_cli, duckdb_conn,
                                             oracle_solver, perf_tracker):
    """A three-table fan-out join with the declaration before the whole join list,
    oracle-verified.

    The declaration slot closes before the first `JOIN` is even parsed, so this
    is the widest gap the reassembly has to span.
    """
    sql = """
        SELECT o.o_orderkey, l.l_linenumber, x
        DECIDE x(BOOL)
        FROM customer c
        JOIN orders o ON c.c_custkey = o.o_custkey
        JOIN lineitem l ON o.o_orderkey = l.l_orderkey
        WHERE o.o_orderkey < 200
        SUCH THAT SUM(x * l.l_quantity) <= 100
        MAXIMIZE SUM(x * l.l_extendedprice)
    """
    rows, cols = decidb_cli.execute(sql)

    data = duckdb_conn.execute("""
        SELECT CAST(o.o_orderkey AS BIGINT), CAST(l.l_linenumber AS BIGINT),
               CAST(l.l_quantity AS DOUBLE), CAST(l.l_extendedprice AS DOUBLE)
        FROM customer c
        JOIN orders o ON c.c_custkey = o.o_custkey
        JOIN lineitem l ON o.o_orderkey = l.l_orderkey
        WHERE o.o_orderkey < 200
        ORDER BY o.o_orderkey, l.l_linenumber
    """).fetchall()
    assert len(rows) == len(data), \
        f"DeciDB saw {len(rows)} join rows, DuckDB {len(data)}"

    oracle_solver.create_model("split_three_table")
    obj, budget = {}, {}
    for i, (_, _, qty, price) in enumerate(data):
        name = f"x_{i}"
        oracle_solver.add_variable(name, VarType.BINARY)
        obj[name] = price
        budget[name] = qty
    oracle_solver.add_constraint(budget, "<=", 100.0, name="qty_budget")
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    oracle_obj = oracle_solver.solve().objective_value

    price_by_key = {(int(o), int(ln)): p for o, ln, _, p in data}
    ok, ln_i, x_i = cols.index("o_orderkey"), cols.index("l_linenumber"), cols.index("x")
    decidb_obj = sum(price_by_key[(int(r[ok]), int(r[ln_i]))] * int(r[x_i]) for r in rows)
    assert abs(decidb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={oracle_obj}"


# ---------------------------------------------------------------------------
# Test 8: The split order composed with WHEN and PER
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_split_order_with_when(decidb_cli, perf_tracker):
    """`WHEN` lives in the constraints slot, which the split order shares with
    the single-block order — so it should behave identically. Compared directly
    against the single-block spelling rather than asserted in isolation."""
    split = """
        SELECT c_custkey, x
        DECIDE x(BOOL)
        FROM customer
        WHERE c_custkey <= 60
        SUCH THAT SUM(x) <= 5 WHEN (c_acctbal > 0)
        MAXIMIZE SUM(x * c_acctbal)
    """
    single_block = """
        SELECT c_custkey, x
        FROM customer
        WHERE c_custkey <= 60
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 5 WHEN (c_acctbal > 0)
        MAXIMIZE SUM(x * c_acctbal)
    """
    split_rows, _ = decidb_cli.execute(split)
    single_rows, _ = decidb_cli.execute(single_block)
    assert len(split_rows) == 60
    assert sorted(split_rows) == sorted(single_rows)


@pytest.mark.correctness
def test_split_order_with_per(decidb_cli, perf_tracker):
    """`PER` in the split order, compared against the single-block spelling."""
    split = """
        SELECT c_custkey, c_nationkey, x
        DECIDE x(BOOL)
        FROM customer
        WHERE c_custkey <= 60
        SUCH THAT SUM(x) <= 2 PER c_nationkey
        MAXIMIZE SUM(x * c_acctbal)
    """
    single_block = """
        SELECT c_custkey, c_nationkey, x
        FROM customer
        WHERE c_custkey <= 60
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 2 PER c_nationkey
        MAXIMIZE SUM(x * c_acctbal)
    """
    split_rows, split_cols = decidb_cli.execute(split)
    single_rows, _ = decidb_cli.execute(single_block)
    assert sorted(split_rows) == sorted(single_rows)

    nk, x_i = split_cols.index("c_nationkey"), split_cols.index("x")
    per_nation = {}
    for r in split_rows:
        per_nation[r[nk]] = per_nation.get(r[nk], 0) + int(r[x_i])
    assert per_nation, "fixture produced no groups"
    for nation, taken in per_nation.items():
        assert taken <= 2, f"nation {nation} took {taken}, cap is 2"


@pytest.mark.correctness
def test_split_order_with_nested_per_objective(decidb_cli, perf_tracker):
    """A nested-aggregate `PER` objective in the split order."""
    split = """
        SELECT c_custkey, c_nationkey, x
        DECIDE x(BOOL)
        FROM customer
        WHERE c_custkey <= 60
        SUCH THAT SUM(x) <= 6
        MAXIMIZE MIN(SUM(x * c_acctbal)) PER c_nationkey
    """
    single_block = """
        SELECT c_custkey, c_nationkey, x
        FROM customer
        WHERE c_custkey <= 60
        DECIDE x(BOOL)
        SUCH THAT SUM(x) <= 6
        MAXIMIZE MIN(SUM(x * c_acctbal)) PER c_nationkey
    """
    split_rows, _ = decidb_cli.execute(split)
    single_rows, _ = decidb_cli.execute(single_block)
    assert len(split_rows) == 60
    assert sorted(split_rows) == sorted(single_rows)


# ---------------------------------------------------------------------------
# Test 9: There are exactly two slots — not three
# ---------------------------------------------------------------------------

@pytest.mark.error_parser
@pytest.mark.error
def test_declaration_between_from_and_where_rejected(decidb_cli):
    """`FROM ... DECIDE ... WHERE ...` is neither slot and is a parser error.

    The two accepted positions are before `FROM` and after `WHERE`; the gap
    between them is not a third one. Worth pinning because the split order makes
    "somewhere in the middle" a plausible guess.
    """
    decidb_cli.assert_error("""
            SELECT c_custkey, x
            FROM customer
            DECIDE x(BOOL)
            WHERE c_custkey <= 20
            SUCH THAT SUM(x) <= 3
            MAXIMIZE SUM(x * c_acctbal)
        """, match=r"(?i)parser error")
