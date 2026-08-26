"""Tests for relation-qualified reducers, ``agg(D: expr)``.

A qualified reducer contributes one term per tuple identity of the qualified
relation instead of one per join-result row (paper §3.2.2). The join repeats each
``nation`` row once per customer, so an unqualified ``SUM`` over a nation-scoped
decision weights that decision by the nation's customer count; the qualified form
weights it exactly once. Both semantics coexist — the unqualified form is unchanged.

Covers:
  - SUM qualified: oracle verification, and divergence from the unqualified form
  - AVG qualified: denominator is the distinct-identity count, not the row count
  - MIN / MAX qualified
  - Qualified reducer on the constraint side
  - PER composed with a qualified reducer (de-duplication runs inside the partition)
  - Mixed qualified and unqualified reducers in one objective
  - A scalar decision multiplied by the qualified relation's own data (batch D):
    weighted and de-duplicated the same as an entity-scoped term
  - Errors: multi-relation qualifier, foreign column, foreign / row-scoped decision
    inside the reducer, a query-wide decision standing alone (row-invariant),
    unknown relation
"""

import pytest
from decidb_cli import DecidBCliError
from solver.types import VarType, ObjSense


# ---------------------------------------------------------------------------
# Shared data helper
# ---------------------------------------------------------------------------

