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


KEYED_COLUMNS = ("id", "g", "h", "c", "v", "t")
# The key's values differ from the bound's, so a bound read from the key's column gives another answer.
KEYED_ROWS = (
    (1, 5, 7, 1, 5, True),
    (2, 5, 7, 1, 4, True),
    (3, 5, 8, 2, 3, False),
    (4, 6, 8, 2, 2, True),
    (5, 6, 9, 2, 1, True),
)
KEYED_CLAUSES = [
    # Group 5 reads 1, 1, 2 and holds the least; group 6 reads 2. Rows 1 and 4, 5 are worth most.
    pytest.param("sum(x) by (k.g) <= max(k.c) by (k.h)", [1, 0, 0, 1, 1], id="another-key"),
    # A reducer's `when` leaves row 3 free: rows 1, 2 share 1, and rows 4, 5 share 2.
    pytest.param("sum(when k.t: x) by (k.g) <= k.c", [1, 0, 1, 1, 1], id="deck-example-7"),
    pytest.param("per k.g: sum(when k.t: x) by (k.g) <= k.c", [1, 0, 1, 1, 1], id="under-its-per"),
]


@pytest.mark.correctness
@pytest.mark.parametrize("key_type", ["INT", "VARCHAR", "HUGEINT"])
@pytest.mark.parametrize("clause, expected", KEYED_CLAUSES)
def test_a_side_tables_value_is_read_past_its_key(decidb_cli, key_type, clause, expected):
    """A stored source a rule reads directly joins each side table's value, not its key columns: the bound is the value,
    whatever the key's type, on the plan the default mode picks as on the solver path."""
    rows = ", ".join(str(row).replace("True", "true").replace("False", "false") for row in KEYED_ROWS)
    sql = f"""CREATE TEMP TABLE keyed AS SELECT id, CAST(g AS {key_type}) AS g, h, c, v, t
            FROM (VALUES {rows}) r({', '.join(KEYED_COLUMNS)});
        SELECT id, x FROM keyed k DECIDE x(BOOL) SUCH THAT {clause} MAXIMIZE SUM(x * k.v) ORDER BY id"""
    for mode in ("off", "auto"):
        assert [row[1] for row in run(decidb_cli, sql, mode)[0]] == expected, mode


NORM_BODIES = {
    "INT": (30, 30, 30, 0),
    "DECIMAL(9,2)": (1.5, 2.5, -3.5, 0),
    "DOUBLE": (1.5, 2.5, -3.5, 0),
}


@pytest.mark.correctness
@pytest.mark.parametrize("plus", ["", " + 1"], ids=["bare", "under-a-sum"])
@pytest.mark.parametrize(
    "body, norm, value",
    [
        # Each norm's definition has another type than the SUM(e) it parses as: what reads it binds to the definition's.
        pytest.param("INT", "2", 2700, id="square-of-whole-numbers-is-a-double"),
        pytest.param("INT", "'inf'", 30, id="greatest-whole-number-is-no-hugeint"),
        pytest.param("DECIMAL(9,2)", "2", 20.75, id="square-of-a-decimal-is-a-double"),
        pytest.param("DECIMAL(9,2)", "'inf'", 3.5, id="greatest-decimal-keeps-its-width"),
        pytest.param("DOUBLE", "0", 3, id="count-of-doubles-is-whole"),
    ],
)
def test_a_norm_reads_as_its_definition(decidb_cli, body, norm, value, plus):
    """`norm(e, p)` holds its definition's value in its definition's type, however it is read and on either path."""
    rows = ", ".join(f"({i}, {v})" for i, v in enumerate(NORM_BODIES[body]))
    sql = f"""SELECT id, x FROM (SELECT id, CAST(v AS {body}) AS v FROM (VALUES {rows}) r(id, v)) t
        DECIDE x(REAL) SUCH THAT x <= norm(t.v, {norm}){plus} AND x <= 5000 MAXIMIZE sum(x) ORDER BY id"""
    expected = value + (1 if plus else 0)
    for mode in ("off", "auto"):
        assert [row[1] for row in run(decidb_cli, sql, mode)[0]] == pytest.approx([expected] * 4), mode


NORM_ROWS = "(VALUES (0, 3), (1, -4), (2, 5), (3, 0)) t(id, v)"


