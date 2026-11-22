"""One descent over the buffered rows (``pipeline/08_execution/todo.md`` EXEC-23): each thread buffers its rows, Combine
places the buffers one after another, and Finalize routes the whole buffer through one descent, each row numbered by its
place. The tests run on real threads: a Parquet file of several row groups, read by four threads, whose rows reach the
buffer in an order that changes from run to run; the answer is the oracle's whatever the order.
"""

import re

import duckdb
import pytest

from ._scope_cases import scope_solver  # noqa: F401 -- scope_solver is a fixture
from decidb_cli import _RESULT_MODE, _parse_quote_rows, _run
from solver.types import ObjSense, SolverStatus, VarType

#: A scan hands a thread whole row groups, and DuckDB's Parquet writer writes none smaller than 2048 rows: 8400 rows
#: are five groups, so up to four threads each buffer rows of their own.
GROUP_ROWS = 2048
THREAD_ROWS = 8400
THREADS = 4
THREAD_SETUP = f"SET threads = {THREADS}; SET decide_direct_solve = 'off';\n"


def _thread_table(tmp_path):
    """The input in a Parquet file of row groups of 2048, which a query reads back as ``t``, and its rows for the
    oracle, read back from the file."""
    path = tmp_path / "rows.parquet"
    connection = duckdb.connect()
    connection.execute(
        f"""COPY (SELECT i AS id, i % 100 AS part, i // 100 AS p, (i * 7919) % 13 AS w, i % 40 AS g1,
            (i // 40) % 5 AS g2, (i // 200) % 4 AS g3, i % 7 AS k FROM range({THREAD_ROWS}) r(i))
            TO '{path}' (FORMAT parquet, ROW_GROUP_SIZE {GROUP_ROWS})"""
    )
    cursor = connection.execute(f"SELECT * FROM read_parquet('{path}') ORDER BY id")
    names = [column[0] for column in cursor.description]
    rows = [dict(zip(names, row)) for row in cursor.fetchall()]
    connection.close()
    return f"FROM read_parquet('{path}') t", rows


def _by(rows, *keys):
    """The indices of the rows, by the values of `keys`."""
    groups = {}
    for i, row in enumerate(rows):
        groups.setdefault(tuple(row[key] for key in keys), []).append(i)
    return groups


def _variables(solver, prefix, count, lb, ub):
    """One continuous variable per index, `prefix` and the index."""
    for i in range(count):
        solver.add_variable(f"{prefix}{i}", VarType.CONTINUOUS, lb, ub)


def _flat_sum(solver, rows):
    """x <= 10, and per g1 the sum of x at most 400."""
    _variables(solver, "x", len(rows), 0.0, 10.0)
    for group in _by(rows, "g1").values():
        solver.add_constraint({f"x{i}": 1.0 for i in group}, "<=", 400.0)


def _nested_max_in_sum(solver, rows):
    """Per g1, the sum over its rows of the max of x over the row's (g1, g2): a helper above x per (g1, g2), counted
    once per row of it."""
    _variables(solver, "x", len(rows), 0.0, 10.0)
    for g1 in {row["g1"] for row in rows}:
        coefficients = {}
        for (_, g2), group in _by(rows, "g1", "g2").items():
            if _ == g1:
                solver.add_variable(f"m{g1}_{g2}", VarType.CONTINUOUS, 0.0, None)
                for i in group:
                    solver.add_constraint({f"m{g1}_{g2}": 1.0, f"x{i}": -1.0}, ">=", 0.0)
                coefficients[f"m{g1}_{g2}"] = float(len(group))
        solver.add_constraint(coefficients, "<=", 50.0)


def _abs_per_class(solver, rows):
    """|sum of x per g1| <= 5, with x in [-10, 10]."""
    _variables(solver, "x", len(rows), -10.0, 10.0)
    for group in _by(rows, "g1").values():
        solver.add_constraint({f"x{i}": 1.0 for i in group}, "<=", 5.0)
        solver.add_constraint({f"x{i}": 1.0 for i in group}, ">=", -5.0)


