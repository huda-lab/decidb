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
  - Errors: multi-relation qualifier, foreign column, foreign / row-scoped / query-wide
    decision inside the reducer, unknown relation
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
    trips a pre-existing Gurobi load failure — see 07_issues/bugs/todo.md.
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


@pytest.mark.error_binder
def test_multi_relation_qualifier_rejected(decidb_cli):
    """`sum(n,c: ...)` is not supported; the message names the single-relation form."""
    with pytest.raises(DecidBCliError, match="qualified by one relation only"):
        decidb_cli.execute(_REJECT_BASE.format(
            extra_decls="",
            constraint="SUM(keepN) <= 5",
            objective="SUM(n,c: n.n_nationkey * keepN)"))


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
def test_query_wide_decision_inside_qualified_reducer_rejected(decidb_cli):
    """A `scalar` decision inside a reducer is rejected whether or not the reducer
    is qualified: the two readings (coefficient 1 vs. coefficient n) are different
    problems, so neither is chosen silently."""
    with pytest.raises(DecidBCliError, match="query-wide decision"):
        decidb_cli.execute(_REJECT_BASE.format(
            extra_decls=", scalar cap(INT)",
            constraint="cap <= 5",
            objective="SUM(n: n.n_nationkey * keepN + cap)"))


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


@pytest.mark.error_binder
@pytest.mark.error
def test_qualified_reducer_with_aggregate_local_when_in_constraint_rejected(
        decidb_cli):
    """The same composition is rejected on the *constraint* side.

    Asymmetric with the objective case above, which works — and the message
    leaks the internal `__qualified_reducer__` tag instead of naming the
    unsupported combination. Both are logged in
    ``07_issues/code_quality/todo.md``; this test pins the current rejection so
    the fix is visible when it lands.
    """
    decidb_cli.assert_error("""
            SELECT c.c_custkey, n.n_nationkey, keepN
            FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
            WHERE n.n_regionkey = 0
            DECIDE n.keepN(BOOL)
            SUCH THAT SUM(n: keepN) WHEN (n.n_nationkey > 1) <= 2
            MAXIMIZE SUM(n: n.n_nationkey * keepN)
        """, match=r"SUCH THAT clause does not support")


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
