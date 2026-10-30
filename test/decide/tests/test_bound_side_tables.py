"""The reducers a bound reads from the data are computed once per group and joined back to the rows.

`sum(x) by (g) <= sum(cap) by (g)` reads `sum(cap) by (g)` as one column of the source: a grouped aggregate over a
read of the source (a side table), joined to each row by `g` with IS NOT DISTINCT FROM, the way DuckDB decorrelates a
scalar subquery. Every case is judged by `_scope_oracle`, which states the rule independently: the solver path
(`off`) and the plan the default mode picks (`auto`, a direct-solve rule where one applies) must reach the oracle's
optimum, and a case a rule takes also runs under `require`. Each case names the branch of the side table it reaches.
"""

import math

import pytest

from ._direct_oracle_common import _allow_direct_path, run  # noqa: F401
from ._scope_cases import aligned, scope_solver  # noqa: F401
from ._scope_oracle import CONSTANT, GLOBAL, Decision, Reduce, RelationColumns, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense, VarType

COLUMNS = ("id", "g", "k", "f", "v", "cap", "p")
NAN = float("nan")
# `k` determines `v` and `g` determines `cap`, so a `per k: v` or `per g: cap` reducer holds. Ids 1 and 2 differ in the
# case of `g` and the sign of `f`; ids 3 and 4 share a NaN `f`; ids 5 and 6 have no `g`; ids 7 and 8 differ in `k`.
ROWS = (
    (1, "a", 1, 0.0, 2.0, 3.0, 5.0),
    (2, "A", 1, -0.0, 2.0, 3.0, 4.0),
    (3, "b", 2, NAN, 3.0, 2.0, 3.0),
    (4, "b", 2, NAN, 3.0, 2.0, 2.0),
    (5, None, 3, 1.0, 1.0, 4.0, 6.0),
    (6, None, 3, 1.0, 1.0, 4.0, 1.0),
    (7, "c", 4, 2.0, 4.0, 1.0, 7.0),
    (8, "c", 5, 2.0, 5.0, 3.0, 2.0),
)
RECORDS = [dict(zip(COLUMNS, row)) for row in ROWS]
X = Decision("x", var_type=VarType.BINARY)
MAXIMIZE_P = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda row: {"x": row["p"]}),))


def _literal(value):
    if value is None:
        return "NULL"
    if isinstance(value, str):
        return "'" + value + "'"
    if isinstance(value, float) and math.isnan(value):
        return "CAST('NaN' AS DOUBLE)"
    if isinstance(value, float):
        return f"CAST('{value!r}' AS DOUBLE)"
    return str(value)


VALUES = "(VALUES " + ", ".join("(" + ", ".join(_literal(v) for v in row) + ")" for row in ROWS) + ")"
TABLE = f"{VALUES} t({', '.join(COLUMNS)})"
# `d` is the distinct (g, cap) pairs: a relation whose columns compare NULL as a value, so ids 5 and 6 are one group.
RELATION = (
    f"{TABLE} JOIN (SELECT DISTINCT g AS dg, cap AS dcap FROM {TABLE}) d "
    "ON t.g IS NOT DISTINCT FROM d.dg AND t.cap = d.dcap"
)
# The same rows as a join, so the source is not a chain over one scan (the reader materializes it once).
JOINED = f"{TABLE} JOIN (VALUES (1), (2), (3), (4), (5)) u(k2) ON k = k2"


def _x(row):
    return {"x": 1}


def _const(fn):
    return lambda row: {CONSTANT: fn(row)}


def _sum_x(by):
    return Reduce("sum", _x, by=by)


def _folded_f(row):
    # DuckDB groups -0.0 with 0.0 and one NaN with another; Python's `nan != nan` would not.
    return ("nan",) if math.isnan(row["f"]) else (row["f"] + 0.0,)


def _folded_g(row):
    return (None if row["g"] is None else row["g"].lower(),)