def _keyed_decision(solver, rows):
    """x <= y of the row's g1, y <= 7, and per g2 a sum of x too large to bind."""
    _variables(solver, "x", len(rows), 0.0, None)
    for (g1,) in _by(rows, "g1"):
        solver.add_variable(f"y{g1}", VarType.CONTINUOUS, 0.0, 7.0)
    for i, row in enumerate(rows):
        solver.add_constraint({f"x{i}": 1.0, f"y{row['g1']}": -1.0}, "<=", 0.0)
    for group in _by(rows, "g2").values():
        solver.add_constraint({f"x{i}": 1.0 for i in group}, "<=", 100000.0)


def _timelines(rows):
    """Each row's index and those before it in its timeline (the rows of its part, by p), nearest first."""
    for group in _by(rows, "part").values():
        ordered = sorted(group, key=lambda i: rows[i]["p"])
        for position, i in enumerate(ordered):
            yield i, ordered[:position][::-1]


def _frame_point(solver, rows):
    """x <= 10 and x <= x of the previous row in the timeline (0 for the first) + w."""
    _variables(solver, "x", len(rows), 0.0, 10.0)
    for i, earlier in _timelines(rows):
        coefficients = {f"x{i}": 1.0}
        if earlier:
            coefficients[f"x{earlier[0]}"] = -1.0
        solver.add_constraint(coefficients, "<=", float(rows[i]["w"]))


def _frame_range(solver, rows):
    """x <= 10 and x <= the average of x over the 5 previous rows + w; a row the timeline lacks reads 0 (`else 0`)."""
    _variables(solver, "x", len(rows), 0.0, 10.0)
    for i, earlier in _timelines(rows):
        coefficients = {f"x{i}": 1.0}
        for j in earlier[:5]:
            coefficients[f"x{j}"] = -1.0 / 5
        solver.add_constraint(coefficients, "<=", float(rows[i]["w"]))


def _abs_of_keyed_in_class(solver, rows):
    """z per k in [-10, 10]; per g1 the sum, over its rows, of |z - 2|: a helper above both z - 2 and 2 - z per k,
    counted once per row of g1 with that k."""
    for (k,) in _by(rows, "k"):
        solver.add_variable(f"z{k}", VarType.CONTINUOUS, -10.0, 10.0)
        solver.add_variable(f"a{k}", VarType.CONTINUOUS, 0.0, None)
        solver.add_constraint({f"a{k}": 1.0, f"z{k}": -1.0}, ">=", -2.0)
        solver.add_constraint({f"a{k}": 1.0, f"z{k}": 1.0}, ">=", 2.0)
    for group in _by(rows, "g1").values():
        coefficients = {}
        for i in group:
            coefficients[f"a{rows[i]['k']}"] = coefficients.get(f"a{rows[i]['k']}", 0.0) + 1.0
        solver.add_constraint(coefficients, "<=", 50.0)


def _abs_under_two_reducers(solver, rows):
    """x in [0, 3], y per g1 in [-10, 10] with a helper a above |y - 2|; the sum of x + a per g2 at most 2500 and per
    g3 at most 3000."""
    _variables(solver, "x", len(rows), 0.0, 3.0)
    for (g1,) in _by(rows, "g1"):
        solver.add_variable(f"y{g1}", VarType.CONTINUOUS, -10.0, 10.0)
        solver.add_variable(f"a{g1}", VarType.CONTINUOUS, 0.0, None)
        solver.add_constraint({f"a{g1}": 1.0, f"y{g1}": -1.0}, ">=", -2.0)
        solver.add_constraint({f"a{g1}": 1.0, f"y{g1}": 1.0}, ">=", 2.0)
    for key in ("g2", "g3"):
        for group in _by(rows, key).values():
            coefficients = {}
            for i in group:
                coefficients[f"x{i}"] = 1.0
                coefficients[f"a{rows[i]['g1']}"] = coefficients.get(f"a{rows[i]['g1']}", 0.0) + 1.0
            solver.add_constraint(coefficients, "<=", 2500.0 if key == "g2" else 3000.0)