def _nation_data(duckdb_conn):
    """Per-nation customer count and acctbal total over the region-0 join.

    Returns (nation_ids, row_count, acctbal_total). `row_count` differing across
    nations is what makes the row-weighted and identity-weighted optima diverge.
    """
    rows = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               COUNT(*),
               SUM(CAST(c.c_acctbal AS DOUBLE))
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        GROUP BY n.n_nationkey
        ORDER BY 1
    """).fetchall()
    nation_ids = [int(r[0]) for r in rows]
    row_count = {int(r[0]): int(r[1]) for r in rows}
    acctbal_total = {int(r[0]): float(r[2]) for r in rows}
    return nation_ids, row_count, acctbal_total


def _keep_by_nation(result, nation_col, keep_col):
    """Collapse the result relation to one keep value per nation, asserting the
    entity-consistency guarantee along the way."""
    values = {}
    for row in result:
        nkey = int(row[nation_col])
        keep = int(row[keep_col])
        if nkey in values:
            assert values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {values[nkey]} vs {keep}"
        values[nkey] = keep
    return values


def _pick_chain_nation(duckdb_conn, min_customers=15):
    """Smallest-by-customer-count nation (by customer/orders/lineitem chain size)
    with enough customers for a `>= 10` keep-constraint to be meaningful — keeps
    the MIP this test solves comparable in size to the nation-scoped tests above."""
    row = duckdb_conn.execute(f"""
        SELECT c.c_nationkey, COUNT(DISTINCT c.c_custkey) n
        FROM customer c
        JOIN orders o ON o.o_custkey = c.c_custkey
        JOIN lineitem l ON l.l_orderkey = o.o_orderkey
        GROUP BY c.c_nationkey
        HAVING COUNT(DISTINCT c.c_custkey) >= {min_customers}
        ORDER BY n LIMIT 1
    """).fetchone()
    assert row is not None, "fixture has no nation with enough customers for this test"
    return int(row[0])


def _customer_order_chain_data(duckdb_conn, nation_key):
    """Per-customer acctbal, distinct order count, and total (order, lineitem) row
    count for one nation's customer/orders/lineitem chain. `order_count` differing
    from `row_count` (which also differs across customers) is what makes the
    composite, single-relation and unqualified qualified reducers diverge: the
    composite scope's tuple identity is (customer, order), so it weights by
    `order_count`; the single-relation scope's is customer alone, weight 1; the
    unqualified form has no identity at all, weight `row_count`."""
    rows = duckdb_conn.execute("""
        SELECT c.c_custkey, MAX(c.c_acctbal),
               COUNT(DISTINCT o.o_orderkey), COUNT(*)
        FROM customer c
        JOIN orders o ON o.o_custkey = c.c_custkey
        JOIN lineitem l ON l.l_orderkey = o.o_orderkey
        WHERE c.c_nationkey = ?
        GROUP BY c.c_custkey
        ORDER BY 1
    """, [nation_key]).fetchall()
    custkeys = [int(r[0]) for r in rows]
    acctbal = {int(r[0]): float(r[1]) for r in rows}
    order_count = {int(r[0]): int(r[2]) for r in rows}
    row_count = {int(r[0]): int(r[3]) for r in rows}
    return custkeys, acctbal, order_count, row_count


# ---------------------------------------------------------------------------
# Test 1: SUM(n: ...) against an oracle that charges each nation once
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_qualified_sum_charges_each_entity_once(decidb_cli, duckdb_conn, oracle_solver,
                                                perf_tracker):
    """`SUM(n: cost * keepN)` charges a nation's cost once, not once per customer."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN * c.c_acctbal) >= 700000
        MINIMIZE SUM(n: (n.n_nationkey + 1) * keepN)
    """
    result, _ = decidb_cli.execute(sql)
    keep = _keep_by_nation(result, nation_col=1, keep_col=2)

    nation_ids, _, acctbal_total = _nation_data(duckdb_conn)

    oracle_solver.create_model("qualified_sum")
    names = {n: f"keepN_{n}" for n in nation_ids}
    for n in nation_ids:
        oracle_solver.add_variable(names[n], VarType.BINARY)
    # The constraint is unqualified, so it keeps SQL's row semantics: each nation
    # contributes the acctbal of every customer row it joins with.
    oracle_solver.add_constraint(
        {names[n]: acctbal_total[n] for n in nation_ids},
        ">=", 700000.0, name="acctbal_floor",
    )
    # The objective is qualified, so each nation contributes its cost exactly once.
    oracle_solver.set_objective(
        {names[n]: float(n + 1) for n in nation_ids}, ObjSense.MINIMIZE,
    )
    oracle = oracle_solver.solve()

    decidb_obj = sum((n + 1) * keep.get(n, 0) for n in nation_ids)
    assert abs(decidb_obj - oracle.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={oracle.objective_value}"


# ---------------------------------------------------------------------------
# Test 2: the qualified and unqualified forms are genuinely different problems
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_qualified_and_unqualified_diverge(decidb_cli, duckdb_conn, oracle_solver,
                                           perf_tracker):
    """Same query, qualifier removed: the unqualified reducer weights each nation by
    its customer count, and each form matches its own oracle."""
    template = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN * c.c_acctbal) >= 700000
        MINIMIZE SUM({reducer})
    """
    nation_ids, row_count, acctbal_total = _nation_data(duckdb_conn)
    assert len(set(row_count.values())) > 1, \
        "fixture must have nations of differing degree for this test to mean anything"

    def solve_oracle(weight, tag):
        oracle_solver.create_model(f"qualified_divergence_{tag}")
        names = {n: f"keepN_{n}" for n in nation_ids}
        for n in nation_ids:
            oracle_solver.add_variable(names[n], VarType.BINARY)
        oracle_solver.add_constraint(
            {names[n]: acctbal_total[n] for n in nation_ids},
            ">=", 700000.0, name="acctbal_floor",
        )
        oracle_solver.set_objective(
            {names[n]: weight(n) for n in nation_ids}, ObjSense.MINIMIZE,
        )
        return oracle_solver.solve().objective_value

    qualified, _ = decidb_cli.execute(
        template.format(reducer="n: (n.n_nationkey + 1) * keepN"))
    unqualified, _ = decidb_cli.execute(
        template.format(reducer="(n.n_nationkey + 1) * keepN"))

    q_keep = _keep_by_nation(qualified, 1, 2)
    u_keep = _keep_by_nation(unqualified, 1, 2)

    q_obj = sum((n + 1) * q_keep.get(n, 0) for n in nation_ids)
    u_obj = sum((n + 1) * row_count[n] * u_keep.get(n, 0) for n in nation_ids)

    assert abs(q_obj - solve_oracle(lambda n: float(n + 1), "q")) < 1e-4
    assert abs(u_obj - solve_oracle(
        lambda n: float((n + 1) * row_count[n]), "u")) < 1e-4

    # The two forms are different optimization problems: scoring the unqualified
    # solution under the qualified objective must not beat the qualified optimum.
    u_under_q = sum((n + 1) * u_keep.get(n, 0) for n in nation_ids)
    assert q_obj <= u_under_q + 1e-6

    # On this fixture they land on different nation sets. If a data change makes the
    # two optima coincide the test stops proving anything, so pin it rather than let
    # it pass vacuously — retune the threshold if this fires.
    assert q_keep != u_keep, \
        "qualified and unqualified reducers chose the same solution; retune the threshold"