@pytest.mark.parametrize(
    "norm, value",
    [
        # `e` is 2, -5, 4, -1.
        pytest.param("1", 12, id="sum-of-sizes"),
        pytest.param("2", 46, id="sum-of-squares"),
        pytest.param("'inf'", 5, id="greatest-size"),
        pytest.param("0", 4, id="count-of-nonzero-rows"),
    ],
)
def test_a_norm_body_holds_a_subquery(decidb_cli, norm, value):
    """Binding sizes the norm before planning, when a bound subquery cannot be copied: it needs only the body's type."""
    sql = f"""SELECT id, x FROM {NORM_ROWS} DECIDE x(REAL)
        SUCH THAT x <= norm(t.v - (SELECT 1), {norm}) AND x <= 5000 MAXIMIZE sum(x) ORDER BY id"""
    for mode in ("off", "auto"):
        assert [row[1] for row in run(decidb_cli, sql, mode)[0]] == pytest.approx([value] * 4), mode


CAST_ROWS = "(SELECT * FROM (VALUES (0, 'a', 3), (1, 'a', -4), (2, 'b', 5), (3, 'b', 0)) r(id, g, v)) t"


@pytest.mark.parametrize(
    "bound, expected",
    [
        pytest.param("CAST(norm(t.v, 2) AS DOUBLE) + 1", [51] * 4, id="a-cast-keeps-the-norm"),
        pytest.param("TRY_CAST(norm(t.v, 2) AS DOUBLE)", [50] * 4, id="a-try-cast-keeps-the-norm"),
        # Group a holds 3, -4 and group b 5, 0.
        pytest.param("CAST(sum(t.v + 0.0) by (t.g) AS DOUBLE) + 2", [1, 1, 7, 7], id="a-cast-keeps-the-by"),
    ],
)
def test_a_cast_that_changes_nothing_keeps_what_its_operand_carries(decidb_cli, bound, expected):
    """A cast to the operand's own type binds to the operand: its tags stay, or the norm reads as SUM(e) and the
    reducer as the whole table's."""
    sql = f"""SELECT id, x FROM {CAST_ROWS} DECIDE x(REAL)
        SUCH THAT x <= {bound} AND x <= 5000 MAXIMIZE sum(x) ORDER BY id"""
    for mode in ("off", "auto"):
        assert [row[1] for row in run(decidb_cli, sql, mode)[0]] == pytest.approx(expected), mode


def test_nested_casts_that_change_nothing_print_as_the_outer(decidb_cli):
    """Each cast records its text, and they all bind to the column, which prints as what was written around it."""
    bound = "CAST(CAST(t.w AS DOUBLE) AS DOUBLE)"
    sql = f"""SELECT id, x FROM (VALUES (0, 'a', 1.0::DOUBLE), (1, 'a', 2.0::DOUBLE)) t(id, g, w) DECIDE x(REAL)
        SUCH THAT sum(x) by (t.g) = {bound} MAXIMIZE sum(x)"""
    result = decidb_cli.execute_raw(sql)
    assert f"`{bound}` takes more than one value here" in result.stderr, result.stderr


@pytest.mark.parametrize("macro", ["a", "max(a)"], ids=["a-scalar-macro", "an-aggregate-macro"])
def test_a_macro_named_sum_does_not_capture_a_norm(decidb_cli, macro):
    """A norm is DECIDE syntax. Its marker parses as SUM(e), so a macro by that name took it: a scalar one raised
    INTERNAL, and an aggregate one read max(v)'s bits as a DOUBLE."""
    sql = f"""ATTACH ':memory:' AS scratch (READ_ONLY false); USE scratch; CREATE MACRO sum(a) AS {macro};
        SELECT id, x FROM {NORM_ROWS} DECIDE x(REAL) SUCH THAT x <= norm(t.v, 2) AND x <= 5000
        MAXIMIZE system.main.sum(x) ORDER BY id"""
    assert [row[1] for row in decidb_cli.execute(sql)[0]] == pytest.approx([50] * 4)


RESCAN_SETUP = """SET threads=1; SET memory_limit='32MB';
    CREATE TEMP TABLE w AS SELECT i AS id, i % 4 AS g, i % 5 AS h, i % 2 AS k, i % 3 + 1 AS c, i + 1 AS v
    FROM range(200000) t(i); SET decide_direct_solve='require';"""