#: The shapes the real-thread test runs: name -> (decisions, constraints, the objective's decision and whether it is
#: weighted by w, the oracle's model). Each reads rows the threads buffered in an order that changes from run to run,
#: and numbers them by their place in the buffer: a G's slots, a G in a G, an F per class, a keyed decision's classes, a
#: frame's rows, an F node of a keyed decision, an F under two reducers. Classes are many and small (g1 of 40, g2 of 5,
#: g3 of 4), so each group of a clause is a few rows.
THREAD_CASES = {
    "flat-sum": ("x(REAL)", "x <= 10 AND per t.g1: sum(x) by (t.g1) <= 400", ("x", True), _flat_sum),
    "nested-max-in-sum": (
        "x(REAL)",
        "x <= 10 AND per t.g1: sum(max(x) by (t.g1, t.g2)) by (t.g1) <= 50",
        ("x", True),
        _nested_max_in_sum,
    ),
    "abs-per-class": (
        "x(REAL)",
        "x <= 10 AND x >= -10 AND abs(sum(x) by (t.g1)) <= 5",
        ("x", True),
        _abs_per_class,
    ),
    "keyed-decision": (
        "x(REAL), per t.g1: y(REAL)",
        "x <= y AND y <= 7 AND per t.g2: sum(x) by (t.g2) <= 100000",
        ("x", True),
        _keyed_decision,
    ),
    "frame-point": (
        "x(REAL)",
        "x <= 10 AND x <= at(previous else 0: x) over (t.p within t.part) + t.w",
        ("x", False),
        _frame_point,
    ),
    "frame-range": (
        "x(REAL)",
        "x <= 10 AND x <= avg(from 5 previous to previous else 0: x) over (t.p within t.part) + t.w",
        ("x", False),
        _frame_range,
    ),
    "abs-of-keyed-in-class": (
        "per t.k: z(REAL)",
        "z <= 10 AND z >= -10 AND per t.g1: sum(abs(z - 2)) by (t.g1) <= 50",
        ("z", True),
        _abs_of_keyed_in_class,
    ),
    "abs-under-two-reducers": (
        "x(REAL), per t.g1: y(REAL)",
        "x <= 3 AND y <= 10 AND y >= -10 AND sum(x + abs(y - 2)) by (t.g2) <= 2500 "
        "AND sum(x + abs(y - 2)) by (t.g3) <= 3000",
        ("x", True),
        _abs_under_two_reducers,
    ),
}


def _answer(cli, sql):
    """One run of `sql` on real threads: its answer rows, as dicts."""
    argv = cli._argv("-cmd", f".mode {_RESULT_MODE}", "-c", f"{THREAD_SETUP}{sql}")
    proc = _run(argv, sql, env=cli._subprocess_env())
    for line in proc.stderr.splitlines():
        assert not line.strip() or line.startswith("Warning:"), proc.stderr
    header = re.search(r"^'", proc.stdout, re.MULTILINE)
    table = _parse_quote_rows(proc.stdout[header.start() :])
    return [dict(zip(table[0], values)) for values in table[1:]]


@pytest.mark.correctness
@pytest.mark.parametrize("name", list(THREAD_CASES))
def test_descents_merged_from_real_threads_reach_the_optimum(decidb_cli, scope_solver, tmp_path, name):
    """The rows of five row groups are read by threads of their own and routed through one descent, each numbered by
    its place in the buffer: the objective the answer reaches is the optimum of the oracle's model."""
    decisions, constraints, (decision, weighted), build = THREAD_CASES[name]
    source, rows = _thread_table(tmp_path)
    objective = "sum(t.w * {0})" if weighted else "sum({0})"
    sql = (
        f"SELECT t.id, {decision} {source} DECIDE {decisions} SUCH THAT {constraints} "
        f"MAXIMIZE {objective.format(decision)} ORDER BY t.id"
    )
    cli = decidb_cli.without_verify_serializer()
    weights = [row["w"] if weighted else 1 for row in rows]
    # The objective's coefficient of each variable: a decision per row carries its row's weight, one per k the weights
    # of its rows.
    coefficients = {}
    for i, row in enumerate(rows):
        name_of = f"{decision}{i}" if decision == "x" else f"z{row['k']}"
        coefficients[name_of] = coefficients.get(name_of, 0.0) + weights[i]
    scope_solver.create_model(name)
    build(scope_solver, rows)
    scope_solver.set_objective(coefficients, ObjSense.MAXIMIZE)
    expected = scope_solver.solve()
    assert expected.status == SolverStatus.OPTIMAL, expected.status
    result = _answer(cli, sql)
    assert [row["id"] for row in result] == [row["id"] for row in rows]
    reached = sum(weight * answered[decision] for weight, answered in zip(weights, result))
    assert reached == pytest.approx(expected.objective_value, abs=1e-4, rel=1e-6)