# ---------------------------------------------------------------------------
# Test 2b (batch E): a composite qualifier needs a third relation to prove itself
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_three_relation_composite_qualifier_differs_from_single_and_unqualified(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """`sum(c,o: ...)` over a customer/orders/lineitem chain. The composite
    qualifier's tuple identity is (customer, order): it collapses only the fan-out
    lineitem (unnamed) contributes, keeping every distinct order a customer has —
    unlike `sum(c: ...)` (collapses all the way to one term per customer) and the
    unqualified form (one term per join row, i.e. per lineitem). The two-relation
    case above cannot tell these apart; three relations can (batch E1)."""
    nation_key = _pick_chain_nation(duckdb_conn)
    custkeys, acctbal, order_count, row_count = _customer_order_chain_data(duckdb_conn, nation_key)
    assert len(set(order_count.values())) > 1 and len(set(row_count.values())) > 1, \
        "fixture must have customers of differing order/row degree for this test to mean anything"

    template = """
        SELECT c.c_custkey, o.o_orderkey, l.l_linenumber, keepC
        FROM customer c
        JOIN orders o ON o.o_custkey = c.c_custkey
        JOIN lineitem l ON l.l_orderkey = o.o_orderkey
        WHERE c.c_nationkey = {nation_key}
        DECIDE c.keepC(BOOL)
        SUCH THAT SUM(c: keepC) >= 10
        MINIMIZE SUM({reducer})
    """

    def solve_oracle(weight, tag):
        oracle_solver.create_model(f"composite_qualifier_{tag}")
        names = {ck: f"keepC_{ck}" for ck in custkeys}
        for ck in custkeys:
            oracle_solver.add_variable(names[ck], VarType.BINARY)
        oracle_solver.add_constraint(
            {names[ck]: 1.0 for ck in custkeys}, ">=", 10.0, name="keep_floor")
        oracle_solver.set_objective(
            {names[ck]: weight(ck) for ck in custkeys}, ObjSense.MINIMIZE)
        return oracle_solver.solve().objective_value

    single, _ = decidb_cli.execute(
        template.format(nation_key=nation_key, reducer="c: c.c_acctbal * keepC"))
    composite, _ = decidb_cli.execute(
        template.format(nation_key=nation_key, reducer="c,o: c.c_acctbal * keepC"))
    unqualified, _ = decidb_cli.execute(
        template.format(nation_key=nation_key, reducer="c.c_acctbal * keepC"))

    single_keep = _keep_by_nation(single, 0, 3)
    composite_keep = _keep_by_nation(composite, 0, 3)
    unqualified_keep = _keep_by_nation(unqualified, 0, 3)

    single_obj = sum(acctbal[ck] * single_keep.get(ck, 0) for ck in custkeys)
    composite_obj = sum(acctbal[ck] * order_count[ck] * composite_keep.get(ck, 0) for ck in custkeys)
    unqualified_obj = sum(acctbal[ck] * row_count[ck] * unqualified_keep.get(ck, 0) for ck in custkeys)

    assert abs(single_obj - solve_oracle(lambda ck: acctbal[ck], "single")) < 1e-4
    assert abs(composite_obj - solve_oracle(
        lambda ck: acctbal[ck] * order_count[ck], "composite")) < 1e-4
    assert abs(unqualified_obj - solve_oracle(
        lambda ck: acctbal[ck] * row_count[ck], "unqualified")) < 1e-4

    # Genuinely three different optimization problems, not just three spellings of
    # the same one. Composite vs. single-relation lands on a different 10-customer
    # set on this fixture; composite vs. unqualified is pinned on the objective
    # value instead — order_count and row_count are correlated enough here that the
    # cheapest-10 argmin can coincide by chance even though the two formulas (weight
    # by distinct orders vs. weight by every lineitem row) are not the same function.
    assert composite_keep != single_keep, \
        "composite and single-relation qualifiers chose the same solution; retune the fixture"
    assert abs(composite_obj - unqualified_obj) > 1e-4, \
        "composite and unqualified objectives coincided; retune the fixture"


# ---------------------------------------------------------------------------
# Test 3: AVG divides by distinct identities, not by rows
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_qualified_avg_denominator_is_distinct_entities(decidb_cli, duckdb_conn,
                                                        oracle_solver, perf_tracker):
    """`AVG(n: cost * keepN)` averages over nations; the unqualified form averages
    over join rows, so the two scale differently."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN * c.c_acctbal) >= 700000
        MINIMIZE AVG(n: (n.n_nationkey + 1) * keepN)
    """
    result, _ = decidb_cli.execute(sql)
    keep = _keep_by_nation(result, 1, 2)

    nation_ids, _, acctbal_total = _nation_data(duckdb_conn)

    oracle_solver.create_model("qualified_avg")
    names = {n: f"keepN_{n}" for n in nation_ids}
    for n in nation_ids:
        oracle_solver.add_variable(names[n], VarType.BINARY)
    oracle_solver.add_constraint(
        {names[n]: acctbal_total[n] for n in nation_ids},
        ">=", 700000.0, name="acctbal_floor",
    )
    denominator = float(len(nation_ids))
    oracle_solver.set_objective(
        {names[n]: (n + 1) / denominator for n in nation_ids}, ObjSense.MINIMIZE,
    )
    oracle = oracle_solver.solve()

    decidb_obj = sum((n + 1) * keep.get(n, 0) for n in nation_ids) / denominator
    assert abs(decidb_obj - oracle.objective_value) < 1e-4, \
        f"AVG objective mismatch: DecidB={decidb_obj}, Oracle={oracle.objective_value}"


# ---------------------------------------------------------------------------
# Test 4: MIN / MAX accept a qualifier
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.parametrize("agg,sense", [("MIN", "MAXIMIZE"), ("MAX", "MINIMIZE")])
def test_qualified_minmax(decidb_cli, duckdb_conn, agg, sense, perf_tracker):
    """MIN/MAX accept a qualifier. De-duplication cannot change an extremum — every
    row of a tuple identity carries the same value, so dropping duplicates only
    removes repeats of a value already present — and the two forms must agree.

    Nation 0 is excluded so no identity has a zero coefficient, which would pin MIN
    at 0 and make the comparison vacuous. The coefficient is a bare column rather
    than `n_nationkey + 1` because an additive coefficient inside a MIN/MAX objective
    trips a pre-existing Gurobi load failure — see 06_issues/bugs/todo.md.
    """
    template = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND n.n_nationkey > 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) >= 1
        {sense} {agg}({reducer})
    """
    qualified, _ = decidb_cli.execute(template.format(
        sense=sense, agg=agg, reducer="n: n.n_nationkey * keepN"))
    unqualified, _ = decidb_cli.execute(template.format(
        sense=sense, agg=agg, reducer="n.n_nationkey * keepN"))

    nation_ids = [int(r[0]) for r in duckdb_conn.execute("""
        SELECT DISTINCT CAST(n.n_nationkey AS BIGINT)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND n.n_nationkey > 0
        ORDER BY 1
    """).fetchall()]
    q_keep = _keep_by_nation(qualified, 1, 2)
    u_keep = _keep_by_nation(unqualified, 1, 2)

    values = lambda keep: [n * keep.get(n, 0) for n in nation_ids]
    reduce = min if agg == "MIN" else max
    assert reduce(values(q_keep)) == reduce(values(u_keep))


# ---------------------------------------------------------------------------
# Test 5: qualified reducer on the constraint side
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_qualified_reducer_in_constraint(decidb_cli, duckdb_conn, oracle_solver,
                                         perf_tracker):
    """A budget stated once per nation. Under row semantics the same budget would
    be exceeded by a nation's customer count, so the two forms admit different
    feasible sets."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(n: (n.n_nationkey + 1) * keepN) <= 20
        MAXIMIZE SUM(keepN * c.c_acctbal)
    """
    result, _ = decidb_cli.execute(sql)
    keep = _keep_by_nation(result, 1, 2)

    nation_ids, _, acctbal_total = _nation_data(duckdb_conn)

    # The budget must hold with each nation charged exactly once.
    spent = sum((n + 1) * keep.get(n, 0) for n in nation_ids)
    assert spent <= 20, f"qualified budget violated: {spent} > 20"

    oracle_solver.create_model("qualified_constraint")
    names = {n: f"keepN_{n}" for n in nation_ids}
    for n in nation_ids:
        oracle_solver.add_variable(names[n], VarType.BINARY)
    oracle_solver.add_constraint(
        {names[n]: float(n + 1) for n in nation_ids}, "<=", 20.0, name="budget",
    )
    oracle_solver.set_objective(
        {names[n]: acctbal_total[n] for n in nation_ids}, ObjSense.MAXIMIZE,
    )
    oracle = oracle_solver.solve()

    decidb_obj = sum(acctbal_total[n] * keep.get(n, 0) for n in nation_ids)
    assert abs(decidb_obj - oracle.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={oracle.objective_value}"


# ---------------------------------------------------------------------------
# Test 6: PER partitions first, de-duplication runs inside the partition
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_qualified_reducer_with_per(decidb_cli, duckdb_conn, perf_tracker):
    """A per-nation budget stated once per nation. Each PER group holds exactly one
    nation, so every group's charge is that nation's cost — feasible only for the
    nations under the cap."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(n: (n.n_nationkey + 1) * keepN) <= 8 PER n.n_nationkey
        MAXIMIZE SUM(keepN * c.c_acctbal)
    """
    result, _ = decidb_cli.execute(sql)
    keep = _keep_by_nation(result, 1, 2)

    nation_ids, _, _ = _nation_data(duckdb_conn)
    for n in nation_ids:
        charge = (n + 1) * keep.get(n, 0)
        assert charge <= 8, f"nation {n} charged {charge} against a per-nation cap of 8"
        if n + 1 <= 8:
            # Affordable nations are kept: acctbal totals in region 0 are positive.
            assert keep.get(n, 0) == 1, f"affordable nation {n} was not kept"


# ---------------------------------------------------------------------------
# Test 7: mixing qualified and unqualified reducers in one objective
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_mixed_qualified_and_unqualified_objective(decidb_cli, duckdb_conn,
                                                   oracle_solver, perf_tracker):
    """Only the qualified term de-duplicates; the unqualified term beside it keeps
    row semantics."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) >= 3
        MINIMIZE SUM(n: 1000 * keepN) + SUM(keepN * c.c_acctbal)
    """
    result, _ = decidb_cli.execute(sql)
    keep = _keep_by_nation(result, 1, 2)

    nation_ids, row_count, acctbal_total = _nation_data(duckdb_conn)

    oracle_solver.create_model("qualified_mixed")
    names = {n: f"keepN_{n}" for n in nation_ids}
    for n in nation_ids:
        oracle_solver.add_variable(names[n], VarType.BINARY)
    oracle_solver.add_constraint(
        {names[n]: float(row_count[n]) for n in nation_ids},
        ">=", 3.0, name="keep_floor",
    )
    oracle_solver.set_objective(
        {names[n]: 1000.0 + acctbal_total[n] for n in nation_ids}, ObjSense.MINIMIZE,
    )
    oracle = oracle_solver.solve()

    decidb_obj = sum((1000.0 + acctbal_total[n]) * keep.get(n, 0) for n in nation_ids)
    assert abs(decidb_obj - oracle.objective_value) < 1e-4, \
        f"Mixed objective mismatch: DecidB={decidb_obj}, Oracle={oracle.objective_value}"


# ---------------------------------------------------------------------------
# Test 7b: A scalar decision multiplied by the qualified relation's own data
# (batch D). The body is not row-invariant -- `n.n_nationkey` varies per
# nation -- so `SUM(n: (nationkey+1) * cap)` is legal and means
# `(sum over distinct nations of nationkey+1) * cap`, charged once per nation
# and not once per customer row.
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_qualified_reducer_scalar_times_entity_data_is_weighted_and_deduplicated(
        decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """D1: `SUM(n: (nationkey+1) * cap)` charges each nation's weight once, not
    once per customer row -- the qualifier's de-duplication applies to a scalar's
    term exactly as it does to an entity-scoped one."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, cap
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE scalar cap(INT)
        SUCH THAT SUM(n: (n.n_nationkey + 1) * cap) <= 300
        MAXIMIZE cap
    """
    result, _ = decidb_cli.execute(sql)
    cap_values = {int(row[2]) for row in result}
    assert len(cap_values) == 1, f"cap must be one value for the query, got {cap_values}"
    cap_value = cap_values.pop()

    nation_ids, row_count, _ = _nation_data(duckdb_conn)
    assert max(row_count.values()) > 1, \
        "fixture must have a nation with more than one customer row for dedup to matter"
    weight_sum = sum(n + 1 for n in nation_ids)  # each nation counted once

    oracle_solver.create_model("qualified_scalar_weighted")
    oracle_solver.add_variable("cap", VarType.INTEGER, lb=0.0)
    oracle_solver.add_constraint({"cap": float(weight_sum)}, "<=", 300.0, name="cap_bound")
    oracle_solver.set_objective({"cap": 1.0}, ObjSense.MAXIMIZE)
    oracle_cap = oracle_solver.solve().objective_value

    assert abs(cap_value - oracle_cap) < 1e-4, \
        f"cap mismatch: DecidB={cap_value}, Oracle={oracle_cap}"


# ---------------------------------------------------------------------------
# Tests 8-12: rejections
# ---------------------------------------------------------------------------

_REJECT_BASE = """
    SELECT c.c_custkey, n.n_nationkey, keepN
    FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
    WHERE n.n_regionkey = 0
    DECIDE n.keepN(BOOL){extra_decls}
    SUCH THAT {constraint}
    MINIMIZE {objective}