def _capsum_by_k(row):
    return sum(r["cap"] for r in RECORDS if r["k"] == row["k"])


# name -> (source, SUCH THAT ..., clauses, direct solve takes it)
CASES = {
    # A reducer with no `by`: the table has no key, and every row is joined to its one row.
    "no_key": (
        TABLE,
        "sum(x) <= sum(cap) / 3",
        [Scoped((Reduce("sum", _x),), "<=", (Reduce("sum", _const(lambda r: r["cap"] / 3)),), per=GLOBAL)],
        True,
    ),
    # `sum(per k: v)` counts each class of k once: 2 + 3 + 1 + 4 + 5, not the 21 of all rows.
    "count_each_class_once": (
        TABLE,
        "sum(x) <= sum(per k: v) / 2.5",
        [Scoped((Reduce("sum", _x),), "<=", (Reduce("sum", _const(lambda r: r["v"] / 2.5), per=("k",)),), per=GLOBAL)],
        False,
    ),
    "count_each_class_once_by_a_key": (
        TABLE,
        "sum(x) by (g) <= sum(per k: v) by (g) - 1",
        [
            Scoped(
                (_sum_x(("g",)),),
                "<=",
                (Reduce("sum", _const(lambda r: r["v"]), per=("k",), by=("g",)), _const(lambda r: -1)),
            )
        ],
        False,
    ),
    # The clause's `when` keeps ids 1, 2, 5 and 7: the classes 'a', 'A' and 'c' (3 + 3 + 1), and the kept id 5, whose
    # `g` is NULL, is in none. Counting class 'b' too gives 9; counting id 5 gives 11.
    "a_class_count_on_the_clauses_rows": (
        TABLE,
        "when p > 3: sum(x) <= sum(per g: cap) / 2",
        [
            Scoped(
                (Reduce("sum", _x),),
                "<=",
                (Reduce("sum", _const(lambda r: r["cap"] / 2), per=("g",)),),
                per=GLOBAL,
                when=lambda r: r["p"] > 3,
            )
        ],
        False,
    ),
    # A row whose written `per` column is NULL is in no class: ids 5 and 6 are not counted (4 classes, not 5).
    "a_null_class_is_not_counted": (
        TABLE,
        "sum(x) <= sum(per g: 1)",
        [Scoped((Reduce("sum", _x),), "<=", (Reduce("sum", _const(lambda r: 1), per=("g",)),), per=GLOBAL)],
        False,
    ),
    # COUNT(*) has no argument to hold back, so only the aggregate's filter keeps the rows of the clause's `when` out.
    "a_count_of_the_clauses_rows": (
        TABLE,
        "when p > 3: sum(x) by (g) <= count(*) by (g) - 1",
        [
            Scoped(
                (_sum_x(("g",)),),
                "<=",
                (Reduce("sum", _const(lambda r: 1), by=("g",)), _const(lambda r: -1)),
                when=lambda r: r["p"] > 3,
            )
        ],
        True,
    ),
    # A group in which the reducer's own `when` keeps no row has a NULL bound, which skips its instance.
    "a_group_the_reducer_reads_no_row_of": (
        TABLE,
        "sum(x) by (g) <= sum(when v > 3: v) by (g)",
        [
            Scoped(
                (_sum_x(("g",)),),
                "<=",
                (Reduce("sum", _const(lambda r: r["v"]), when=lambda r: r["v"] > 3, by=("g",)),),
            )
        ],
        True,
    ),
    # A row whose written `by` column is NULL is in no group.
    "a_null_key_is_no_group": (
        TABLE,
        "sum(x) by (g) <= sum(cap) by (g) - 2",
        [Scoped((_sum_x(("g",)),), "<=", (Reduce("sum", _const(lambda r: r["cap"]), by=("g",)), _const(lambda r: -2)))],
        True,
    ),
    # 'a' and 'A' are one group under NOCASE: the table is grouped and joined under the collation.
    "a_collated_key": (
        TABLE,
        "sum(x) by (g COLLATE NOCASE) <= sum(cap) by (g COLLATE NOCASE) - 2",
        [
            Scoped(
                (_sum_x(_folded_g),),
                "<=",
                (Reduce("sum", _const(lambda r: r["cap"]), by=_folded_g), _const(lambda r: -2)),
            )
        ],
        True,
    ),
    # 0.0 and -0.0 are one group, and so are two NaNs, in the table and in the join.
    "zero_and_nan_keys": (
        TABLE,
        "sum(x) by (f) <= sum(cap) by (f) - 2",
        [
            Scoped(
                (_sum_x(_folded_f),),
                "<=",
                (Reduce("sum", _const(lambda r: r["cap"]), by=_folded_f), _const(lambda r: -2)),
            )
        ],
        True,
    ),
    # A relation's columns compare NULL as a value: ids 5 and 6 are one group, and the join finds it.
    "a_relation_key_with_a_null": (
        RELATION,
        "sum(x) by (d) <= sum(cap) by (d) / 8",
        [
            Scoped(
                (_sum_x((RelationColumns(("g", "cap")),)),),
                "<=",
                (Reduce("sum", _const(lambda r: r["cap"] / 8), by=(RelationColumns(("g", "cap")),)),),
            )
        ],
        False,
    ),
    # The bound has one value per `k` and a group of `g` holds two (ids 7 and 8): the group's tightest value bounds it,
    # which is a group bound over another bound's column.
    "a_bound_that_reads_another_bound": (
        TABLE,
        "sum(x) by (g) <= sum(cap) by (k) - 1",
        [Scoped((_sum_x(("g",)),), "<=", (_const(lambda r: _capsum_by_k(r) - 1),))],
        False,
    ),
    # The source is a join, which the reader materializes once; every read of it joins the table.
    "a_materialized_source": (
        JOINED,
        "sum(x) by (g) <= sum(cap) by (g) - 2",
        [Scoped((_sum_x(("g",)),), "<=", (Reduce("sum", _const(lambda r: r["cap"]), by=("g",)), _const(lambda r: -2)))],
        True,
    ),
}