@pytest.mark.parametrize(
    "rule, query, total",
    [
        # Every `max(c)` is 3, so each group of `g` takes its top 3 values of `v`.
        pytest.param(
            "S1",
            "DECIDE x(BOOL) SUCH THAT sum(x) by (g) <= max(c) by (h) AND sum(x) by (g) <= max(c) by (k) MAXIMIZE sum(x * v)",
            2399934,
            id="two-side-tables-under-s1",
        ),
        # `x` is min(10, 3) on every row, so the total is 3 * sum(v).
        pytest.param(
            "A1", "DECIDE x(REAL) SUCH THAT x <= 10 AND x <= max(c) by (g) MAXIMIZE sum(x * v)", 60000300000, id="a1"
        ),
    ],
)
def test_a_side_tables_value_is_read_on_the_rescan_route(decidb_cli, rule, query, total):
    """200000 rows pass the copy budget, so a rule scans its source again and joins each side table to it: its value is
    read past the source's columns, table by table (TakeRecomputedSource). `require` holds the rule to the query."""
    source = f"SELECT v, x FROM w {query}"
    plan = decidb_cli.execute_raw(f"{RESCAN_SETUP} EXPLAIN {source}")
    assert rule in plan.stdout and "direct_source" not in plan.stdout, plan.stdout
    rows, _ = decidb_cli.execute(f"{RESCAN_SETUP} SELECT sum(x * v) FROM ({source})")
    assert rows[0][0] == pytest.approx(total)


@pytest.mark.parametrize(
    "bound, expected",
    [
        # Group a sums 3 and -4, group b 5 and 0: the inner `by` reads -1, -1, 5, 5.
        pytest.param("sum(q := sum(t.v + 0.0) by (t.g))", 8, id="a-reducers-by"),
        # `next` reads -4, 5, 0 and, at the last row, the `else`.
        pytest.param("sum(q := at(next else 0: t.v) over (t.id))", 1, id="a-frame"),
    ],
)
def test_an_argument_name_changes_nothing(decidb_cli, bound, expected):
    """SQL accepts `sum(q := v)`, and the name means nothing to the aggregate, so the child keeps what it carries."""
    sql = f"""SELECT id, x FROM {CAST_ROWS} DECIDE x(REAL)
        SUCH THAT x >= -50 AND x <= {bound} MAXIMIZE sum(x) ORDER BY id"""
    for mode in ("off", "auto"):
        assert [row[1] for row in run(decidb_cli, sql, mode)[0]] == pytest.approx([expected] * 4), mode


def test_a_cast_under_a_reducer_prints_as_written(decidb_cli):
    """A reducer binds its argument directly, not through ExpressionBinder::Bind, and the outer cast's text wins there
    too. The bound is 2.5, so the tightest repair adds 997.5 to reach row 2's 1000."""
    bound = "sum(CAST(CAST(t.w AS DOUBLE) AS DOUBLE))"
    rows, columns = decidb_cli.execute(
        f"""SELECT clause, suggested_change FROM (DIAGNOSE SELECT id, x
            FROM (VALUES (1, 1.0::DOUBLE, 0), (2, 1.5::DOUBLE, 1)) t(id, w, v) DECIDE x(REAL)
            SUCH THAT x <= {bound} AND x >= t.v * 1000 MAXIMIZE sum(x)) LIMIT 1"""
    )
    assert rows[0] == (f"x <= {bound}", f"x <= {bound} + 997.5"), rows


@pytest.mark.parametrize(
    "sql, expected",
    [
        pytest.param(
            """SELECT d AS "__source_fragment_99999999999999999999__" FROM (VALUES (1.5)) t(d)""",
            1.5,
            id="too-long-for-a-tag",
        ),
        pytest.param(
            """SELECT ({'e__source_fragment_1__': d, 'd__source_fragment_1__': e})."e__source_fragment_1__"
            FROM (VALUES (1.0, 2.0)) t(d, e)""",
            1.0,
            id="a-struct-key",
        ),
    ],
)
def test_outside_decide_an_alias_is_only_a_name(decidb_cli, sql, expected):
    """DECIDE's tags live in aliases, but plain SQL binds with no DECIDE binder: the same text there is a name."""
    assert decidb_cli.execute(sql)[0][0][0] == pytest.approx(expected)


def test_a_norm_of_a_decision_holds_a_subquery(decidb_cli):
    """The objective's least is x = 1 on every row."""
    sql = f"""SELECT id, x FROM {NORM_ROWS} DECIDE x(REAL)
        SUCH THAT x <= 10 MINIMIZE norm(x - (SELECT 1), 1) ORDER BY id"""
    for mode in ("off", "auto"):
        assert [row[1] for row in run(decidb_cli, sql, mode)[0]] == pytest.approx([1] * 4), mode