"""


@pytest.mark.correctness
def test_two_relation_composite_qualifier_equals_unqualified_when_query_has_only_those_two_relations(
        decidb_cli, duckdb_conn):
    """`sum(n,c: ...)` is now legal (batch E). Its composite tuple identity is the
    concatenation of n's and c's own keys — which, in a query joining only n and c,
    *is* the join-result row, so de-duplication is a no-op and the composite form
    must match the unqualified form exactly. (Batch E1: the paper's own two-relation
    example can't tell a composite scope apart from "no scope" — the two only
    diverge once a third, unnamed relation is in the join and fans out; see
    test_three_relation_composite_qualifier_differs_from_single_and_unqualified.)"""
    template = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN * c.c_acctbal) >= 700000
        MINIMIZE SUM({reducer})
    """
    composite, _ = decidb_cli.execute(template.format(reducer="n,c: n.n_nationkey * keepN"))
    unqualified, _ = decidb_cli.execute(template.format(reducer="n.n_nationkey * keepN"))

    # Row-for-row identical output, not just the same objective: every (customer,
    # nation) row keeps the same keepN either way.
    assert composite == unqualified


@pytest.mark.error_binder
def test_unknown_relation_in_multi_relation_qualifier_rejected(decidb_cli):
    """`sum(n,bogus: ...)`: the same "not in the FROM clause" rejection as the
    single-relation case, naming the unresolvable relation."""
    with pytest.raises(DecidBCliError, match="not in the FROM clause"):
        decidb_cli.execute(_REJECT_BASE.format(
            extra_decls="",
            constraint="SUM(keepN) <= 5",
            objective="SUM(n,bogus: n.n_nationkey * keepN)"))