def _sql(source, constraints):
    return f"SELECT id, x FROM {source} DECIDE x(BOOL) SUCH THAT {constraints} MAXIMIZE SUM(p * x) ORDER BY id"


@pytest.mark.correctness
@pytest.mark.parametrize("name", sorted(CASES))
def test_a_bound_read_from_the_data_is_the_oracles(decidb_cli, scope_solver, name):
    source, constraints, clauses, direct = CASES[name]
    model = ScopedModel(RECORDS, [X])
    optimum = model.solve_scoped(scope_solver, clauses, MAXIMIZE_P)
    for mode in ("off", "auto", "require") if direct else ("off", "auto"):
        rows, columns = run(decidb_cli, _sql(source, constraints), mode)
        result = aligned([dict(zip(columns, row)) for row in rows], model, "id")
        assert model.judge_scoped(result, clauses, MAXIMIZE_P) == pytest.approx(optimum), mode


@pytest.mark.correctness
def test_a_bound_over_a_stored_table_is_the_oracles(decidb_cli, scope_solver):
    """A stored source carries its row id past the plan (late materialization): the bound's column is dropped from the
    adapter's outputs as the row id is, and the answer is the oracle's."""
    source, constraints, clauses, _ = CASES["a_null_key_is_no_group"]
    model = ScopedModel(RECORDS, [X])
    optimum = model.solve_scoped(scope_solver, clauses, MAXIMIZE_P)
    sql = f"CREATE TEMP TABLE bound_rows AS SELECT * FROM {source}; {_sql('bound_rows', constraints)}"
    for mode in ("off", "require"):
        rows, columns = run(decidb_cli, sql, mode)
        result = aligned([dict(zip(columns, row)) for row in rows], model, "id")
        assert model.judge_scoped(result, clauses, MAXIMIZE_P) == pytest.approx(optimum), mode
