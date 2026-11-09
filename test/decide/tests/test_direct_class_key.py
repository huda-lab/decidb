"""The class key of a direct plan, and of the side tables a bound reads: how each key column is compared.

A rule joins every row to the one row of its class, and a bound joins every row to its group's value. The key has no
flag column beside it (a row that is in no class has a NULL where the join compares with `=`, as in SQL's outer join):

- an element whose NULL puts a row in no class (a written column, D5) is compared with `=`;
- an element where NULL is a value (a column of a relation) is compared with IS NOT DISTINCT FROM;
- a key of such elements only, with a `when` that can drop rows, has one more column, compared with `=`, that is NULL on
  a dropped row.

Each test reads the comparisons of the joins that give a row its class's or group's values from the plan. What the
plans answer is the oracles' business (test_per_class_null_key_bound.py, test_bound_side_tables.py, the rules' suites).
"""

import json
import math

import pytest

from solver.types import ObjSense, VarType

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle


def _single_join_conditions(cli, sql, mode="require"):
    """The comparison of each condition of each SINGLE join in the plan, as "=" or "NDF", one list per join."""
    out = cli.execute_raw(f"SET decide_direct_solve='{mode}'; EXPLAIN (FORMAT JSON) {sql}").stdout
    stack, joins = json.loads(out[out.index("[") :]), []
    while stack:
        node = stack.pop()
        info = node.get("extra_info", {})
        if node["name"].strip() == "HASH_JOIN" and info.get("Join Type") == "SINGLE":
            conditions = info["Conditions"]
            lines = conditions if isinstance(conditions, list) else conditions.split("\n")
            joins.append(sorted("NDF" if "IS NOT DISTINCT FROM" in line else "=" for line in lines))
        stack.extend(node.get("children", []))
    return sorted(joins)


ROWS = "(VALUES (1, 'a', 2.0, true), (2, NULL, 3.0, false), (3, 'a', 4.0, true)) t(id, g, v, f)"
# A relation: its column `region` compares NULL as a value.
RELATION = (
    "(VALUES (1, 'a', 2.0, true, 'x'), (2, NULL, 3.0, false, NULL), (3, 'a', 4.0, true, 'x')) t(id, g, v, f, region) JOIN "
    "(SELECT DISTINCT region AS dr FROM (VALUES ('x'), (CAST(NULL AS VARCHAR))) u(region)) d "
    "ON t.region IS NOT DISTINCT FROM d.dr"
)


@pytest.mark.correctness
@pytest.mark.parametrize(
    "name, sql, expected",
    [
        # A written column: a row with a NULL is in no class, so the join matches nothing for it (`=`, no flag).
        (
            "a-written-column",
            f"SELECT id, c FROM {ROWS} DECIDE per g: c(REAL) SUCH THAT c BETWEEN 0 AND 9 MINIMIZE SUM(POWER(c - v, 2))",
            [["="]],
        ),
        # A relation: NULL is a value of its column, so the class a NULL names is found (IS NOT DISTINCT FROM).
        (
            "a-relation-column",
            f"SELECT id, x FROM {RELATION} DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) by (d) <= 4 MAXIMIZE SUM(v * x)",
            [["NDF"]],
        ),
        # The same under a `when`, which can drop a row: no element says so, and the marker does (`=`).
        (
            "a-relation-column-under-a-when",
            f"SELECT id, x FROM {RELATION} DECIDE x(REAL) SUCH THAT when f: SUM(POWER(x, 2)) by (d) <= 4 "
            "MAXIMIZE SUM(v * x)",
            [["=", "NDF"]],
        ),
        # A written column under a `when`: the `when` has made the column NULL on a dropped row, so no marker.
        (
            "a-written-column-under-a-when",
            f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT when f: SUM(POWER(x, 2)) by (g) <= 4 MAXIMIZE SUM(v * x)",
            [["="]],
        ),
        # No key and a `when`: every row of the scope is in the one class, and a dropped row is in none (the marker).
        (
            "no-key-under-a-when",
            f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT when f: SUM(POWER(x, 2)) <= 4 MAXIMIZE SUM(v * x)",
            [["="]],
        ),
    ],
)
def test_the_join_of_a_class_compares_each_key_column_by_what_a_null_means(decidb_cli, name, sql, expected):
    assert _single_join_conditions(decidb_cli, sql) == expected, name


@pytest.mark.correctness
@pytest.mark.parametrize(
    "name, source, key, expected",
    [
        # A bound read from the data is a side table joined to the rows by the key of its `by`, element by element.
        ("a-written-column", ROWS, "g", [["="], ["="]]),
        ("a-relation-column", RELATION, "d", [["NDF"], ["NDF"]]),
    ],
)
def test_the_join_of_a_bound_compares_each_key_column_by_what_a_null_means(decidb_cli, name, source, key, expected):
    # The side table is built and joined by the source reader, which the solver path uses as well (its plan prints the
    # DECIDE operator's internal columns, so the comparisons are read from a direct plan). The bound's reducer has a key
    # of its own, so it is not the clause's group: the table is joined once for the bound and once for its check.
    sql = (
        f"SELECT id, x FROM {source} DECIDE x(BOOL) SUCH THAT SUM(x) <= SUM(v) by ({key}) / 8 MAXIMIZE SUM(v * x)"
    )
    assert _single_join_conditions(decidb_cli, sql) == expected, name


# Rows 1 and 2 have no `g` and name no class, so the clause `per g:` has no instance, and the groups of `by (h)` it
# would have bounded are not bounded: no group is required to hold a total of 5, which two rows cannot reach. A group
# that exists only because its key is not NULL has no bound, and no infeasibility, to show for it.
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("shape", ["bool", "real"])
def test_a_clause_with_no_instance_bounds_none_of_its_groups(decidb_cli, oracle_solver, shape):
    values = "(VALUES (1, CAST(NULL AS VARCHAR), 'a', 1.0, 3.0), (2, NULL, 'a', 1.0, 2.0), (3, NULL, 'b', 1.0, 4.0))"
    declaration = "x(BOOL)" if shape == "bool" else "x(REAL)"
    box = "" if shape == "bool" else "x BETWEEN 0 AND 1 AND "
    body = "SUM(x)" if shape == "bool" else "SUM(w * x)"
    sql = (
        f"SELECT id, x FROM (FROM {values} t(id, g, h, w, v) DECIDE {declaration} SUCH THAT {box}per g: {body} by (h) >= 5 "
        "MAXIMIZE SUM(v * x)) q ORDER BY id"
    )
    scores = {1: 3.0, 2: 2.0, 3: 4.0}
    oracle_solver.create_model("clause_without_instance")
    for i in scores:
        oracle_solver.add_variable(f"x_{i}", VarType.BINARY if shape == "bool" else VarType.CONTINUOUS, 0.0, 1.0)
    oracle_solver.set_objective({f"x_{i}": v for i, v in scores.items()}, ObjSense.MAXIMIZE)
    oracle = oracle_solver.solve()

    def check_assignment(result, columns, mode):
        chosen = {row[0]: row[1] for row in result}
        assert set(chosen) == set(scores), mode
        value = sum(scores[i] * chosen[i] for i in scores)
        assert math.isclose(value, oracle.objective_value, abs_tol=1e-6), (mode, value, oracle.objective_value)

    agree_with_oracle(decidb_cli, sql, oracle, check_assignment)