@pytest.mark.error_binder
def test_foreign_column_inside_qualified_reducer_rejected(decidb_cli):
    """§3.2.2: everything inside `SUM(n: ...)` must come from `n`."""
    with pytest.raises(DecidBCliError, match="does not come from n"):
        decidb_cli.execute(_REJECT_BASE.format(
            extra_decls="",
            constraint="SUM(keepN) <= 5",
            objective="SUM(n: c.c_acctbal * keepN)"))


@pytest.mark.error_binder
def test_row_scoped_decision_inside_qualified_reducer_rejected(decidb_cli):
    """A row-scoped decision belongs to no relation, so it cannot be de-duplicated
    by one — the message points at declaring it on the qualified relation."""
    with pytest.raises(DecidBCliError, match="is not a decision of n"):
        decidb_cli.execute(_REJECT_BASE.format(
            extra_decls=", y(INT)",
            constraint="y <= 5",
            objective="SUM(n: n.n_nationkey * keepN + y)"))


@pytest.mark.error_binder
def test_query_wide_decision_alone_inside_qualified_reducer_rejected(decidb_cli):
    """A `scalar` decision standing *alone* inside a qualified reducer is still
    rejected: the body is row-invariant, so there is nothing for the reducer to
    de-duplicate or sum over (batch D: "row-invariant", not "contains a scalar" --
    a scalar *combined with* the qualified relation's own data is legal instead,
    see test_qualified_reducer_scalar_times_entity_data_is_weighted_and_deduplicated)."""
    with pytest.raises(DecidBCliError, match="query-wide decision"):
        decidb_cli.execute(_REJECT_BASE.format(
            extra_decls=", scalar cap(INT)",
            constraint="cap <= 5",
            objective="SUM(n: cap)"))


