"""A clause states each distinct constraint once (language/guide.md §6.2, §7.1).

A clause with no `per` is one row per kept row, but rows that read the same thing are one constraint: when no per-row
decision stands in its signature (a query-wide or a keyed decision is the same on every row of its instance), its
instances are its distinct signatures, as `per ():` makes them, and the model holds one row for each. A clause that
reads a per-row decision differs on every row and keeps a row of its own. Judged by the scope oracle
(`_scope_oracle`), which states every constraint, and by the model's size.
"""

import pytest

from ._scope_cases import answer, const, dump_counts, scope_solver, table, table_rows  # noqa: F401 -- fixture
from ._scope_oracle import CONSTANT, GLOBAL, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense

COLUMNS = ("id", "a", "b", "g", "c")
#: Twelve rows in three groups of `g`, with `c` constant in each group: a bound that agrees between rows.
ROWS = tuple((i, i * 7 % 5 + 1, i * 3 % 4 + 1, f"g{i % 3}", i % 3 + 10) for i in range(1, 13))
RELATION = table("t", COLUMNS, ROWS)

X = lambda r: {"x": 1}  # noqa: E731
X_BOUND = Scoped((X,), "<=", (const(5),))
MAXIMIZE_X = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", X),))


def _a_x(r):
    return {"x": r["a"]}


def _b_x(r):
    return {"x": r["b"]}


def _query(decide, clauses, objective="MAXIMIZE sum(x)"):
    names = ", ".join(declaration.split(":")[-1].split("(")[0].strip() for declaration in decide.split("),"))
    return f"SELECT id, {names} {RELATION} DECIDE {decide} SUCH THAT {clauses} {objective} ORDER BY id"


def _sizes(cli, tmp_path, sql):
    return dump_counts(cli.dump_model(sql, tmp_path / "model.dump"))


def _judged(cli, solver, sql, decisions, clauses, objective=MAXIMIZE_X):
    model = ScopedModel(table_rows(COLUMNS, ROWS), decisions)
    assert model.judge_scoped(answer(cli, sql), clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective), abs=1e-4
    )


@pytest.mark.correctness
def test_a_value_clause_with_no_per_has_the_rows_per_writes(decidb_cli, scope_solver, tmp_path):
    """`sum(a * x) + max(b * x) <= (SELECT 40)` reads no per-row decision outside its reducers, so its twelve rows,
    which are one constraint, build the model `per ():` builds: the clause's row and the definitions of the helpers,
    not twelve copies of the row."""
    bare = "sum(a * x) + max(b * x) <= (SELECT 40)"
    sqls = {written: _query("x(INT)", f"x <= 5 AND {written}") for written in (bare, f"per (): {bare}")}
    sizes = {written: _sizes(decidb_cli, tmp_path, sql) for written, sql in sqls.items()}
    assert sizes[bare] == sizes[f"per (): {bare}"] == {"num_vars": 13, "num_rows": 13}
    clauses = [X_BOUND, Scoped((Reduce("sum", _a_x), Reduce("max", _b_x)), "<=", (const(40),))]
    for sql in sqls.values():
        _judged(decidb_cli, scope_solver, sql, [Decision("x")], clauses)


@pytest.mark.correctness
def test_a_clause_with_no_per_has_one_row_per_distinct_signature(decidb_cli, scope_solver, tmp_path):
    """The reducers' `by (g)` is a field of the clause's signature: the twelve rows are three variants, one per group
    of `g`, and the model holds a row for each beside the helper's definition."""
    sql = _query("x(INT)", "x <= 5 AND sum(a * x) + sum(b * x) by (g) <= (SELECT 40)")
    assert _sizes(decidb_cli, tmp_path, sql) == {"num_vars": 13, "num_rows": 4}
    clauses = [X_BOUND, Scoped((Reduce("sum", _a_x), Reduce("sum", _b_x, by=("g",))), "<=", (const(40),))]
    _judged(decidb_cli, scope_solver, sql, [Decision("x")], clauses)


