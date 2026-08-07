"""Tests for table-scoped decision variables.

Covers:
  - Basic entity-scoped variable with oracle verification
  - Entity consistency: same entity → same variable value across join rows
  - INT entity-scoped variable
  - Mixed row-scoped + entity-scoped variables (exercises VarIndexer three-block layout)
  - Entity-scoped with WHEN conditions
  - Error: scoping to nonexistent table
  - Entity-scoped with PER grouping constraints
  - Entity-scoped with MAX constraint (MIN/MAX linearization)
  - Entity-scoped with AVG constraint (scaling)
  - Triple interaction: entity-scoped + WHEN + PER
"""

import pytest
from decidb_cli import DecidBCliError
from solver.types import VarType, ObjSense, SolverStatus

from ._oracle_helpers import add_ne_indicator


# ---------------------------------------------------------------------------
# Test 1: Basic entity-scoped with oracle verification
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_nation_selection(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped variable on nation table: pick nations to maximize customer acctbal.

    Join customer x nation, decide keepN per nation (not per customer).
    Constraint: at most 100 join rows selected (SUM(keepN) <= 100).
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, n.n_name, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) <= 100
        MAXIMIZE SUM(keepN * c.c_acctbal)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    # Verify: rows with the same n_nationkey have the same keepN value
    nation_values = {}
    for row in decidb_result:
        nkey = int(row[1])
        keep = int(row[3])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Build oracle model: one variable per nation in region 0
    data = duckdb_conn.execute("""
        SELECT CAST(c.c_custkey AS BIGINT), CAST(n.n_nationkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
    """).fetchall()

    nation_ids = sorted(set(int(row[1]) for row in data))

    # Aggregated coefficients per nation (SQL semantics: SUM over join rows)
    nation_count = {}
    nation_acctbal = {}
    for row in data:
        nkey = int(row[1])
        acctbal = float(row[2])
        nation_count[nkey] = nation_count.get(nkey, 0) + 1
        nation_acctbal[nkey] = nation_acctbal.get(nkey, 0.0) + acctbal

    oracle_solver.create_model("entity_scope_nation")
    vnames = {nkey: f"keepN_{nkey}" for nkey in nation_ids}
    for nkey in nation_ids:
        oracle_solver.add_variable(vnames[nkey], VarType.BINARY)

    # SUM(keepN) <= 100 counts join rows, so each nation contributes its row count
    oracle_solver.add_constraint(
        {vnames[nkey]: float(nation_count[nkey]) for nkey in nation_ids},
        "<=", 100.0, name="nation_limit",
    )

    oracle_solver.set_objective(
        {vnames[nkey]: nation_acctbal[nkey] for nkey in nation_ids},
        ObjSense.MAXIMIZE,
    )

    result = oracle_solver.solve()
    oracle_obj = result.objective_value

    # Compute DecidB objective
    decidb_obj = 0.0
    for row in data:
        nkey = int(row[1])
        acctbal = float(row[2])
        decidb_obj += nation_values.get(nkey, 0) * acctbal

    assert abs(decidb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={oracle_obj}"


# ---------------------------------------------------------------------------
# Test 2: Entity consistency with tight constraint
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_consistency(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Verify rows from the same entity always have the same variable value.
    Use tight constraint to ensure some nations are excluded."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) <= 5
        MAXIMIZE SUM(keepN * c.c_acctbal)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    # Group by nation and verify consistency
    nation_values = {}
    for row in decidb_result:
        nkey = int(row[1])
        keep = int(row[2])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Inconsistent keepN for nation {nkey}: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Verify constraint holds (SUM counts join rows)
    data = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT), COUNT(*) as cnt
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        GROUP BY n.n_nationkey
    """).fetchall()

    total_selected_rows = 0
    for nkey_row in data:
        nkey = int(nkey_row[0])
        cnt = int(nkey_row[1])
        if nation_values.get(nkey, 0) == 1:
            total_selected_rows += cnt
    assert total_selected_rows <= 5, \
        f"Constraint violated: SUM(keepN) = {total_selected_rows} > 5"

    # Oracle: verify objective optimality
    nation_ids = sorted(nation_values.keys())
    nation_count_map = {int(r[0]): int(r[1]) for r in data}
    nation_acctbal = {}
    for row in duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT), CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
    """).fetchall():
        nkey = int(row[0])
        nation_acctbal[nkey] = nation_acctbal.get(nkey, 0.0) + float(row[1])

    oracle_solver.create_model("entity_scoped_consistency")
    vnames = {nkey: f"keepN_{nkey}" for nkey in nation_ids}
    for nkey in nation_ids:
        oracle_solver.add_variable(vnames[nkey], VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[nkey]: float(nation_count_map[nkey]) for nkey in nation_ids},
        "<=", 5.0, name="nation_limit",
    )
    oracle_solver.set_objective(
        {vnames[nkey]: nation_acctbal.get(nkey, 0.0) for nkey in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()
    decidb_obj = sum(
        nation_values.get(nkey, 0) * nation_acctbal.get(nkey, 0.0)
        for nkey in nation_ids
    )
    assert abs(decidb_obj - oracle_result.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={oracle_result.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test 3: INT entity-scoped variable
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_integer(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped(INT) variable — tests the INTEGER readback path with VarIndexer."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, qty
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND n.n_nationkey <= 5
        DECIDE n.qty(INT)
        SUCH THAT qty <= 3
          AND SUM(qty) <= 10
        MAXIMIZE SUM(qty * c.c_acctbal)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    # Verify consistency: same nation → same qty
    nation_values = {}
    for row in decidb_result:
        nkey = int(row[1])
        q = int(row[2])
        if nkey in nation_values:
            assert nation_values[nkey] == q, \
                f"Nation {nkey} has inconsistent qty: {nation_values[nkey]} vs {q}"
        else:
            nation_values[nkey] = q

    # Verify per-row constraint: qty <= 3
    for nkey, q in nation_values.items():
        assert q <= 3, f"Nation {nkey} has qty={q} > 3"

    # Oracle: INTEGER vars per nation, bounded by 3, aggregate SUM(qty*cnt) <= 10
    data = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND n.n_nationkey <= 5
    """).fetchall()

    nation_ids = sorted(set(int(row[0]) for row in data))
    nation_count = {}
    nation_acctbal = {}
    for row in data:
        nkey = int(row[0])
        nation_count[nkey] = nation_count.get(nkey, 0) + 1
        nation_acctbal[nkey] = nation_acctbal.get(nkey, 0.0) + float(row[1])

    oracle_solver.create_model("entity_scoped_integer")
    vnames = {nkey: f"qty_{nkey}" for nkey in nation_ids}
    for nkey in nation_ids:
        oracle_solver.add_variable(vnames[nkey], VarType.INTEGER, lb=0.0, ub=3.0)
    # SUM(qty) <= 10 counts join rows: SUM_n(qty_n * cnt_n) <= 10
    oracle_solver.add_constraint(
        {vnames[nkey]: float(nation_count[nkey]) for nkey in nation_ids},
        "<=", 10.0, name="sum_qty_limit",
    )
    oracle_solver.set_objective(
        {vnames[nkey]: nation_acctbal[nkey] for nkey in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()
    oracle_obj = oracle_result.objective_value

    decidb_obj = sum(
        nation_values.get(nkey, 0) * nation_acctbal.get(nkey, 0.0)
        for nkey in nation_ids
    )
    assert abs(decidb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={oracle_obj:.4f}"


# ---------------------------------------------------------------------------
# Test 4: Mixed row-scoped + entity-scoped (VarIndexer three-block layout)
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_mixed_with_row_scoped(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Mixed query: entity-scoped keepN (per nation) + row-scoped x (per customer row).
    This exercises the VarIndexer three-block layout where row-scoped and entity-scoped
    variables coexist with different indexing schemes."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, x, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_acctbal > 0
        DECIDE n.keepN(BOOL), x(BOOL)
        SUCH THAT x <= keepN
          AND SUM(x) <= 10
        MAXIMIZE SUM(x * c.c_acctbal)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    # Verify entity consistency for keepN
    nation_values = {}
    for row in decidb_result:
        nkey = int(row[1])
        x_val = int(row[2])
        keep_val = int(row[3])
        if nkey in nation_values:
            assert nation_values[nkey] == keep_val, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep_val}"
        else:
            nation_values[nkey] = keep_val

        # Verify per-row constraint: x <= keepN
        assert x_val <= keep_val, \
            f"Per-row constraint violated: x={x_val} > keepN={keep_val} for custkey={row[0]}"

    # Verify SUM(x) <= 10
    total_x = sum(int(row[2]) for row in decidb_result)
    assert total_x <= 10, f"Aggregate constraint violated: SUM(x) = {total_x} > 10"

    # If a nation has keepN=0, all its customers must have x=0
    for row in decidb_result:
        nkey = int(row[1])
        x_val = int(row[2])
        if nation_values[nkey] == 0:
            assert x_val == 0, \
                f"Nation {nkey} has keepN=0 but customer {row[0]} has x={x_val}"

    # Oracle: binary vars per nation + per customer; x_c <= keepN_{nation(c)}, SUM(x) <= 10
    raw = duckdb_conn.execute("""
        SELECT CAST(c.c_custkey AS BIGINT),
               CAST(n.n_nationkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_acctbal > 0
    """).fetchall()
    customers = [(int(r[0]), int(r[1]), float(r[2])) for r in raw]
    cust_acctbal = {ckey: acctbal for ckey, _, acctbal in customers}
    oracle_nation_ids = sorted(set(nkey for _, nkey, _ in customers))

    oracle_solver.create_model("entity_scoped_mixed")
    kn_vars = {nkey: f"keepN_{nkey}" for nkey in oracle_nation_ids}
    x_cvars = {ckey: f"x_{ckey}" for ckey, _, _ in customers}
    for nkey in oracle_nation_ids:
        oracle_solver.add_variable(kn_vars[nkey], VarType.BINARY)
    for ckey, _, _ in customers:
        oracle_solver.add_variable(x_cvars[ckey], VarType.BINARY)
    for ckey, nkey, _ in customers:
        oracle_solver.add_constraint(
            {x_cvars[ckey]: 1.0, kn_vars[nkey]: -1.0},
            "<=", 0.0, name=f"link_{ckey}",
        )
    oracle_solver.add_constraint(
        {x_cvars[ckey]: 1.0 for ckey, _, _ in customers},
        "<=", 10.0, name="sum_x",
    )
    oracle_solver.set_objective(
        {x_cvars[ckey]: acctbal for ckey, _, acctbal in customers},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()
    decidb_obj = sum(int(row[2]) * cust_acctbal.get(int(row[0]), 0.0)
                     for row in decidb_result)
    assert abs(decidb_obj - oracle_result.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={oracle_result.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test 5: Entity-scoped with WHEN condition
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_with_when(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped variable with WHEN condition on constraint."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN * c.c_acctbal) <= 50000 WHEN c.c_acctbal > 0
        MAXIMIZE SUM(keepN)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    # Verify entity consistency
    nation_values = {}
    for row in decidb_result:
        nkey = int(row[1])
        keep = int(row[2])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Oracle: WHEN filters which rows contribute to constraint;
    # objective is SUM(keepN) counting all join rows
    raw = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
    """).fetchall()
    nation_ids = sorted(set(int(r[0]) for r in raw))
    nation_count = {}
    nation_acctbal_pos = {}
    for r in raw:
        nkey = int(r[0]); acctbal = float(r[1])
        nation_count[nkey] = nation_count.get(nkey, 0) + 1
        if acctbal > 0:
            nation_acctbal_pos[nkey] = nation_acctbal_pos.get(nkey, 0.0) + acctbal

    oracle_solver.create_model("entity_scoped_with_when")
    vnames = {nkey: f"keepN_{nkey}" for nkey in nation_ids}
    for nkey in nation_ids:
        oracle_solver.add_variable(vnames[nkey], VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[nkey]: nation_acctbal_pos.get(nkey, 0.0) for nkey in nation_ids},
        "<=", 50000.0, name="when_constraint",
    )
    oracle_solver.set_objective(
        {vnames[nkey]: float(nation_count[nkey]) for nkey in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()
    decidb_obj = float(sum(int(row[2]) for row in decidb_result))
    assert abs(decidb_obj - oracle_result.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={oracle_result.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test 6: Error — nonexistent table
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_nonexistent_table(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Scoping to a nonexistent table should produce a clear error."""
    sql = """
        SELECT c.c_custkey, x
        FROM customer c
        DECIDE nonexistent.x(BOOL)
        SUCH THAT SUM(x) <= 5
        MAXIMIZE SUM(x * c.c_acctbal)
    """
    with pytest.raises(DecidBCliError, match="not found"):
        decidb_cli.execute(sql)


# ---------------------------------------------------------------------------
# Test 7: Entity-scoped with PER grouping constraint
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.per_clause
def test_entity_scoped_with_per(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped variable with PER constraint on region.

    Tests VarIndexer entity dedup + PER group partitioning interaction.
    SUM(keepN) counts join rows, so each selected nation contributes its
    customer count to the per-region total.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, r.r_name, keepN
        FROM customer c
        JOIN nation n ON c.c_nationkey = n.n_nationkey
        JOIN region r ON n.n_regionkey = r.r_regionkey
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) <= 100 PER r_name
        MAXIMIZE SUM(keepN * c_acctbal)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    assert len(decidb_result) > 0

    nkey_idx = decidb_cols.index("n_nationkey")
    keep_idx = decidb_cols.index("keepN")
    region_idx = decidb_cols.index("r_name")

    # Verify entity consistency: same nation → same keepN
    nation_values = {}
    for row in decidb_result:
        nkey = int(row[nkey_idx])
        keep = int(row[keep_idx])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Verify PER constraint: SUM(keepN) <= 100 per region
    region_sums = {}
    for row in decidb_result:
        region = str(row[region_idx])
        keep = int(row[keep_idx])
        region_sums[region] = region_sums.get(region, 0) + keep

    for region, total in region_sums.items():
        assert total <= 100, \
            f"PER constraint violated: SUM(keepN) = {total} > 100 for region '{region}'"

    # Oracle: one BINARY var per nation, PER constraint per region
    data = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               CAST(r.r_name AS VARCHAR),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c
        JOIN nation n ON c.c_nationkey = n.n_nationkey
        JOIN region r ON n.n_regionkey = r.r_regionkey
    """).fetchall()

    nation_ids = sorted(set(int(row[0]) for row in data))
    nation_region = {}
    nation_count = {}
    nation_acctbal = {}
    for row in data:
        nkey = int(row[0])
        rname = str(row[1])
        nation_region[nkey] = rname
        nation_count[nkey] = nation_count.get(nkey, 0) + 1
        nation_acctbal[nkey] = nation_acctbal.get(nkey, 0.0) + float(row[2])

    oracle_solver.create_model("entity_scoped_per")
    vnames = {nkey: f"keepN_{nkey}" for nkey in nation_ids}
    for nkey in nation_ids:
        oracle_solver.add_variable(vnames[nkey], VarType.BINARY)
    # SUM(keepN) <= 100 PER r_name: per region, sum of (keepN_n * cnt_n) <= 100
    for r in set(nation_region.values()):
        oracle_solver.add_constraint(
            {vnames[nkey]: float(nation_count[nkey])
             for nkey in nation_ids if nation_region[nkey] == r},
            "<=", 100.0, name=f"per_region_{r.replace(' ', '_')}",
        )
    oracle_solver.set_objective(
        {vnames[nkey]: nation_acctbal[nkey] for nkey in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()
    oracle_obj = oracle_result.objective_value

    decidb_obj = sum(
        nation_values.get(nkey, 0) * nation_acctbal.get(nkey, 0.0)
        for nkey in nation_ids
    )
    assert abs(decidb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={oracle_obj:.4f}"


# ---------------------------------------------------------------------------
# Test 8: Entity-scoped with MAX constraint (easy case)
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.min_max
def test_entity_scoped_with_max(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped variable with MAX(expr) <= K constraint (easy case).

    MAX(keepN * c.c_acctbal) <= 8000 means every customer in a selected
    nation must have acctbal <= 8000. Tests MIN/MAX linearization with
    entity-scoped variable indexing.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT MAX(keepN * c.c_acctbal) <= 8000
        MAXIMIZE SUM(keepN)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    assert len(decidb_result) > 0

    # Verify entity consistency
    nation_values = {}
    for row in decidb_result:
        nkey = int(row[1])
        keep = int(row[3])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Verify MAX constraint: for selected nations, all customer acctbal <= 8000
    for row in decidb_result:
        keep = int(row[3])
        acctbal = float(row[2])
        if keep == 1:
            assert acctbal <= 8000 + 1e-4, \
                f"MAX constraint violated: keepN=1 and c_acctbal={acctbal} > 8000"

    # Oracle: MAX easy case → if any customer in nation n has acctbal > 8000,
    # then keepN_n must be 0. Maximize SUM_n keepN_n * cnt_n.
    raw = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               MAX(c.c_acctbal) as max_acctbal,
               COUNT(*) as cnt
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        GROUP BY n.n_nationkey
    """).fetchall()
    nation_max_acctbal = {int(r[0]): float(r[1]) for r in raw}
    nation_cnt9 = {int(r[0]): int(r[2]) for r in raw}
    nation_ids9 = sorted(nation_max_acctbal.keys())

    oracle_solver.create_model("entity_scoped_with_max")
    vnames9 = {nkey: f"keepN_{nkey}" for nkey in nation_ids9}
    for nkey in nation_ids9:
        # MAX easy case: nation with any acctbal > 8000 must be forced to 0
        ub = 0.0 if nation_max_acctbal[nkey] > 8000 else 1.0
        oracle_solver.add_variable(vnames9[nkey], VarType.BINARY, ub=ub)
    oracle_solver.set_objective(
        {vnames9[nkey]: float(nation_cnt9[nkey]) for nkey in nation_ids9},
        ObjSense.MAXIMIZE,
    )
    oracle_result9 = oracle_solver.solve()
    decidb_obj9 = float(sum(int(row[3]) for row in decidb_result))
    assert abs(decidb_obj9 - oracle_result9.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj9:.4f}, Oracle={oracle_result9.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test 10: Entity-scoped with AVG constraint
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.avg_rewrite
def test_entity_scoped_with_avg(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped variable with AVG constraint.

    AVG(keepN * c.c_acctbal) <= 3000 tests the AVG→SUM scaling
    with entity-scoped variables where the row count (N) differs from
    the entity count.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT AVG(keepN * c.c_acctbal) <= 3000
        MAXIMIZE SUM(keepN)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    assert len(decidb_result) > 0

    # Verify entity consistency
    nation_values = {}
    for row in decidb_result:
        nkey = int(row[1])
        keep = int(row[2])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Verify AVG constraint: SUM(keepN * acctbal) / N <= 3000
    data = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
    """).fetchall()

    n_total = len(data)
    weighted_sum = sum(
        nation_values.get(int(row[0]), 0) * float(row[1])
        for row in data
    )
    avg_val = weighted_sum / n_total
    assert avg_val <= 3000 + 1e-4, \
        f"AVG constraint violated: AVG(keepN * c_acctbal) = {avg_val:.2f} > 3000"

    # Oracle: AVG rewrite → SUM(keepN * acctbal) <= 3000 * n_total
    oracle_solver.create_model("entity_scoped_with_avg")
    nation_ids = sorted(nation_values.keys())
    nation_count2 = {}
    for r in data:
        nkey = int(r[0])
        nation_count2[nkey] = nation_count2.get(nkey, 0) + 1
    nation_acctbal2 = {}
    for r in data:
        nkey = int(r[0])
        nation_acctbal2[nkey] = nation_acctbal2.get(nkey, 0.0) + float(r[1])

    vnames = {nkey: f"keepN_{nkey}" for nkey in nation_ids}
    for nkey in nation_ids:
        oracle_solver.add_variable(vnames[nkey], VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[nkey]: nation_acctbal2.get(nkey, 0.0) for nkey in nation_ids},
        "<=", 3000.0 * n_total, name="avg_limit",
    )
    oracle_solver.set_objective(
        {vnames[nkey]: float(nation_count2.get(nkey, 0)) for nkey in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()
    decidb_obj = float(sum(int(row[2]) for row in decidb_result))
    assert abs(decidb_obj - oracle_result.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={oracle_result.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test 11: Triple interaction — entity_scope + WHEN + PER
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.per_clause
@pytest.mark.when_constraint
def test_entity_scoped_when_per_triple(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Triple interaction: entity-scoped variable + WHEN + PER.

    SUM(keepN * c.c_acctbal) <= 50000 WHEN c.c_acctbal > 5000 PER r.r_name
    means: for each region, the sum of keepN * acctbal over high-balance
    customers only must be <= 50000.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, r.r_name, c.c_acctbal, keepN
        FROM customer c
        JOIN nation n ON c.c_nationkey = n.n_nationkey
        JOIN region r ON n.n_regionkey = r.r_regionkey
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN * c_acctbal) <= 50000 WHEN c_acctbal > 5000 PER r_name
        MAXIMIZE SUM(keepN * c_acctbal)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)
    assert len(decidb_result) > 0

    nkey_idx = decidb_cols.index("n_nationkey")
    keep_idx = decidb_cols.index("keepN")
    region_idx = decidb_cols.index("r_name")
    acctbal_idx = decidb_cols.index("c_acctbal")

    # Verify entity consistency
    nation_values = {}
    for row in decidb_result:
        nkey = int(row[nkey_idx])
        keep = int(row[keep_idx])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN: {nation_values[nkey]} vs {keep}"
        else:
            nation_values[nkey] = keep

    # Verify WHEN+PER constraint: per region, sum over high-balance customers <= 50000
    region_sums = {}
    for row in decidb_result:
        acctbal = float(row[acctbal_idx])
        if acctbal > 5000:  # WHEN filter
            region = str(row[region_idx])
            keep = int(row[keep_idx])
            region_sums[region] = region_sums.get(region, 0.0) + keep * acctbal

    for region, total in region_sums.items():
        assert total <= 50000 + 1e-4, \
            f"WHEN+PER constraint violated: region '{region}' sum={total:.2f} > 50000"

    # Oracle: BINARY var per nation; constraint uses only high-balance rows (WHEN filter)
    data = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               CAST(r.r_name AS VARCHAR),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c
        JOIN nation n ON c.c_nationkey = n.n_nationkey
        JOIN region r ON n.n_regionkey = r.r_regionkey
    """).fetchall()

    nation_ids = sorted(set(int(row[0]) for row in data))
    nation_region = {}
    nation_acctbal_all = {}
    nation_acctbal_high = {}
    for row in data:
        nkey = int(row[0])
        rname = str(row[1])
        acctbal = float(row[2])
        nation_region[nkey] = rname
        nation_acctbal_all[nkey] = nation_acctbal_all.get(nkey, 0.0) + acctbal
        if acctbal > 5000:
            nation_acctbal_high[nkey] = nation_acctbal_high.get(nkey, 0.0) + acctbal

    oracle_solver.create_model("entity_scoped_when_per")
    vnames = {nkey: f"keepN_{nkey}" for nkey in nation_ids}
    for nkey in nation_ids:
        oracle_solver.add_variable(vnames[nkey], VarType.BINARY)
    # WHEN+PER: per region, sum of high-balance acctbal weighted by keepN <= 50000
    for r in set(nation_region.values()):
        coeff = {vnames[nkey]: nation_acctbal_high.get(nkey, 0.0)
                 for nkey in nation_ids if nation_region[nkey] == r}
        oracle_solver.add_constraint(coeff, "<=", 50000.0,
                                     name=f"when_per_{r.replace(' ', '_')}")
    oracle_solver.set_objective(
        {vnames[nkey]: nation_acctbal_all[nkey] for nkey in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()
    oracle_obj = oracle_result.objective_value

    decidb_obj = sum(
        nation_values.get(nkey, 0) * nation_acctbal_all.get(nkey, 0.0)
        for nkey in nation_ids
    )
    assert abs(decidb_obj - oracle_obj) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={oracle_obj:.4f}"


# ---------------------------------------------------------------------------
# Test 12: NE (<>) constraint with entity-scoped variable
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.cons_comparison
def test_entity_scoped_ne_constraint(decidb_cli, duckdb_conn, oracle_solver):
    """SUM(keepN) <> K with entity-scoped variable — oracle-compared.

    Nation table only (no join) so SUM(keepN) = number of nations selected.
    Oracle uses add_ne_indicator to encode SUM(keepN) != 2 via Gurobi
    indicator constraints (Big-M-free)."""
    sql = """
        SELECT n.n_nationkey, n.n_name, keepN
        FROM nation n
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) <> 2
          AND SUM(keepN) <= 4
        MAXIMIZE SUM(keepN)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    raw = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT)
        FROM nation n WHERE n.n_regionkey = 0
    """).fetchall()
    nation_ids = sorted(int(r[0]) for r in raw)

    oracle_solver.create_model("entity_scoped_ne_constraint")
    vnames = {nk: f"keepN_{nk}" for nk in nation_ids}
    for nk in nation_ids:
        oracle_solver.add_variable(vnames[nk], VarType.BINARY)
    sum_coeffs = {vnames[nk]: 1.0 for nk in nation_ids}
    add_ne_indicator(oracle_solver, sum_coeffs, 2.0, name="ne_2")
    oracle_solver.add_constraint(sum_coeffs, "<=", 4.0, name="sum_le_4")
    oracle_solver.set_objective(sum_coeffs, ObjSense.MAXIMIZE)
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    keepN_idx = cols.index("keepN")
    decidb_obj = sum(int(row[keepN_idx]) for row in result)
    assert abs(decidb_obj - res.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


# ---------------------------------------------------------------------------
# Test 14: MAX hard case (MAX >= K, Big-M) with entity-scoped variable
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.min_max
def test_entity_scoped_max_hard_case(decidb_cli, duckdb_conn, oracle_solver):
    """MAX(keepN * c.c_acctbal) >= K with entity-scoped variable (hard case).

    MAX >= K requires at least one selected entity to have a row with
    acctbal >= K. Tests Big-M indicator linearization with entity-scoped
    variable indexing (hard case: requires disjunctive constraint).
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT MAX(keepN * c.c_acctbal) >= 5000
          AND SUM(keepN) <= 50
        MAXIMIZE SUM(keepN)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    keepN_idx = cols.index("keepN")
    acctbal_idx = cols.index("c_acctbal")

    # Verify MAX constraint: at least one row has keepN=1 and acctbal >= 5000
    max_selected = max(
        (float(row[acctbal_idx]) for row in result if int(row[keepN_idx]) == 1),
        default=0.0,
    )
    assert max_selected >= 5000 - 1e-4, \
        f"MAX >= 5000 constraint violated: max selected acctbal = {max_selected:.2f}"

    # Oracle: MAX hard case → at least one nation with max_acctbal >= 5000 selected.
    # SUM(keepN) <= 50 counts join rows. Maximize SUM_n keepN_n * cnt_n.
    raw = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               MAX(c.c_acctbal) as max_acctbal,
               COUNT(*) as cnt
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        GROUP BY n.n_nationkey
    """).fetchall()
    nation_max14 = {int(r[0]): float(r[1]) for r in raw}
    nation_cnt14 = {int(r[0]): int(r[2]) for r in raw}
    nation_ids14 = sorted(nation_max14.keys())

    oracle_solver.create_model("entity_scoped_max_hard")
    vnames14 = {nkey: f"keepN_{nkey}" for nkey in nation_ids14}
    for nkey in nation_ids14:
        oracle_solver.add_variable(vnames14[nkey], VarType.BINARY)
    # SUM(keepN) <= 50 in join rows
    oracle_solver.add_constraint(
        {vnames14[nkey]: float(nation_cnt14[nkey]) for nkey in nation_ids14},
        "<=", 50.0, name="sum_limit",
    )
    # MAX >= 5000: at least one qualifying nation must be selected
    qualifying = [nkey for nkey in nation_ids14 if nation_max14[nkey] >= 5000]
    if qualifying:
        oracle_solver.add_constraint(
            {vnames14[nkey]: 1.0 for nkey in qualifying},
            ">=", 1.0, name="max_hard_lb",
        )
    oracle_solver.set_objective(
        {vnames14[nkey]: float(nation_cnt14[nkey]) for nkey in nation_ids14},
        ObjSense.MAXIMIZE,
    )
    oracle_result14 = oracle_solver.solve()
    decidb_obj14 = float(sum(int(row[keepN_idx]) for row in result))
    assert abs(decidb_obj14 - oracle_result14.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj14:.4f}, Oracle={oracle_result14.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test 15: Mixed row-scoped + entity-scoped + WHEN + PER (all-four interaction)
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.per_clause
@pytest.mark.when_constraint
def test_entity_scoped_mixed_when_per(
    decidb_cli, duckdb_conn, oracle_solver,
):
    """All-four interaction: entity-scoped keepN + row-scoped x + WHEN + PER
    — oracle-compared. Customer-level MIP (~1500 binaries) is trivial for
    Gurobi; the prior HiGHS-based oracle skip no longer applies.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, r.r_name, c.c_acctbal, x, keepN
        FROM customer c
        JOIN nation n ON c.c_nationkey = n.n_nationkey
        JOIN region r ON n.n_regionkey = r.r_regionkey
        DECIDE n.keepN(BOOL), x(BOOL)
        SUCH THAT x <= keepN
          AND SUM(x * c.c_acctbal) <= 15000 WHEN c.c_acctbal > 0 PER r_name
        MAXIMIZE SUM(x * c.c_acctbal)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    keepN_idx = cols.index("keepN")
    x_idx = cols.index("x")
    region_idx = cols.index("r_name")
    acctbal_idx = cols.index("c_acctbal")
    nkey_idx = cols.index("n_nationkey")
    ckey_idx = cols.index("c_custkey")

    # Oracle mirrors the query exactly: one binary x_i per customer row, one
    # binary keepN_n per nation, linked by x_i <= keepN_{n(i)}. PER grouping
    # partitions WHEN-filtered rows by region; the objective sums over all
    # rows (the optimizer will naturally set x_i=0 for negative acctbal).
    data = duckdb_conn.execute("""
        SELECT CAST(c.c_custkey AS BIGINT),
               CAST(n.n_nationkey AS BIGINT),
               CAST(r.r_name AS VARCHAR),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c
        JOIN nation n ON c.c_nationkey = n.n_nationkey
        JOIN region r ON n.n_regionkey = r.r_regionkey
    """).fetchall()
    nation_ids = sorted({int(row[1]) for row in data})

    oracle_solver.create_model("entity_scoped_mixed_when_per")
    knames = {nk: f"keepN_{nk}" for nk in nation_ids}
    for nk in nation_ids:
        oracle_solver.add_variable(knames[nk], VarType.BINARY)
    xnames = [f"x_{i}" for i in range(len(data))]
    for xn in xnames:
        oracle_solver.add_variable(xn, VarType.BINARY)

    for i, row in enumerate(data):
        nk = int(row[1])
        oracle_solver.add_constraint(
            {xnames[i]: 1.0, knames[nk]: -1.0}, "<=", 0.0,
            name=f"link_{i}",
        )

    # WHEN→PER: partition rows with acctbal > 0 by region.
    region_rows: dict[str, list[int]] = {}
    for i, row in enumerate(data):
        if float(row[3]) > 0:
            region_rows.setdefault(str(row[2]), []).append(i)
    for rname, idxs in region_rows.items():
        oracle_solver.add_constraint(
            {xnames[i]: float(data[i][3]) for i in idxs},
            "<=", 15000.0, name=f"per_{rname.replace(' ', '_')[:8]}",
        )

    oracle_solver.set_objective(
        {xnames[i]: float(data[i][3]) for i in range(len(data))},
        ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve(time_limit=120.0)
    assert res.status == SolverStatus.OPTIMAL

    decidb_obj = sum(
        int(row[x_idx]) * float(row[acctbal_idx]) for row in result
    )
    # Both solvers stop at Gurobi's default MIPGap (1e-4) from the true
    # optimum, so the two incumbents can differ by up to ~2*MIPGap of obj.
    tol = max(1e-3, 2e-4 * abs(res.objective_value))
    assert abs(decidb_obj - res.objective_value) <= tol, (
        f"Objective mismatch: DecidB={decidb_obj:.4f}, "
        f"Oracle={res.objective_value:.4f}, tol={tol:.4f}"
    )

    # Invariants: entity consistency + per-row x<=keepN + WHEN+PER group cap.
    nation_values = {}
    for row in result:
        nkey = int(row[nkey_idx])
        keep = int(row[keepN_idx])
        if nkey in nation_values:
            assert nation_values[nkey] == keep
        else:
            nation_values[nkey] = keep
    for row in result:
        nkey = int(row[nkey_idx])
        assert int(row[x_idx]) <= nation_values[nkey]
    region_sums: dict[str, float] = {}
    for row in result:
        ab = float(row[acctbal_idx])
        if ab > 0:
            region = str(row[region_idx])
            region_sums[region] = (
                region_sums.get(region, 0.0) + int(row[x_idx]) * ab
            )
    for region, total in region_sums.items():
        assert total <= 15000 + 1e-4, \
            f"WHEN+PER violated for region '{region}': {total:.2f} > 15000"


# ---------------------------------------------------------------------------
# Test 16: WHEN modifier on objective with entity-scoped variable
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.when_objective
def test_entity_scoped_when_on_objective(decidb_cli, duckdb_conn, oracle_solver):
    """WHEN modifier on objective with entity-scoped variable.

    MAXIMIZE SUM(keepN * c.c_acctbal) WHEN c.c_acctbal > 0 tests that WHEN
    correctly filters rows from objective coefficient assembly with entity-scoped
    variables (only positive-acctbal rows contribute to the maximized value).
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) <= 10
        MAXIMIZE SUM(keepN * c.c_acctbal) WHEN c.c_acctbal > 0
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    keepN_idx = cols.index("keepN")
    nkey_idx = cols.index("n_nationkey")
    acctbal_idx = cols.index("c_acctbal")

    # Verify entity consistency
    nation_values = {}
    for row in result:
        nkey = int(row[nkey_idx])
        keep = int(row[keepN_idx])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN"
        else:
            nation_values[nkey] = keep

    # SUM(keepN) <= 10 (counts join rows)
    total_rows = sum(int(row[keepN_idx]) for row in result)
    assert total_rows <= 10, f"SUM(keepN) <= 10 violated: {total_rows}"

    # Oracle: WHEN filters objective; constraint counts all rows
    raw = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT), CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
    """).fetchall()
    nation_ids = sorted(set(int(r[0]) for r in raw))
    nation_count = {}; nation_acctbal_pos = {}
    for r in raw:
        nkey = int(r[0]); acctbal = float(r[1])
        nation_count[nkey] = nation_count.get(nkey, 0) + 1
        if acctbal > 0:
            nation_acctbal_pos[nkey] = nation_acctbal_pos.get(nkey, 0.0) + acctbal

    oracle_solver.create_model("entity_scoped_when_obj")
    vnames = {nkey: f"keepN_{nkey}" for nkey in nation_ids}
    for nkey in nation_ids:
        oracle_solver.add_variable(vnames[nkey], VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[nkey]: float(nation_count[nkey]) for nkey in nation_ids},
        "<=", 10.0, name="nation_limit",
    )
    oracle_solver.set_objective(
        {vnames[nkey]: nation_acctbal_pos.get(nkey, 0.0) for nkey in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()
    decidb_obj = sum(
        nation_values.get(nkey, 0) * nation_acctbal_pos.get(nkey, 0.0)
        for nkey in nation_ids
    )
    assert abs(decidb_obj - oracle_result.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={oracle_result.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test 17: Multi-column PER with entity-scoped variable
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.per_clause
def test_entity_scoped_multi_column_per(decidb_cli, duckdb_conn, oracle_solver):
    """Entity-scoped variable with multi-column PER (region × market segment).

    PER (n.n_regionkey, c.c_mktsegment) creates one constraint per
    (region, market segment) pair. Tests multi-column group key with
    entity-scoped variable indexing.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, n.n_regionkey, c.c_mktsegment, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey < 3
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) <= 30 PER (n_regionkey, c_mktsegment)
        MAXIMIZE SUM(keepN * c.c_acctbal)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    keepN_idx = cols.index("keepN")
    nkey_idx = cols.index("n_nationkey")
    rkey_idx = cols.index("n_regionkey")
    seg_idx = cols.index("c_mktsegment")

    # Verify entity consistency
    nation_values = {}
    for row in result:
        nkey = int(row[nkey_idx])
        keep = int(row[keepN_idx])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN"
        else:
            nation_values[nkey] = keep

    # Verify PER constraint: per (region, segment), SUM(keepN) <= 30
    group_sums = {}
    for row in result:
        key = (int(row[rkey_idx]), str(row[seg_idx]))
        group_sums[key] = group_sums.get(key, 0) + int(row[keepN_idx])
    for key, total in group_sums.items():
        assert total <= 30, \
            f"Multi-column PER violated for group {key}: SUM={total} > 30"

    # Oracle: per (region, segment) group, SUM_n keepN_n * cnt_{n,seg} <= 30
    raw = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               CAST(n.n_regionkey AS BIGINT),
               CAST(c.c_mktsegment AS VARCHAR),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey < 3
    """).fetchall()
    nation_ids = sorted(set(int(r[0]) for r in raw))
    nation_region = {}; nation_acctbal = {}; nation_seg_count = {}
    for r in raw:
        nkey = int(r[0]); rkey = int(r[1]); seg = str(r[2]); acctbal = float(r[3])
        nation_region[nkey] = rkey
        nation_acctbal[nkey] = nation_acctbal.get(nkey, 0.0) + acctbal
        nation_seg_count[(nkey, seg)] = nation_seg_count.get((nkey, seg), 0) + 1

    groups = set((nation_region[nkey], seg) for (nkey, seg) in nation_seg_count)
    oracle_solver.create_model("entity_scoped_multi_col_per")
    vnames = {nkey: f"keepN_{nkey}" for nkey in nation_ids}
    for nkey in nation_ids:
        oracle_solver.add_variable(vnames[nkey], VarType.BINARY)
    for (rkey, seg) in groups:
        oracle_solver.add_constraint(
            {vnames[nkey]: float(nation_seg_count.get((nkey, seg), 0))
             for nkey in nation_ids if nation_region.get(nkey) == rkey},
            "<=", 30.0, name=f"per_{rkey}_{seg[:4]}",
        )
    oracle_solver.set_objective(
        {vnames[nkey]: nation_acctbal[nkey] for nkey in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()
    decidb_obj = sum(
        nation_values.get(nkey, 0) * nation_acctbal.get(nkey, 0.0)
        for nkey in nation_ids
    )
    assert abs(decidb_obj - oracle_result.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj:.4f}, Oracle={oracle_result.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test 18 (bonus): MIN easy case (MIN >= K) with entity-scoped variable
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.min_max
def test_entity_scoped_min_easy_case(decidb_cli, duckdb_conn, oracle_solver):
    """MIN(keepN * c.c_acctbal) >= K with entity-scoped variable (easy case).

    Easy case: MIN >= K rewrites to per-row keepN * acctbal >= K, which
    strips the MIN aggregate. Tests MIN/MAX linearization easy-case path
    with entity-scoped variable indexing.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT MIN(keepN * c.c_acctbal) >= 0
          AND SUM(keepN) <= 20
        MAXIMIZE SUM(keepN)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    keepN_idx = cols.index("keepN")
    acctbal_idx = cols.index("c_acctbal")
    nkey_idx = cols.index("n_nationkey")

    # Verify entity consistency
    nation_values = {}
    for row in result:
        nkey = int(row[nkey_idx])
        keep = int(row[keepN_idx])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN"
        else:
            nation_values[nkey] = keep

    # Verify MIN constraint: all rows with keepN=1 have acctbal >= 0
    for row in result:
        if int(row[keepN_idx]) == 1:
            assert float(row[acctbal_idx]) >= -1e-4, \
                f"MIN >= 0 violated: keepN=1 but acctbal={row[acctbal_idx]}"

    # Oracle: MIN easy case → nations where any customer has acctbal < 0 are blocked.
    # Maximize SUM_n keepN_n * cnt_n subject to SUM_n keepN_n * cnt_n <= 20.
    raw19 = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT),
               MIN(c.c_acctbal) as min_acctbal,
               COUNT(*) as cnt
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        GROUP BY n.n_nationkey
    """).fetchall()
    nation_min19 = {int(r[0]): float(r[1]) for r in raw19}
    nation_cnt19 = {int(r[0]): int(r[2]) for r in raw19}
    nation_ids19 = sorted(nation_min19.keys())

    oracle_solver.create_model("entity_scoped_min_easy")
    vnames19 = {nkey: f"keepN_{nkey}" for nkey in nation_ids19}
    for nkey in nation_ids19:
        ub19 = 0.0 if nation_min19[nkey] < 0 else 1.0
        oracle_solver.add_variable(vnames19[nkey], VarType.BINARY, ub=ub19)
    oracle_solver.add_constraint(
        {vnames19[nkey]: float(nation_cnt19[nkey]) for nkey in nation_ids19},
        "<=", 20.0, name="sum_limit",
    )
    oracle_solver.set_objective(
        {vnames19[nkey]: float(nation_cnt19[nkey]) for nkey in nation_ids19},
        ObjSense.MAXIMIZE,
    )
    oracle_result19 = oracle_solver.solve()
    decidb_obj19 = float(sum(int(row[keepN_idx]) for row in result))
    assert abs(decidb_obj19 - oracle_result19.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj19:.4f}, Oracle={oracle_result19.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test 19: AVG + PER with entity-scoped variable
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.per_clause
@pytest.mark.avg_rewrite
def test_entity_scoped_avg_per(decidb_cli, duckdb_conn, oracle_solver):
    """AVG constraint + PER grouping with entity-scoped variable.

    AVG(keepN * c.c_acctbal) <= K PER r.r_name tests that AVG→SUM scaling
    with entity-scoped variables and PER group sizes interacts correctly.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, r.r_name, keepN
        FROM customer c
        JOIN nation n ON c.c_nationkey = n.n_nationkey
        JOIN region r ON n.n_regionkey = r.r_regionkey
        DECIDE n.keepN(BOOL)
        SUCH THAT AVG(keepN * c.c_acctbal) <= 2000 PER r_name
        MAXIMIZE SUM(keepN)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    keepN_idx = cols.index("keepN")
    nkey_idx = cols.index("n_nationkey")

    # Verify entity consistency
    nation_values = {}
    for row in result:
        nkey = int(row[nkey_idx])
        keep = int(row[keepN_idx])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN"
        else:
            nation_values[nkey] = keep

    # Oracle: AVG PER r_name → per region: SUM_n keepN_n * acctbal_sum_n <= 2000 * n_rows_in_region
    raw20 = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT), CAST(r.r_name AS VARCHAR),
               SUM(c.c_acctbal) as acctbal_sum, COUNT(*) as cnt
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        JOIN region r ON n.n_regionkey = r.r_regionkey
        GROUP BY n.n_nationkey, r.r_name
    """).fetchall()
    nation_ids20 = sorted(set(int(r[0]) for r in raw20))
    nation_region20 = {int(r[0]): str(r[1]) for r in raw20}
    nation_acctbal20 = {int(r[0]): float(r[2]) for r in raw20}
    nation_cnt20 = {int(r[0]): int(r[3]) for r in raw20}
    region_row_count20 = {}
    for r in raw20:
        rname = str(r[1])
        region_row_count20[rname] = region_row_count20.get(rname, 0) + int(r[3])

    oracle_solver.create_model("entity_scoped_avg_per")
    vnames20 = {nkey: f"keepN_{nkey}" for nkey in nation_ids20}
    for nkey in nation_ids20:
        oracle_solver.add_variable(vnames20[nkey], VarType.BINARY)
    for rname in set(nation_region20.values()):
        coeff20 = {vnames20[nkey]: nation_acctbal20[nkey]
                   for nkey in nation_ids20 if nation_region20[nkey] == rname}
        oracle_solver.add_constraint(
            coeff20, "<=", 2000.0 * region_row_count20[rname],
            name=f"avg_per_{rname.replace(' ', '_')[:8]}",
        )
    oracle_solver.set_objective(
        {vnames20[nkey]: float(nation_cnt20[nkey]) for nkey in nation_ids20},
        ObjSense.MAXIMIZE,
    )
    oracle_result20 = oracle_solver.solve()
    decidb_obj20 = float(sum(int(row[keepN_idx]) for row in result))
    assert abs(decidb_obj20 - oracle_result20.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj20:.4f}, Oracle={oracle_result20.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test 20: NE + PER with entity-scoped variable
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.per_clause
@pytest.mark.cons_comparison
def test_entity_scoped_ne_per(decidb_cli, duckdb_conn, oracle_solver):
    """NE constraint + PER grouping with entity-scoped variable — oracle-compared.

    Per region, number of selected nations must not equal 2. Uses nation
    table directly (no join) so SUM per region = count of selected nations
    in that region. Oracle emits one NE indicator per region."""
    sql = """
        SELECT n.n_nationkey, n.n_name, n.n_regionkey, keepN
        FROM nation n
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) <> 2 PER n_regionkey
          AND SUM(keepN) <= 20
        MAXIMIZE SUM(keepN)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    raw = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT), CAST(n.n_regionkey AS BIGINT)
        FROM nation n
    """).fetchall()
    nation_ids = sorted(int(r[0]) for r in raw)
    nation_region = {int(r[0]): int(r[1]) for r in raw}
    regions = sorted(set(nation_region.values()))

    oracle_solver.create_model("entity_scoped_ne_per")
    vnames = {nk: f"keepN_{nk}" for nk in nation_ids}
    for nk in nation_ids:
        oracle_solver.add_variable(vnames[nk], VarType.BINARY)
    for rk in regions:
        region_coeffs = {
            vnames[nk]: 1.0 for nk in nation_ids if nation_region[nk] == rk
        }
        add_ne_indicator(oracle_solver, region_coeffs, 2.0, name=f"ne_r{rk}")
    oracle_solver.add_constraint(
        {vnames[nk]: 1.0 for nk in nation_ids}, "<=", 20.0, name="sum_le",
    )
    oracle_solver.set_objective(
        {vnames[nk]: 1.0 for nk in nation_ids}, ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    keepN_idx = cols.index("keepN")
    decidb_obj = sum(int(row[keepN_idx]) for row in result)
    assert abs(decidb_obj - res.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


# ---------------------------------------------------------------------------
# Test 21: BETWEEN constraint with entity-scoped variable
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.cons_between
def test_entity_scoped_between_constraint(decidb_cli, duckdb_conn, oracle_solver):
    """BETWEEN constraint with entity-scoped variable — oracle-compared.

    Oracle expands BETWEEN 2 AND 4 to two linear constraints."""
    sql = """
        SELECT n.n_nationkey, n.n_name, keepN
        FROM nation n
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) BETWEEN 2 AND 4
        MAXIMIZE SUM(keepN)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    raw = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT)
        FROM nation n WHERE n.n_regionkey = 0
    """).fetchall()
    nation_ids = sorted(int(r[0]) for r in raw)

    oracle_solver.create_model("entity_scoped_between")
    vnames = {nk: f"keepN_{nk}" for nk in nation_ids}
    for nk in nation_ids:
        oracle_solver.add_variable(vnames[nk], VarType.BINARY)
    sum_coeffs = {vnames[nk]: 1.0 for nk in nation_ids}
    oracle_solver.add_constraint(sum_coeffs, ">=", 2.0, name="between_lo")
    oracle_solver.add_constraint(sum_coeffs, "<=", 4.0, name="between_hi")
    oracle_solver.set_objective(sum_coeffs, ObjSense.MAXIMIZE)
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    keepN_idx = cols.index("keepN")
    decidb_obj = sum(int(row[keepN_idx]) for row in result)
    assert abs(decidb_obj - res.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


# ---------------------------------------------------------------------------
# Test 22: Two entity-scoped variables from different tables
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_two_tables(decidb_cli, duckdb_conn, oracle_solver):
    """Two entity-scoped variables from different source tables.

    keepN per nation, keepR per region. Constraint keepN <= keepR means
    a nation can be selected only if its region is selected. Tests the
    multi-table VarIndexer layout where two entity-scoped blocks coexist.
    SUM(keepR) <= 10 limits to at most 2 regions (5 nations per region).
    """
    sql = """
        SELECT n.n_nationkey, r.r_regionkey, r.r_name, keepN, keepR
        FROM nation n JOIN region r ON n.n_regionkey = r.r_regionkey
        DECIDE n.keepN(BOOL), r.keepR(BOOL)
        SUCH THAT keepN <= keepR
          AND SUM(keepR) <= 10
        MAXIMIZE SUM(keepN)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    keepN_idx = cols.index("keepN")
    keepR_idx = cols.index("keepR")
    nkey_idx = cols.index("n_nationkey")
    rkey_idx = cols.index("r_regionkey")

    # Verify entity consistency for keepN (per nation)
    nation_values = {}
    for row in result:
        nkey = int(row[nkey_idx])
        keep = int(row[keepN_idx])
        if nkey in nation_values:
            assert nation_values[nkey] == keep, \
                f"Nation {nkey} has inconsistent keepN"
        else:
            nation_values[nkey] = keep

    # Verify entity consistency for keepR (per region)
    region_values = {}
    for row in result:
        rkey = int(row[rkey_idx])
        keep = int(row[keepR_idx])
        if rkey in region_values:
            assert region_values[rkey] == keep, \
                f"Region {rkey} has inconsistent keepR"
        else:
            region_values[rkey] = keep

    # Verify per-row: keepN <= keepR
    for row in result:
        assert int(row[keepN_idx]) <= int(row[keepR_idx]), \
            f"keepN <= keepR violated for nation {row[nkey_idx]}"

    # Verify SUM(keepR) <= 10 (counts join rows; 5 nations per region → at most 2 regions)
    total_keepR_rows = sum(int(row[keepR_idx]) for row in result)
    assert total_keepR_rows <= 10, \
        f"SUM(keepR) <= 10 violated: {total_keepR_rows}"

    # Oracle: binary per nation + binary per region; keepN_n <= keepR_{r(n)};
    # SUM_r keepR_r * 5 <= 10 → at most 2 regions; maximize SUM_n keepN_n.
    raw22 = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT), CAST(n.n_regionkey AS BIGINT)
        FROM nation n JOIN region r ON n.n_regionkey = r.r_regionkey
    """).fetchall()
    nation_ids22 = sorted(set(int(r[0]) for r in raw22))
    nation_to_region22 = {int(r[0]): int(r[1]) for r in raw22}
    region_ids22 = sorted(set(nation_to_region22.values()))
    nations_per_region22 = {}
    for nkey in nation_ids22:
        rkey = nation_to_region22[nkey]
        nations_per_region22[rkey] = nations_per_region22.get(rkey, 0) + 1

    oracle_solver.create_model("entity_scoped_two_tables")
    kn22 = {nkey: f"keepN_{nkey}" for nkey in nation_ids22}
    kr22 = {rkey: f"keepR_{rkey}" for rkey in region_ids22}
    for nkey in nation_ids22:
        oracle_solver.add_variable(kn22[nkey], VarType.BINARY)
    for rkey in region_ids22:
        oracle_solver.add_variable(kr22[rkey], VarType.BINARY)
    # keepN_n <= keepR_{r(n)}
    for nkey in nation_ids22:
        rkey = nation_to_region22[nkey]
        oracle_solver.add_constraint(
            {kn22[nkey]: 1.0, kr22[rkey]: -1.0}, "<=", 0.0,
            name=f"link_n{nkey}",
        )
    # SUM(keepR) <= 10 in join rows: SUM_r keepR_r * nations_per_region_r <= 10
    oracle_solver.add_constraint(
        {kr22[rkey]: float(nations_per_region22[rkey]) for rkey in region_ids22},
        "<=", 10.0, name="sum_keepR",
    )
    oracle_solver.set_objective(
        {kn22[nkey]: 1.0 for nkey in nation_ids22},
        ObjSense.MAXIMIZE,
    )
    oracle_result22 = oracle_solver.solve()
    decidb_obj22 = float(sum(int(row[keepN_idx]) for row in result))
    assert abs(decidb_obj22 - oracle_result22.objective_value) < 1e-4, \
        f"Objective mismatch: DecidB={decidb_obj22:.4f}, Oracle={oracle_result22.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test 23: Error — entity-scoped variable referenced in WHEN condition
# ---------------------------------------------------------------------------

@pytest.mark.error_binder
def test_entity_scoped_var_in_when_condition_error(decidb_cli):
    """WHEN condition must not reference DECIDE variables (including entity-scoped)."""
    with pytest.raises(DecidBCliError,
                       match="WHEN conditions cannot reference DECIDE variables"):
        decidb_cli.execute("""
            SELECT n.n_nationkey, keepN
            FROM nation n
            DECIDE n.keepN(BOOL)
            SUCH THAT SUM(keepN) <= 5 WHEN keepN = 1
            MAXIMIZE SUM(keepN)
        """)


# ---------------------------------------------------------------------------
# Test 24: WHEN filters out all rows for some entities
# ---------------------------------------------------------------------------

@pytest.mark.error_infeasible
@pytest.mark.when_constraint
def test_entity_scoped_when_entity_invisible(decidb_cli):
    """Entity-scoped aggregate constraint with WHEN matching no rows on the
    given data — now rejected pre-solver per the "reject all empty aggregate
    sets" rule."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN * c.c_acctbal) <= 50000 WHEN c.c_acctbal > 9998
          AND SUM(keepN) <= 10
        MAXIMIZE SUM(keepN)
    """
    decidb_cli.assert_error(sql, match=r"empty|WHEN")


# ---------------------------------------------------------------------------
# Test 25: Equality (=) constraint with entity-scoped variable
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_equality_constraint(
    decidb_cli, duckdb_conn, oracle_solver,
):
    """SUM(keepN) = K with entity-scoped variable — oracle-compared."""
    sql = """
        SELECT n.n_nationkey, n.n_name, keepN
        FROM nation n
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) = 3
        MAXIMIZE SUM(keepN)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    raw = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT)
        FROM nation n WHERE n.n_regionkey = 0
    """).fetchall()
    nation_ids = sorted(int(r[0]) for r in raw)

    oracle_solver.create_model("entity_scoped_equality")
    vnames = {nk: f"keepN_{nk}" for nk in nation_ids}
    for nk in nation_ids:
        oracle_solver.add_variable(vnames[nk], VarType.BINARY)
    sum_coeffs = {vnames[nk]: 1.0 for nk in nation_ids}
    oracle_solver.add_constraint(sum_coeffs, "=", 3.0, name="eq")
    oracle_solver.set_objective(sum_coeffs, ObjSense.MAXIMIZE)
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    keepN_idx = cols.index("keepN")
    decidb_obj = sum(int(row[keepN_idx]) for row in result)
    assert abs(decidb_obj - res.objective_value) <= 1e-6, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


# ---------------------------------------------------------------------------
# Cross-feature interactions: entity-scoped with rewrite-heavy features.
# Written 2026-04-15 to close gaps in entity_scope/todo.md. All oracle-compared.
#
# While adding these, we found and fixed a silent-correctness bug:
# entity-key columns that didn't also appear in SELECT/WHERE/constraints/
# objective were pruned from the scan by the binder. With only a partial
# key surviving, distinct entities (nations) silently collapsed into
# whatever grouping the surviving column produced (e.g., region groups).
# Fix: register entity-key columns via TableBinding::GetColumnBinding at
# bind time, store BoundColumnRefExpressions on LogicalDecide, and
# visit them in RemoveUnusedColumns + refresh bindings in plan_decide.
# ---------------------------------------------------------------------------


@pytest.mark.correctness
@pytest.mark.var_real
def test_entity_scoped_is_real(decidb_cli, duckdb_conn, oracle_solver):
    """REAL entity-scoped variable — DOUBLE readback via VarIndexer.

    Single-table entity-scoped REAL query. Exercises the no-JOIN path where
    the binder's initial column pruning could silently drop the entity-key
    identity column.
    """
    sql = """
        SELECT n_nationkey, ROUND(budget, 2) AS budget
        FROM nation WHERE n_regionkey <= 2
        DECIDE nation.budget(REAL)
        SUCH THAT budget <= 1000 AND SUM(budget) <= 5000
        MAXIMIZE SUM(budget * n_nationkey)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) == 15

    nkey_idx = cols.index("n_nationkey")
    budget_idx = cols.index("budget")

    # Sanity: per-row constraint budget<=1000 and total<=5000
    budgets = [float(row[budget_idx]) for row in result]
    for b in budgets:
        assert 0.0 <= b <= 1000.0 + 1e-6
    assert sum(budgets) <= 5000.0 + 1e-4

    # Oracle: one continuous var per nation in region<=2, ub=1000, SUM<=5000
    data = duckdb_conn.execute("""
        SELECT CAST(n_nationkey AS BIGINT)
        FROM nation WHERE n_regionkey <= 2
        ORDER BY n_nationkey
    """).fetchall()
    nation_ids = [int(r[0]) for r in data]

    oracle_solver.create_model("entity_scoped_is_real")
    vnames = {n: f"budget_{n}" for n in nation_ids}
    for n in nation_ids:
        oracle_solver.add_variable(vnames[n], VarType.CONTINUOUS, lb=0.0, ub=1000.0)
    oracle_solver.add_constraint(
        {vnames[n]: 1.0 for n in nation_ids},
        "<=", 5000.0, name="sum_budget",
    )
    oracle_solver.set_objective(
        {vnames[n]: float(n) for n in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()

    decidb_obj = sum(float(row[nkey_idx]) * float(row[budget_idx]) for row in result)
    assert abs(decidb_obj - oracle_result.objective_value) < 1e-3, \
        f"DecidB={decidb_obj:.4f}, Oracle={oracle_result.objective_value:.4f}"


@pytest.mark.correctness
@pytest.mark.min_max
def test_entity_scoped_hard_min_max(decidb_cli, duckdb_conn, oracle_solver):
    """MIN(qty) <= K hard case with entity-scoped INTEGER.

    Hard MIN case: at least one entity must have qty<=K. The Big-M indicator
    per entity (not per row) must index off the entity key.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, qty
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 100
        DECIDE n.qty(INT)
        SUCH THAT qty <= 10 AND MIN(qty) <= 3 AND SUM(qty) >= 60
        MAXIMIZE SUM(qty)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    nkey_idx = cols.index("n_nationkey")
    qty_idx = cols.index("qty")

    # Per-nation consistency check
    per_nation = {}
    for row in result:
        k = int(row[nkey_idx])
        q = int(row[qty_idx])
        if k in per_nation:
            assert per_nation[k] == q, f"Nation {k} inconsistent qty"
        else:
            per_nation[k] = q

    # MIN<=3 means at least one nation has qty<=3
    assert any(q <= 3 for q in per_nation.values()), \
        f"MIN(qty)<=3 violated: per-nation qty = {per_nation}"

    # Oracle: per-nation INTEGER var [0, 10], SUM over join rows >= 60,
    # at least one nation with qty<=3 (Big-M indicator y_n: qty_n <= 3 + M*(1-y_n), SUM(y_n)>=1).
    data = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT), COUNT(*) AS cnt
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 100
        GROUP BY n.n_nationkey
    """).fetchall()
    nation_cnt = {int(r[0]): int(r[1]) for r in data}
    nation_ids = sorted(nation_cnt.keys())

    oracle_solver.create_model("entity_scoped_hard_min")
    qvars = {n: f"qty_{n}" for n in nation_ids}
    yvars = {n: f"y_{n}" for n in nation_ids}
    BIG_M = 20.0  # qty max 10; slack must exceed 10-3 = 7
    for n in nation_ids:
        oracle_solver.add_variable(qvars[n], VarType.INTEGER, lb=0.0, ub=10.0)
        oracle_solver.add_variable(yvars[n], VarType.BINARY)
    # SUM over join rows: each nation contributes qty_n * cnt_n
    oracle_solver.add_constraint(
        {qvars[n]: float(nation_cnt[n]) for n in nation_ids},
        ">=", 60.0, name="sum_min",
    )
    # Big-M: qty_n - M*(1-y_n) <= 3  ⇒  qty_n + M*y_n <= 3 + M
    for n in nation_ids:
        oracle_solver.add_constraint(
            {qvars[n]: 1.0, yvars[n]: BIG_M},
            "<=", 3.0 + BIG_M, name=f"bigm_min_{n}",
        )
    # at least one indicator on
    oracle_solver.add_constraint(
        {yvars[n]: 1.0 for n in nation_ids},
        ">=", 1.0, name="one_indicator",
    )
    oracle_solver.set_objective(
        {qvars[n]: float(nation_cnt[n]) for n in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()

    decidb_obj = sum(int(row[qty_idx]) for row in result)
    assert abs(decidb_obj - oracle_result.objective_value) < 1e-4, \
        f"DecidB={decidb_obj:.4f}, Oracle={oracle_result.objective_value:.4f}"


@pytest.mark.correctness
def test_entity_scoped_abs(decidb_cli, duckdb_conn, oracle_solver):
    """ABS linearization with entity-scoped coefficient aggregation.

    SUM(ABS(c_acctbal * keepN - 3000)) <= K. ABS rewrite creates per-row
    auxiliary variables; each row's coefficient references an entity-scoped
    keepN. The aux-to-entity indexing must be consistent.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 80
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(ABS(c_acctbal * keepN - 3000)) <= 50000
        MAXIMIZE SUM(keepN)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    nkey_idx = cols.index("n_nationkey")
    keepN_idx = cols.index("keepN")
    acctbal_idx = cols.index("c_acctbal")

    per_nation = {}
    for row in result:
        k = int(row[nkey_idx])
        v = int(row[keepN_idx])
        if k in per_nation:
            assert per_nation[k] == v
        else:
            per_nation[k] = v

    # Verify ABS constraint
    abs_sum = sum(abs(float(row[acctbal_idx]) * int(row[keepN_idx]) - 3000.0) for row in result)
    assert abs_sum <= 50000.0 + 1e-3

    # Oracle: one keepN_n per nation; per-row aux_i >= |acctbal_i*keepN_{n(i)} - 3000|
    data = duckdb_conn.execute("""
        SELECT CAST(c.c_custkey AS BIGINT), CAST(n.n_nationkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 80
        ORDER BY c.c_custkey
    """).fetchall()
    nation_ids = sorted(set(int(r[1]) for r in data))

    oracle_solver.create_model("entity_scoped_abs")
    keep_v = {n: f"keepN_{n}" for n in nation_ids}
    for n in nation_ids:
        oracle_solver.add_variable(keep_v[n], VarType.BINARY)

    aux_vars = []
    for idx, (_, nkey, acctbal) in enumerate(data):
        aux = f"aux_{idx}"
        aux_vars.append(aux)
        oracle_solver.add_variable(aux, VarType.CONTINUOUS, lb=0.0, ub=100000.0)
        nkey = int(nkey)
        # aux >= acctbal*keepN - 3000
        oracle_solver.add_constraint(
            {aux: 1.0, keep_v[nkey]: -float(acctbal)},
            ">=", -3000.0, name=f"abs_pos_{idx}",
        )
        # aux >= -(acctbal*keepN - 3000) = 3000 - acctbal*keepN
        oracle_solver.add_constraint(
            {aux: 1.0, keep_v[nkey]: float(acctbal)},
            ">=", 3000.0, name=f"abs_neg_{idx}",
        )
    oracle_solver.add_constraint(
        {a: 1.0 for a in aux_vars},
        "<=", 50000.0, name="abs_sum",
    )
    # Objective SUM(keepN) over join rows
    nation_cnt = {}
    for row in data:
        nation_cnt[int(row[1])] = nation_cnt.get(int(row[1]), 0) + 1
    oracle_solver.set_objective(
        {keep_v[n]: float(nation_cnt[n]) for n in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()

    decidb_obj = sum(int(row[keepN_idx]) for row in result)
    assert abs(decidb_obj - oracle_result.objective_value) < 1e-4, \
        f"DecidB={decidb_obj:.4f}, Oracle={oracle_result.objective_value:.4f}"


@pytest.mark.correctness
@pytest.mark.min_max
@pytest.mark.when_constraint
def test_entity_scoped_when_min_max_triple(decidb_cli, duckdb_conn, oracle_solver):
    """Entity-scoped + WHEN + hard MAX: all three features interacting.

    MAX(c_acctbal*keepN) >= 5000 WHEN c_acctbal > 2000. WHEN filters rows
    BEFORE the MAX aggregate; the Big-M indicator selection must respect
    both the WHEN mask and the entity dedup.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, c.c_acctbal, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 80
        DECIDE n.keepN(BOOL)
        SUCH THAT MAX(c_acctbal * keepN) >= 5000 WHEN c_acctbal > 2000
        MAXIMIZE SUM(keepN)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) > 0

    nkey_idx = cols.index("n_nationkey")
    keepN_idx = cols.index("keepN")
    acctbal_idx = cols.index("c_acctbal")

    # Sanity: at least one row with acctbal>2000, keepN=1, acctbal>=5000 (Big-M satisfied)
    qualifying = [row for row in result
                  if float(row[acctbal_idx]) > 2000.0
                  and int(row[keepN_idx]) == 1
                  and float(row[acctbal_idx]) >= 5000.0]
    assert qualifying, "No row satisfies MAX >= 5000 WHEN c_acctbal>2000"

    # Oracle: the WHEN-filtered rows are candidates for the MAX. To satisfy
    # MAX >= 5000 among those rows, at least one (nation, row) with acctbal>2000
    # and acctbal>=5000 must have keepN=1. MAXIMIZE SUM(keepN) over join rows.
    data = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT), CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0 AND c.c_custkey <= 80
    """).fetchall()
    nation_cnt = {}
    qualifying_nations = set()
    for nkey, acctbal in data:
        nkey = int(nkey); acctbal = float(acctbal)
        nation_cnt[nkey] = nation_cnt.get(nkey, 0) + 1
        if acctbal > 2000.0 and acctbal >= 5000.0:
            qualifying_nations.add(nkey)
    nation_ids = sorted(nation_cnt.keys())

    oracle_solver.create_model("entity_scoped_when_max_triple")
    kv = {n: f"keepN_{n}" for n in nation_ids}
    for n in nation_ids:
        oracle_solver.add_variable(kv[n], VarType.BINARY)
    # MAX>=5000 WHEN c_acctbal>2000 → at least one qualifying nation kept
    if qualifying_nations:
        oracle_solver.add_constraint(
            {kv[n]: 1.0 for n in sorted(qualifying_nations)},
            ">=", 1.0, name="max_when",
        )
    oracle_solver.set_objective(
        {kv[n]: float(nation_cnt[n]) for n in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()

    decidb_obj = sum(int(row[keepN_idx]) for row in result)
    assert abs(decidb_obj - oracle_result.objective_value) < 1e-4, \
        f"DecidB={decidb_obj:.4f}, Oracle={oracle_result.objective_value:.4f}"


@pytest.mark.correctness
@pytest.mark.var_integer
def test_entity_scoped_ne_oracle(decidb_cli, duckdb_conn, oracle_solver):
    """Entity-scoped + NE (<>) with oracle verification.

    qty ∈ [8, 10], qty <> 10 removes the unconstrained optimum. Forces the
    solver to qty=9 on every nation via the Big-M disjunction rewrite.
    Previously entity_scoped NE tests were constraint-only.
    """
    sql = """
        SELECT n_nationkey, qty
        FROM nation WHERE n_regionkey <= 2
        DECIDE nation.qty(INT)
        SUCH THAT qty >= 8 AND qty <= 10 AND qty <> 10
        MAXIMIZE SUM(qty * n_nationkey)
    """
    result, cols = decidb_cli.execute(sql)
    assert len(result) == 15

    nkey_idx = cols.index("n_nationkey")
    qty_idx = cols.index("qty")

    per_nation = {}
    for row in result:
        k = int(row[nkey_idx])
        q = int(row[qty_idx])
        per_nation[k] = q
        assert 8 <= q <= 10 and q != 10, f"qty={q} out of domain {{8,9}}"

    # Oracle: qty_n ∈ {8, 9}. Maximize SUM(qty_n * n). Optimum qty_n=9 for
    # every nation with n>0; for n=0 the coefficient is 0, so 8 or 9 both
    # optimal there. Implement NE via Big-M disjunction (for code-path parity).
    data = duckdb_conn.execute("""
        SELECT CAST(n_nationkey AS BIGINT)
        FROM nation WHERE n_regionkey <= 2
    """).fetchall()
    nation_ids = sorted(int(r[0]) for r in data)

    oracle_solver.create_model("entity_scoped_ne")
    qv = {n: f"qty_{n}" for n in nation_ids}
    zv = {n: f"z_{n}" for n in nation_ids}
    BIG_M = 20.0
    for n in nation_ids:
        oracle_solver.add_variable(qv[n], VarType.INTEGER, lb=8.0, ub=10.0)
        oracle_solver.add_variable(zv[n], VarType.BINARY)
        # qty - M*z <= 9 (z=0 → qty<=9; z=1 → qty<=9+M trivially)
        oracle_solver.add_constraint(
            {qv[n]: 1.0, zv[n]: -BIG_M},
            "<=", 9.0, name=f"ne_le_{n}",
        )
        # qty - M*z >= 11 - M  (z=1 → qty>=11; z=0 → qty>=11-M trivially)
        oracle_solver.add_constraint(
            {qv[n]: 1.0, zv[n]: -BIG_M},
            ">=", 11.0 - BIG_M, name=f"ne_ge_{n}",
        )
    oracle_solver.set_objective(
        {qv[n]: float(n) for n in nation_ids},
        ObjSense.MAXIMIZE,
    )
    oracle_result = oracle_solver.solve()

    decidb_obj = sum(int(row[nkey_idx]) * int(row[qty_idx]) for row in result)
    assert abs(decidb_obj - oracle_result.objective_value) < 1e-4, \
        f"DecidB={decidb_obj:.4f}, Oracle={oracle_result.objective_value:.4f}"


# ---------------------------------------------------------------------------
# Test E2: Row-scoped variables on a 1-to-many (fan-out) JOIN.
# Baseline contrast to entity-scoping: each JOIN row has its own variable,
# so duplicates from the fan-out share no state.
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.sql_joins
def test_row_scoped_vars_on_fanout_join(
    decidb_cli, duckdb_conn, oracle_solver
):
    """Row-scoped `x(BOOL)` on a 1-to-many orders×lineitem JOIN.

    Each duplicated join row gets its own solver variable — the aggregate
    SUM still sees every row independently, so the optimum is the textbook
    0/1 knapsack on join rows (not on order entities). Contrast with
    `test_entity_scoped_*` where `orders.x` would pick one value per order.
    """
    sql = """
        SELECT o.o_orderkey, l.l_linenumber, l.l_quantity, l.l_extendedprice, x
        FROM orders o JOIN lineitem l ON o.o_orderkey = l.l_orderkey
        WHERE o.o_orderkey <= 10
        DECIDE x(BOOL)
        SUCH THAT SUM(x * l_quantity) <= 50
        MAXIMIZE SUM(x * l_extendedprice)
    """
    decidb_rows, decidb_cols = decidb_cli.execute(sql)

    data = duckdb_conn.execute("""
        SELECT CAST(o.o_orderkey AS BIGINT),
               CAST(l.l_linenumber AS BIGINT),
               CAST(l.l_quantity AS DOUBLE),
               CAST(l.l_extendedprice AS DOUBLE)
        FROM orders o JOIN lineitem l ON o.o_orderkey = l.l_orderkey
        WHERE o.o_orderkey <= 10
    """).fetchall()
    n = len(data)

    oracle_solver.create_model("row_scoped_fanout_join")
    vnames = [f"x_{i}" for i in range(n)]
    for v in vnames:
        oracle_solver.add_variable(v, VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[i]: data[i][2] for i in range(n)},
        "<=", 50.0, name="qty_cap",
    )
    oracle_solver.set_objective(
        {vnames[i]: data[i][3] for i in range(n)}, ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    from comparison.compare import compare_solutions
    cmp = compare_solutions(
        decidb_rows, decidb_cols, res, data, ["x"],
        coeff_fn=lambda row: {"x": float(row[decidb_cols.index("l_extendedprice")])},
    )
    assert cmp.status in ("identical", "optimal")


# ---------------------------------------------------------------------------
# Test E3: Three-way interaction — uncorrelated scalar subquery RHS +
# entity-scoped variable + PER grouping.
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.cons_subquery
@pytest.mark.per_clause
def test_entity_scoped_subquery_per_three_way(
    decidb_cli, duckdb_conn, oracle_solver
):
    """Uncorrelated scalar subquery RHS shared across PER groups on an
    entity-scoped variable.

    Exercises: (1) scalar subquery evaluated once and reused for every PER
    group, (2) PER grouping by an entity-table column, (3) entity-scoped
    variable consistency across rows in each group.
    """
    sql = """
        SELECT n.n_nationkey, n.n_regionkey, n.n_name, keepN
        FROM nation n
        WHERE n.n_regionkey IN (0, 1)
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) <= (SELECT CAST(COUNT(*) / 2 AS INTEGER) FROM nation)
                  PER n_regionkey
        MAXIMIZE SUM(keepN)
    """
    decidb_rows, decidb_cols = decidb_cli.execute(sql)

    rhs_val = duckdb_conn.execute(
        "SELECT CAST(COUNT(*) / 2 AS INTEGER) FROM nation"
    ).fetchone()[0]

    data = duckdb_conn.execute("""
        SELECT CAST(n.n_nationkey AS BIGINT), CAST(n.n_regionkey AS BIGINT)
        FROM nation n WHERE n.n_regionkey IN (0, 1)
    """).fetchall()
    nation_ids = sorted(int(r[0]) for r in data)
    nation_region = {int(r[0]): int(r[1]) for r in data}

    oracle_solver.create_model("entity_scope_subq_per")
    vnames = {nk: f"keepN_{nk}" for nk in nation_ids}
    for nk in nation_ids:
        oracle_solver.add_variable(vnames[nk], VarType.BINARY)

    # PER n_regionkey: one SUM(keepN) <= rhs_val constraint per region group.
    regions: dict[int, list[int]] = {}
    for nk in nation_ids:
        regions.setdefault(nation_region[nk], []).append(nk)
    for region, nks in regions.items():
        oracle_solver.add_constraint(
            {vnames[nk]: 1.0 for nk in nks},
            "<=", float(rhs_val), name=f"per_region_{region}",
        )

    oracle_solver.set_objective(
        {vnames[nk]: 1.0 for nk in nation_ids}, ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    keep_idx = decidb_cols.index("keepN")
    nkey_idx = decidb_cols.index("n_nationkey")
    # Aggregate row-level keepN back to entity-level (same nation → same var).
    nation_to_keep: dict[int, int] = {}
    for row in decidb_rows:
        nkey = int(row[nkey_idx])
        k = int(row[keep_idx])
        if nkey in nation_to_keep:
            assert nation_to_keep[nkey] == k, \
                f"Nation {nkey} inconsistent keepN: {nation_to_keep[nkey]} vs {k}"
        else:
            nation_to_keep[nkey] = k
    decidb_obj = sum(nation_to_keep.values())
    assert abs(decidb_obj - res.objective_value) < 1e-6, (
        f"DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


# ---------------------------------------------------------------------------
# Test E4: NULL in the entity-key column. Per `physical_decide.cpp:1578-1607`,
# DecidB tags NULL vs non-NULL in the composite entity key; all NULL-keyed
# rows share a single entity variable. Oracle mirrors this exactly — a
# divergence (e.g., DecidB accidentally creating one entity per NULL row)
# would surface as a vector mismatch.
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_null_key(
    decidb_cli, duckdb_conn, oracle_solver
):
    """NULL-keyed rows group into one entity variable (documented semantics).

    The table has two NULL-key rows and one non-NULL row. Setting the
    shared NULL-entity variable to 1 forces both NULL rows to show keep=1,
    which means SUM(keep) = 2 across those two rows. A naive implementation
    that gave each NULL row its own variable would pick the highest-value
    NULL row (obj=200) under SUM(keep)=1; DecidB instead picks the non-NULL
    row (obj=10) because the NULL entity cannot be turned on without
    violating the count cap.
    """
    sql = """
        WITH t_null_entity(nk, val) AS (
            SELECT NULL::INTEGER, 100 UNION ALL
            SELECT NULL::INTEGER, 200 UNION ALL
            SELECT 1, 10
        )
        SELECT n.nk, n.val, keep
        FROM t_null_entity n
        DECIDE n.keep(BOOL)
        SUCH THAT SUM(keep) = 1
        MAXIMIZE SUM(keep * n.val)
    """
    decidb_rows, decidb_cols = decidb_cli.execute(sql)

    # Oracle: two entities — NULL-entity (covers both NULL rows, combined
    # coefficient 300 = 100 + 200) and entity nk=1 (coefficient 10).
    # SUM(keep) counts one contribution per *row*, so the NULL entity
    # contributes twice to SUM(keep) (= 2 * keep_NULL).
    oracle_solver.create_model("entity_scoped_null_key")
    oracle_solver.add_variable("keep_NULL", VarType.BINARY)
    oracle_solver.add_variable("keep_1", VarType.BINARY)
    oracle_solver.add_constraint(
        {"keep_NULL": 2.0, "keep_1": 1.0}, "=", 1.0, name="sum_eq_1",
    )
    oracle_solver.set_objective(
        {"keep_NULL": 300.0, "keep_1": 10.0}, ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL
    assert res.objective_value == pytest.approx(10.0), (
        f"Expected obj=10 (nk=1 row, NULL-entity forced to 0), got {res.objective_value}"
    )

    # Compute DecidB's objective from actual row values (order-independent).
    keep_idx = decidb_cols.index("keep")
    nk_idx = decidb_cols.index("nk")
    val_idx = decidb_cols.index("val")
    decidb_obj = sum(
        float(row[val_idx]) * int(row[keep_idx]) for row in decidb_rows
    )
    assert decidb_obj == pytest.approx(res.objective_value), (
        f"DecidB obj={decidb_obj}, oracle obj={res.objective_value}"
    )
    # Verify NULL-entity consistency: both NULL rows must have the same keep value.
    null_vals = [int(row[keep_idx]) for row in decidb_rows if row[nk_idx] is None]
    assert len(set(null_vals)) == 1, (
        f"NULL-keyed rows should share an entity variable, got keep values: {null_vals}"
    )


# ---------------------------------------------------------------------------
# Three-table fan-out (customer × nation × region) with PER on the outer table
# ---------------------------------------------------------------------------

@pytest.mark.correctness
def test_entity_scoped_three_way_join_per_region(
    decidb_cli, duckdb_conn, oracle_solver
):
    """Three-table fan-out with entity variable on the middle table and
    PER on the outer table.

    Join customer × nation × region. Entity variable lives on nation
    (`n.keepN`). Per-region cap enforces ``SUM(keepN) <= 25 PER r_name`` —
    where SUM counts customer join rows, so each nation contributes its
    customer count to its region's sum. Exercises that the VarIndexer
    correctly deduplicates the nation variable across the outer-joined
    region rows.
    """
    sql = """
        SELECT c.c_custkey, n.n_nationkey, n.n_regionkey,
               r.r_name, c.c_acctbal, keepN
        FROM customer c
          JOIN nation n ON c.c_nationkey = n.n_nationkey
          JOIN region r ON n.n_regionkey = r.r_regionkey
        WHERE c.c_custkey <= 300 AND n.n_regionkey IN (0, 1)
        DECIDE n.keepN(BOOL)
        SUCH THAT SUM(keepN) <= 25 PER r_name
        MAXIMIZE SUM(keepN * c.c_acctbal)
    """
    decidb_rows, decidb_cols = decidb_cli.execute(sql)
    assert len(decidb_rows) > 0

    # Entity consistency: every row with the same nation must have the same keepN.
    nk_idx = decidb_cols.index("n_nationkey")
    keep_idx = decidb_cols.index("keepN")
    nation_keep = {}
    for row in decidb_rows:
        nk = int(row[nk_idx])
        kv = int(row[keep_idx])
        if nk in nation_keep:
            assert nation_keep[nk] == kv, (
                f"nation {nk} has inconsistent keepN: "
                f"{nation_keep[nk]} vs {kv}"
            )
        else:
            nation_keep[nk] = kv

    data = duckdb_conn.execute("""
        SELECT CAST(c.c_custkey AS BIGINT),
               CAST(n.n_nationkey AS BIGINT),
               CAST(n.n_regionkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c
          JOIN nation n ON c.c_nationkey = n.n_nationkey
          JOIN region r ON n.n_regionkey = r.r_regionkey
        WHERE c.c_custkey <= 300 AND n.n_regionkey IN (0, 1)
    """).fetchall()

    # One variable per nation; aggregate join-row coefficients per nation.
    nation_ids = sorted({int(row[1]) for row in data})
    nation_region = {int(row[1]): int(row[2]) for row in data}
    nation_count = {nk: 0 for nk in nation_ids}
    nation_acctbal = {nk: 0.0 for nk in nation_ids}
    for row in data:
        nk = int(row[1])
        nation_count[nk] += 1
        nation_acctbal[nk] += float(row[3])

    oracle_solver.create_model("entity_scoped_three_way_join_per_region")
    vnames = {nk: f"keepN_{nk}" for nk in nation_ids}
    for nk in nation_ids:
        oracle_solver.add_variable(vnames[nk], VarType.BINARY)

    # Per-region row-count cap: each nation contributes its cust-count to its
    # region's sum. SUM(keepN) over a region = sum of customer join rows in
    # that region whose nation is selected.
    regions = sorted({nation_region[nk] for nk in nation_ids})
    for rk in regions:
        oracle_solver.add_constraint(
            {
                vnames[nk]: float(nation_count[nk])
                for nk in nation_ids
                if nation_region[nk] == rk
            },
            "<=", 25.0, name=f"region_{rk}_cap",
        )

    oracle_solver.set_objective(
        {vnames[nk]: nation_acctbal[nk] for nk in nation_ids},
        ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    # DecidB objective from actual row values.
    ac_idx = decidb_cols.index("c_acctbal")
    decidb_obj = sum(
        int(row[keep_idx]) * float(row[ac_idx]) for row in decidb_rows
    )
    assert abs(decidb_obj - res.objective_value) < 1e-4, (
        f"Objective mismatch: DecidB={decidb_obj}, Oracle={res.objective_value}"
    )


# ---------------------------------------------------------------------------
# Entity-scope over a subquery / CTE that wraps a base table
# ---------------------------------------------------------------------------
# Regression: plan_decide.cpp used to read child bindings AFTER calling
# CreatePlan on the child, but LogicalProjection::CreatePlan std::moves its
# `expressions` vector into the physical op, which leaves GetColumnBindings()
# empty. The resulting entity_key_physical_indices stayed empty, collapsing
# all rows into a single entity and silently producing all-zero decisions.
# Fix: capture child_bindings before CreatePlan.


@pytest.mark.correctness
def test_entity_scoped_over_subquery_of_base_table(
    decidb_cli, duckdb_conn, oracle_solver
):
    """Entity-scoped variable sourced from a subquery that wraps a base table.

    Regression for a bug where `FROM (SELECT ... FROM base) t DECIDE t.x ...`
    silently collapsed every row into a single shared entity (all keep=0).
    """
    sql = """
        SELECT t.rk, t.val, keep
        FROM (
            SELECT n_nationkey AS rk, CAST(n_nationkey AS DOUBLE) AS val
            FROM nation
        ) t
        DECIDE t.keep(BOOL)
        SUCH THAT SUM(keep) <= 5
        MAXIMIZE SUM(keep * t.val)
    """
    decidb_rows, decidb_cols = decidb_cli.execute(sql)

    data = duckdb_conn.execute(
        "SELECT CAST(n_nationkey AS BIGINT), CAST(n_nationkey AS DOUBLE) FROM nation"
    ).fetchall()
    rks = sorted({int(row[0]) for row in data})

    oracle_solver.create_model("entity_scope_subquery_of_base")
    vnames = {rk: f"keep_{rk}" for rk in rks}
    for rk in rks:
        oracle_solver.add_variable(vnames[rk], VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[rk]: 1.0 for rk in rks}, "<=", 5.0, name="sum_cap",
    )
    oracle_solver.set_objective(
        {vnames[rk]: float(rk) for rk in rks}, ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    keep_idx = decidb_cols.index("keep")
    val_idx = decidb_cols.index("val")
    rk_idx = decidb_cols.index("rk")
    decidb_obj = sum(
        int(row[keep_idx]) * float(row[val_idx]) for row in decidb_rows
    )
    assert decidb_obj == pytest.approx(res.objective_value), (
        f"DecidB obj={decidb_obj}, oracle obj={res.objective_value}"
    )
    # Entity consistency: rows with the same rk share the same keep value.
    per_rk = {}
    for row in decidb_rows:
        rk = int(row[rk_idx])
        keep = int(row[keep_idx])
        if rk in per_rk:
            assert per_rk[rk] == keep, f"rk={rk}: inconsistent keep"
        else:
            per_rk[rk] = keep


@pytest.mark.correctness
def test_entity_scoped_over_cte_of_base_table(
    decidb_cli, duckdb_conn, oracle_solver
):
    """Same regression shape as the subquery form, but via a WITH-CTE."""
    sql = """
        WITH t(rk, val) AS (
            SELECT n_nationkey, CAST(n_nationkey AS DOUBLE) FROM nation
        )
        SELECT t.rk, t.val, keep
        FROM t
        DECIDE t.keep(BOOL)
        SUCH THAT SUM(keep) <= 5
        MAXIMIZE SUM(keep * t.val)
    """
    decidb_rows, decidb_cols = decidb_cli.execute(sql)

    data = duckdb_conn.execute(
        "SELECT CAST(n_nationkey AS BIGINT) FROM nation"
    ).fetchall()
    rks = sorted({int(row[0]) for row in data})

    oracle_solver.create_model("entity_scope_cte_of_base")
    vnames = {rk: f"keep_{rk}" for rk in rks}
    for rk in rks:
        oracle_solver.add_variable(vnames[rk], VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[rk]: 1.0 for rk in rks}, "<=", 5.0, name="sum_cap",
    )
    oracle_solver.set_objective(
        {vnames[rk]: float(rk) for rk in rks}, ObjSense.MAXIMIZE,
    )
    res = oracle_solver.solve()
    assert res.status == SolverStatus.OPTIMAL

    keep_idx = decidb_cols.index("keep")
    val_idx = decidb_cols.index("val")
    decidb_obj = sum(
        int(row[keep_idx]) * float(row[val_idx]) for row in decidb_rows
    )
    assert decidb_obj == pytest.approx(res.objective_value), (
        f"DecidB obj={decidb_obj}, oracle obj={res.objective_value}"
    )


# ---------------------------------------------------------------------------
# NULL-semantics divergence: entity-scope vs PER
# ---------------------------------------------------------------------------

@pytest.mark.correctness
@pytest.mark.per_clause
def test_entity_scoped_vs_per_null_semantics(
    decidb_cli, duckdb_conn, oracle_solver
):
    """Side-by-side: NULLs collapse into one entity, but PER drops NULL groups.

    Two queries over the same synthetic-NULL shape. The entity-scope form
    shares a single variable across all NULL-key rows (so turning it on
    costs `num_null_rows` against the count cap). The PER form excludes
    NULL-keyed rows from groups entirely, letting them float free.
    """
    base_cte = """
        WITH t(rk, val) AS (
            SELECT NULLIF(n_regionkey, 0)::INTEGER,
                   CAST(n_nationkey AS DOUBLE)
            FROM nation
        )
    """

    # --- (A) Entity-scope: one variable per distinct rk (NULLs share) ---
    sql_entity = base_cte + """
        SELECT t.rk, t.val, keep
        FROM t
        DECIDE t.keep(BOOL)
        SUCH THAT SUM(keep) <= 5
        MAXIMIZE SUM(keep * t.val)
    """
    rows_entity, cols_entity = decidb_cli.execute(sql_entity)

    data = duckdb_conn.execute("""
        SELECT NULLIF(n_regionkey, 0),
               CAST(n_nationkey AS DOUBLE)
        FROM nation
    """).fetchall()
    # Collapse NULL rk into one entity key
    def _key(rk):
        return "NULL" if rk is None else f"r{int(rk)}"
    entities = {}
    counts = {}
    coeffs = {}
    for rk, val in data:
        k = _key(rk)
        entities.setdefault(k, True)
        counts[k] = counts.get(k, 0) + 1
        coeffs[k] = coeffs.get(k, 0.0) + float(val)
    keys = sorted(entities)
    oracle_solver.create_model("entity_null_vs_per_A")
    vnames = {k: f"keep_{k}" for k in keys}
    for k in keys:
        oracle_solver.add_variable(vnames[k], VarType.BINARY)
    oracle_solver.add_constraint(
        {vnames[k]: float(counts[k]) for k in keys}, "<=", 5.0, name="cap",
    )
    oracle_solver.set_objective(
        {vnames[k]: coeffs[k] for k in keys}, ObjSense.MAXIMIZE,
    )
    res_a = oracle_solver.solve()
    assert res_a.status == SolverStatus.OPTIMAL

    keep_idx = cols_entity.index("keep")
    val_idx = cols_entity.index("val")
    rk_idx = cols_entity.index("rk")
    obj_a = sum(int(r[keep_idx]) * float(r[val_idx]) for r in rows_entity)
    assert obj_a == pytest.approx(res_a.objective_value), (
        f"(A) entity-scope: DecidB obj={obj_a}, oracle={res_a.objective_value}"
    )
    # NULL rows must all share the same keep value.
    null_keeps = [int(r[keep_idx]) for r in rows_entity if r[rk_idx] is None]
    assert null_keeps and len(set(null_keeps)) == 1, (
        f"(A) NULL-keyed rows should share an entity, got: {null_keeps}"
    )

    # --- (B) Row-scope + PER rk: one picked row per non-NULL group; NULLs free ---
    sql_per = base_cte + """
        SELECT t.rk, t.val, keep
        FROM t
        DECIDE keep(BOOL)
        SUCH THAT SUM(keep) <= 1 PER rk
        MAXIMIZE SUM(keep * t.val)
    """
    rows_per, cols_per = decidb_cli.execute(sql_per)

    # Oracle for (B): one variable per row. NULL rows are free of the PER cap.
    oracle_solver.create_model("entity_null_vs_per_B")
    row_vars = []
    for i, (rk, val) in enumerate(data):
        vn = f"keep_row_{i}"
        oracle_solver.add_variable(vn, VarType.BINARY)
        row_vars.append((vn, rk, float(val)))
    # PER rk excludes NULL rows; one SUM<=1 per non-NULL rk
    non_null_rks = sorted({rk for _, rk, _ in row_vars if rk is not None})
    for rk in non_null_rks:
        oracle_solver.add_constraint(
            {vn: 1.0 for vn, r, _ in row_vars if r == rk},
            "<=", 1.0, name=f"cap_r{rk}",
        )
    oracle_solver.set_objective(
        {vn: c for vn, _, c in row_vars}, ObjSense.MAXIMIZE,
    )
    res_b = oracle_solver.solve()
    assert res_b.status == SolverStatus.OPTIMAL

    keep_idx_b = cols_per.index("keep")
    val_idx_b = cols_per.index("val")
    obj_b = sum(int(r[keep_idx_b]) * float(r[val_idx_b]) for r in rows_per)
    assert obj_b == pytest.approx(res_b.objective_value), (
        f"(B) PER: DecidB obj={obj_b}, oracle={res_b.objective_value}"
    )

    # Divergence check: the two semantics should give different optima on
    # this shape (NULL rows free in PER, constrained in entity-scope).
    assert obj_b > obj_a, (
        f"Expected PER obj > entity-scope obj (NULL rows free under PER), "
        f"got entity={obj_a}, per={obj_b}"
    )


# ---------------------------------------------------------------------------
# Test N: Entity-scoped per-row constraint with linear LHS (x + const)
# ---------------------------------------------------------------------------
# Regression for Fix A: a per-row constraint like `x + 3 <= 10` on an
# entity-scoped variable emits one constraint per join row, all referencing
# the SAME solver variable. The fix must not introduce duplicate-index
# entries or per-row coefficient accumulation errors when multiple rows
# share the same underlying var.

@pytest.mark.correctness
def test_entity_scoped_perrow_linear_lhs(decidb_cli, duckdb_conn, oracle_solver, perf_tracker):
    """Entity-scoped var with per-row `x + 3 <= 10`. Each entity gets constrained
    once per join row; all copies agree on the same upper bound (7), so the
    shared variable's feasible range is x <= 7."""
    sql = """
        SELECT c.c_custkey, n.n_nationkey, keepN
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
        DECIDE n.keepN(INT)
        SUCH THAT keepN + 3 <= 10
        MAXIMIZE SUM(keepN * c.c_acctbal)
    """
    decidb_result, decidb_cols = decidb_cli.execute(sql)

    # Verify entity consistency (same nation → same keepN) and upper bound.
    nation_values = {}
    for row in decidb_result:
        nkey = int(row[1])
        keep = int(row[2])
        assert 0 <= keep <= 7, f"Nation {nkey} keepN={keep} violates entity-scoped x+3<=10 bound"
        if nkey in nation_values:
            assert nation_values[nkey] == keep, (
                f"Nation {nkey} entity inconsistency: {nation_values[nkey]} vs {keep}"
            )
        nation_values[nkey] = keep

    # Oracle comparison: one var per nation with ub=7.
    data = duckdb_conn.execute("""
        SELECT CAST(c.c_custkey AS BIGINT), CAST(n.n_nationkey AS BIGINT),
               CAST(c.c_acctbal AS DOUBLE)
        FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey
        WHERE n.n_regionkey = 0
    """).fetchall()
    nation_ids = sorted(set(int(r[1]) for r in data))
    nation_acctbal = {}
    for r in data:
        nation_acctbal[int(r[1])] = nation_acctbal.get(int(r[1]), 0.0) + float(r[2])

    oracle_solver.create_model("entity_perrow_linear")
    vnames = {nk: f"keepN_{nk}" for nk in nation_ids}
    for nk in nation_ids:
        oracle_solver.add_variable(vnames[nk], VarType.INTEGER, lb=0.0, ub=7.0)
    oracle_solver.set_objective(
        {vnames[nk]: nation_acctbal[nk] for nk in nation_ids},
        ObjSense.MAXIMIZE,
    )
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    decidb_obj = 0.0
    for r in data:
        decidb_obj += nation_values.get(int(r[1]), 0) * float(r[2])
    assert abs(decidb_obj - result.objective_value) < 1e-4, (
        f"Entity-scoped per-row linear LHS: DecidB={decidb_obj}, oracle={result.objective_value}"
    )