@pytest.mark.error_binder
def test_unknown_relation_qualifier_rejected(decidb_cli):
    with pytest.raises(DecidBCliError, match="not in the FROM clause"):
        decidb_cli.execute(_REJECT_BASE.format(
            extra_decls="",
            constraint="SUM(keepN) <= 5",
            objective="SUM(supplier: n.n_nationkey * keepN)"))


# ---------------------------------------------------------------------------
# Test 9: Hard-direction MIN / MAX with a qualifier
#
# Test 4 covers the easy directions, which become per-row constraints with no
# auxiliaries. The hard directions add a global auxiliary plus one Big-M
# indicator per active row, so the de-duplication mask and the indicator rows
# meet for the first time here.
# ---------------------------------------------------------------------------

@pytest.mark.min_max
@pytest.mark.correctness
def test_qualified_hard_max_objective(decidb_cli, duckdb_conn, perf_tracker):
    """`MAXIMIZE MAX(n: ...)` — the Big-M direction.

    `done.md` claims MIN/MAX are unaffected by de-duplication because every row
    of an identity carries the same value. That reasoning should survive the
    Big-M encoding too, which is what this checks: the qualified and unqualified
    forms must reach the same extremum.
    """
    template = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND n.n_nationkey > 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(n: keepN) <= 2
        MAXIMIZE MAX({reducer})
    """
    qualified, _ = decidb_cli.execute(
        template.format(reducer="n: n.n_nationkey * keepN"))
    unqualified, _ = decidb_cli.execute(
        template.format(reducer="n.n_nationkey * keepN"))

    nation_ids = [int(r[0]) for r in duckdb_conn.execute("""
        SELECT DISTINCT CAST(n.n_nationkey AS BIGINT)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND n.n_nationkey > 0
        ORDER BY 1
    """).fetchall()]

    q_keep = _keep_by_nation(qualified, 1, 2)
    u_keep = _keep_by_nation(unqualified, 1, 2)
    q_max = max(n * q_keep.get(n, 0) for n in nation_ids)
    u_max = max(n * u_keep.get(n, 0) for n in nation_ids)

    assert q_max == u_max, \
        f"qualifier changed the extremum: qualified={q_max}, unqualified={u_max}"
    assert q_max == max(nation_ids), \
        "the objective should reach the largest available nationkey"
    assert sum(q_keep.values()) <= 2, "qualified budget violated"


@pytest.mark.min_max
@pytest.mark.correctness
def test_qualified_hard_min_constraint(decidb_cli, duckdb_conn, perf_tracker):
    """`MIN(n: ...) <= K` — the hard direction on the constraint side."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND n.n_nationkey > 0
        DECIDE n.keepN(BOOL)
        SUCH THAT MIN(n: n.n_nationkey * keepN) <= 1 AND SUM(n: keepN) >= 2
        MAXIMIZE SUM(n: keepN)
    """
    result, _ = decidb_cli.execute(sql)
    keep = _keep_by_nation(result, 1, 2)

    nation_ids = [int(r[0]) for r in duckdb_conn.execute("""
        SELECT DISTINCT CAST(n.n_nationkey AS BIGINT)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND n.n_nationkey > 0
        ORDER BY 1
    """).fetchall()]

    values = [n * keep.get(n, 0) for n in nation_ids]
    assert min(values) <= 1, f"MIN constraint violated: min={min(values)}"
    assert sum(keep.values()) >= 2, "SUM constraint violated"


# ---------------------------------------------------------------------------
# Test 10: Qualifier composed with an aggregate-local WHEN
#
# The qualifier mask is ANDed into the same `TermFilterState` slot that
# aggregate-local WHEN uses, so these two are the only masks that share a slot.
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_qualified_reducer_with_aggregate_local_when_in_objective(
        decidb_cli, duckdb_conn, perf_tracker):
    """Both masks apply, and the qualifier survives the composition.

    Nations 5 (9 rows) and 14 (2 rows) are chosen so row-weighting inverts the
    ranking: identity weights are 5 and 14, row weights 45 and 28. With a budget
    of one nation, the qualified form must pick 14 and the unqualified 5 — so a
    dropped qualifier is visible in the answer, not just the objective.
    """
    template = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_nationkey IN (5, 14) AND c.c_custkey <= 200
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(n: keepN) <= 1
        MAXIMIZE SUM({reducer}) WHEN (n.n_nationkey > 1)
    """
    counts = {
        int(r[0]): int(r[1]) for r in duckdb_conn.execute("""
            SELECT CAST(n.n_nationkey AS BIGINT), COUNT(*)
            FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
            WHERE n.n_nationkey IN (5, 14) AND c.c_custkey <= 200
            GROUP BY 1
        """).fetchall()
    }
    assert 5 * counts[5] > 14 * counts[14], \
        "fixture no longer inverts the ranking; pick different nations"

    qualified, _ = decidb_cli.execute(
        template.format(reducer="n: n.n_nationkey * keepN"))
    unqualified, _ = decidb_cli.execute(
        template.format(reducer="n.n_nationkey * keepN"))

    q_keep = _keep_by_nation(qualified, 1, 2)
    u_keep = _keep_by_nation(unqualified, 1, 2)

    assert q_keep.get(14) == 1 and q_keep.get(5) == 0, \
        f"qualified form should charge each nation once and pick 14, got {q_keep}"
    assert u_keep.get(5) == 1 and u_keep.get(14) == 0, \
        f"unqualified form should be row-weighted and pick 5, got {u_keep}"