@pytest.mark.correctness
def test_a_clause_over_query_wide_decisions_has_one_row_per_distinct_bound(decidb_cli, scope_solver, tmp_path):
    """`s + u <= t.c` reads only query-wide decisions, so rows with one `c` are one constraint: the three values of
    `c`, three rows, where each of the twelve rows had its own."""
    sql = _query("x(INT), per (): s(INT), per (): u(INT)", "x <= 5 AND s <= 3 AND u <= 3 AND s + u <= t.c")
    sql = sql.replace("MAXIMIZE sum(x)", "MAXIMIZE sum(x) + s + u")
    assert _sizes(decidb_cli, tmp_path, sql)["num_rows"] == 3
    clauses = [
        X_BOUND,
        Scoped((lambda r: {"s": 1},), "<=", (const(3),)),
        Scoped((lambda r: {"u": 1},), "<=", (const(3),)),
        Scoped((lambda r: {"s": 1, "u": 1},), "<=", (lambda r: {CONSTANT: r["c"]},)),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", X), lambda r: {"s": 1, "u": 1}))
    _judged(
        decidb_cli, scope_solver, sql, [Decision("x"), Decision("s", GLOBAL), Decision("u", GLOBAL)], clauses, objective
    )


@pytest.mark.correctness
def test_a_clause_over_a_per_row_decision_keeps_a_row_per_row(decidb_cli, scope_solver, tmp_path):
    """`x + s <= t.c` reads the per-row `x`, which differs on every row: twelve rows, though `c` takes three values."""
    sql = _query("x(INT), per (): s(INT)", "x <= 5 AND s <= 3 AND x + s <= t.c")
    sql = sql.replace("MAXIMIZE sum(x)", "MAXIMIZE sum(x) + s")
    assert _sizes(decidb_cli, tmp_path, sql)["num_rows"] == len(ROWS)
    clauses = [
        X_BOUND,
        Scoped((lambda r: {"s": 1},), "<=", (const(3),)),
        Scoped((lambda r: {"x": 1, "s": 1},), "<=", (lambda r: {CONSTANT: r["c"]},)),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", X), lambda r: {"s": 1}))
    _judged(decidb_cli, scope_solver, sql, [Decision("x"), Decision("s", GLOBAL)], clauses, objective)


# `per K:` classes that agree on everything the clause reads are one instance, so one row.
KEYED_COLUMNS = ("id", "g", "b")


def _keyed(rows, clause_sql, decide="per (): s(INT), per (): u(INT)"):
    return f"""SELECT id, s, u {table('t', KEYED_COLUMNS, rows)} DECIDE {decide}
        SUCH THAT s <= 3 AND u <= 3 AND {clause_sql} MAXIMIZE s + u ORDER BY id"""


@pytest.mark.correctness
def test_classes_that_agree_on_what_a_clause_reads_are_one_row(decidb_cli, scope_solver, tmp_path):
    """`per t.g: s + u <= t.b` over three classes with one `b` is one constraint, which the model states once."""
    rows = ((1, "a", 5), (2, "b", 5), (3, "c", 5))
    sql = _keyed(rows, "per t.g: s + u <= t.b")
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == 1
    clauses = [
        Scoped((lambda r: {"s": 1},), "<=", (const(3),)),
        Scoped((lambda r: {"u": 1},), "<=", (const(3),)),
        Scoped((lambda r: {"s": 1, "u": 1},), "<=", (lambda r: {CONSTANT: r["b"]},), per=("g",)),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (lambda r: {"s": 1, "u": 1},))
    model = ScopedModel(table_rows(KEYED_COLUMNS, rows), [Decision("s", GLOBAL), Decision("u", GLOBAL)])
    assert model.judge_scoped(answer(decidb_cli, sql), clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )


@pytest.mark.correctness
def test_classes_that_differ_in_a_field_their_key_holds_stay_two_rows(decidb_cli, scope_solver, tmp_path):
    """`per t.g: s <= length(t.g)` reads a function of the key, which the signature leaves out because the key holds it:
    the classes 'a' and 'bb' agree on every field the signature has and are two constraints, not one."""
    rows = ((1, "a", 0), (2, "bb", 0))
    sql = _keyed(rows, "per t.g: s + u <= length(t.g)")
    assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == 2
    length = lambda r: float(len(r["g"]))  # noqa: E731
    clauses = [
        Scoped((lambda r: {"s": 1},), "<=", (const(3),)),
        Scoped((lambda r: {"u": 1},), "<=", (const(3),)),
        Scoped((lambda r: {"s": 1, "u": 1},), "<=", (lambda r: {CONSTANT: length(r)},), per=("g",)),
    ]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (lambda r: {"s": 1, "u": 1},))
    model = ScopedModel(table_rows(KEYED_COLUMNS, rows), [Decision("s", GLOBAL), Decision("u", GLOBAL)])
    assert model.judge_scoped(answer(decidb_cli, sql), clauses, objective) == pytest.approx(
        model.solve_scoped(scope_solver, clauses, objective), abs=1e-4
    )