@pytest.mark.correctness
def test_aggregate_local_when_filters_inside_a_qualified_reducer(
        decidb_cli, perf_tracker):
    """The WHEN mask still excludes rows once the qualifier is in play.

    `n_nationkey < 10` drops nation 14 from the term entirely, so the only
    profitable choice left is nation 5 — the opposite of the previous test's
    answer, driven purely by the local filter.
    """
    result, _ = decidb_cli.execute("""
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_nationkey IN (5, 14) AND c.c_custkey <= 200
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(n: keepN) <= 1
        MAXIMIZE SUM(n: n.n_nationkey * keepN) WHEN (n.n_nationkey < 10)
    """)
    keep = _keep_by_nation(result, 1, 2)
    assert keep.get(5) == 1, \
        f"nation 14 is filtered out of the objective, so 5 is the only gain: {keep}"


@pytest.mark.correctness
def test_qualified_reducer_with_aggregate_local_when_in_constraint(
        decidb_cli):
    """The same composition (WHEN before the comparison) now works on the
    *constraint* side too, matching the objective case above.

    Nation 5 sits outside the WHEN filter (`n_nationkey > 5`), so it never
    counts against the `<= 1` cap and is free to keep; nations 14/15/16 do
    count, and only one of them fits under the cap, so the value-maximizing
    choice (16) is the unique optimum. If the WHEN filter were silently
    dropped (folding back to the old bug, which swallowed it into a whole-
    constraint condition applied to nothing), nation 5 would compete for the
    same budget and get dropped in favor of 16 alone — so nation 5's keep
    value is what distinguishes a correctly scoped WHEN from a broken one.
    """
    result, _ = decidb_cli.execute("""
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_nationkey IN (5, 14, 15, 16)
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(n: keepN) WHEN (n.n_nationkey > 5) <= 1
        MAXIMIZE SUM(n: n.n_nationkey * keepN)
    """)
    keep = _keep_by_nation(result, 1, 2)
    assert keep == {5: 1, 14: 0, 15: 0, 16: 1}, \
        f"nation 5 should be free (kept) and 16 should win the capped budget, got {keep}"


# ---------------------------------------------------------------------------
# Test 11: Composed MIN/MAX silently drops the qualifier — KNOWN DEFECT
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_composed_minmax_preserves_the_qualifier(decidb_cli, duckdb_conn,
                                                 perf_tracker):
    """Adding `+ MAX(keepN)` to a qualified reducer must not change what the
    qualifier means.

    Same inverting fixture as the WHEN test: with a one-nation budget the
    qualified form picks 14 (identity weights 5 vs 14) and the unqualified form
    would pick 5 (row weights 45 vs 28).

    Regression for the dropped-qualifier bug: `ComposedMinMaxTerm` carried no
    `qualifier_scope_idx`, so the de-duplication mask never reached the composed
    path and the reducer silently reverted to row semantics — answering 5. The
    control is the same query without the composed term, which is what makes a
    failure here a dropped qualifier rather than a different problem.
    """
    template = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_nationkey IN (5, 14) AND c.c_custkey <= 200
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(n: keepN) <= 1
        MAXIMIZE SUM(n: n.n_nationkey * keepN){extra}
    """
    control, _ = decidb_cli.execute(template.format(extra=""))
    composed, _ = decidb_cli.execute(template.format(extra=" + MAX(keepN)"))

    control_keep = _keep_by_nation(control, 1, 2)
    composed_keep = _keep_by_nation(composed, 1, 2)

    assert control_keep.get(14) == 1, \
        f"control lost its qualifier too — this test no longer isolates the defect: {control_keep}"
    assert composed_keep.get(14) == 1, (
        f"composed MIN/MAX dropped the qualifier: picked {composed_keep}, "
        f"control picked {control_keep}")


@pytest.mark.correctness
def test_composed_minmax_preserves_the_qualifier_in_a_constraint(decidb_cli,
                                                                 perf_tracker):
    """The composed *constraint* path is separate code from the objective path,
    so it needs its own pin.

    Budget 20 against `SUM(n: nationkey * keepN) + MAX(keepN)`. Under identity
    semantics both nations fit: 5 + 14 + 1 = 20. Under row semantics neither
    does on its own (45 + 1 and 28 + 1 both exceed 20), so a dropped qualifier
    collapses the answer to the empty selection.
    """
    template = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_nationkey IN (5, 14) AND c.c_custkey <= 200
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM({reducer}) + MAX(keepN) <= 20
        MAXIMIZE SUM(n: keepN)
    """
    qualified, _ = decidb_cli.execute(
        template.format(reducer="n: n.n_nationkey * keepN"))
    unqualified, _ = decidb_cli.execute(
        template.format(reducer="n.n_nationkey * keepN"))

    q_keep = _keep_by_nation(qualified, 1, 2)
    u_keep = _keep_by_nation(unqualified, 1, 2)

    assert q_keep.get(5) == 1 and q_keep.get(14) == 1, \
        f"identity semantics fit both nations under the budget, got {q_keep}"
    assert sum(u_keep.values()) == 0, \
        f"row semantics fit neither nation under the budget, got {u_keep}"


# ---------------------------------------------------------------------------
# Test 13: a qualified reducer as the BOUND, not the model side
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_qualified_reducer_as_a_bound(decidb_cli, duckdb_conn, oracle_solver):
    """``SUM(n: keepN) <= SUM(n: n_nationkey) / 25`` — the qualifier de-duplicates the
    right-hand side too.

    The right side has always had the machinery (the physical layer runs the same
    ``BuildQualifierKeepMask`` for a right-hand reducer that the left side uses), but
    it was unreachable: the binder's RHS check validated the qualifier wrapper's second
    child — a relation alias — as if it were a value on the bound side, and rejected the
    whole constraint. Reachable since the canonicalization refactor.

    The de-duplication is the whole test. Four distinct nations sum to 50, so the bound
    is 2; the join repeats each nation once per customer, and the row-weighted sum is
    3057, which would make the bound 122 and let every nation be kept.
    """
    result, _ = decidb_cli.execute("""
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_nationkey IN (5, 14, 15, 16)
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(n: keepN) <= SUM(n: n.n_nationkey) / 25
        MAXIMIZE SUM(n: n.n_nationkey * keepN)
    """)
    keep = _keep_by_nation(result, nation_col=1, keep_col=2)

    nation_ids = [5, 14, 15, 16]
    distinct_sum, row_weighted = duckdb_conn.execute("""
        SELECT (SELECT SUM(n_nationkey) FROM
                  (SELECT DISTINCT n_nationkey FROM nation WHERE n_nationkey IN (5,14,15,16))),
               (SELECT SUM(n.n_nationkey) FROM customer c
                  JOIN nation n ON c.c_nationkey = n.n_nationkey
                  WHERE n.n_nationkey IN (5,14,15,16))
    """).fetchone()
    assert float(distinct_sum) / 25 < float(row_weighted) / 25, \
        "fixture must make de-duplicated and row-weighted bounds differ"

    oracle_solver.create_model("qualified_reducer_as_bound")
    names = {n: f"keepN_{n}" for n in nation_ids}
    for n in nation_ids:
        oracle_solver.add_variable(names[n], VarType.BINARY)
    # Both sides qualified: each nation is charged once, and the bound is the
    # de-duplicated sum.
    oracle_solver.add_constraint(
        {names[n]: 1.0 for n in nation_ids},
        "<=", float(distinct_sum) / 25, name="entity_cap",
    )
    oracle_solver.set_objective(
        {names[n]: float(n) for n in nation_ids}, ObjSense.MAXIMIZE,
    )
    oracle = oracle_solver.solve()

    decidb_obj = sum(n * keep.get(n, 0) for n in nation_ids)
    assert abs(decidb_obj - oracle.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={oracle.objective_value}"
    assert sum(keep.values()) == 2, \
        f"row-weighted bound would admit all four nations, got {keep}"
