"""A descent per thread, merged in Combine (``pipeline/08_execution/done.md``): each thread's descent reads its own
rows, numbered from its first, and DecideDescent::Merge takes it into the operator's at the rows placed before. The
merged containers are those one descent over every row would fill, so the model and every message are the same.

``SET decide_debug = 'split=n'`` merges every ``n`` rows as a thread of its own would; each case compares a query
under it with the query read by one descent, and names the part of Merge it reaches. The last tests run on real
threads: a Parquet file of several row groups, read by four threads, each with a descent of its own.
"""

import json
import re

import duckdb
import pytest

from . import _diagnose_relation
from ._scope_cases import scope_solver, values  # noqa: F401 -- scope_solver is a fixture
from decidb_cli import _RESULT_MODE, _parse_quote_rows, _run
from solver.types import ObjSense, SolverStatus, VarType

SETUP = "SET threads = 1; SET decide_direct_solve = 'off';\n"

# Row 6's g is NULL: it lies in no class of t.g, and its group of `by (t.g)` in none.
ROWS = (
    (1, "a", "k1", 1, 4),
    (2, "b", "k2", 1, 6),
    (3, "a", "k1", 2, 1),
    (4, "c", "k3", 1, 7),
    (5, "b", "k2", 2, 3),
    (6, None, "k1", 3, 2),
)
SOURCE = f"FROM {values(ROWS, 't', ('id', 'g', 'k', 'period', 'v'))}"
COLLATED_ROWS = ((1, "b"), (2, "B"), (3, "b"), (4, "c"))
COLLATED = f"FROM (SELECT id, g COLLATE NOCASE AS g FROM {values(COLLATED_ROWS, 'v', ('id', 'g'))}) t"

#: Each query reaches a part of Merge no other case reaches.
MODEL_CASES = {
    # A keyed decision's classes: a later piece finds 'a' and 'b' again, makes 'c' and reads none on row 6
    # (class_map, known, new or none), and its G a slot in no class there (in_a_class).
    "keyed-classes": f"""SELECT id, x, y {SOURCE} DECIDE x(INT), per t.g: y(INT)
        SUCH THAT x <= 9 AND x <= y AND y <= 5 AND per t.g: sum(x) by (t.g) <= 7 MAXIMIZE sum(x) + sum(y) ORDER BY id""",
    # A set E's instances of a decision per row, rebased by the row, and of a keyed one, by its scope's classes.
    "a-set-e-of-decisions": f"""SELECT id, x {SOURCE} DECIDE x(INT), per t.g: y(INT)
        SUCH THAT y <= 3 AND x <= 9 AND per t.id: x + y <= 9 MAXIMIZE sum(x) + sum(y) ORDER BY id""",
    # An E within a G's slot, found by its enclosing slot and key (group_maps).
    "a-class-lies-in-the-group-it-feeds": f"""SELECT id, x {SOURCE} DECIDE x(INT)
        SUCH THAT x <= 9 AND x <= sum(per t.k: t.v) by (t.g) MAXIMIZE sum(t.id * x) ORDER BY id""",
    # An E of an empty key within a G's slot, one per slot (empty_key_slots, by its enclosing slot).
    "an-empty-key-in-a-group": f"""SELECT id, x {SOURCE} DECIDE x(INT)
        SUCH THAT x <= 9 AND x <= sum(per (): t.v) by (t.g) MAXIMIZE sum(x) ORDER BY id""",
    # A set E reads an O's `else` at the anchor's own row, which is rebased (rebase_read, fallback).
    "an-else-read-at-its-row": f"""SELECT id, x {SOURCE} DECIDE x(REAL)
        SUCH THAT x <= 9 AND per t.id: x <= at(previous else 0.5: x) over (t.period within t.k) + 2
        MAXIMIZE sum(x) ORDER BY id""",
    # An O two reducers' operands read, whose rows arrive in the slots of both (MIXED_ENCLOSING).
    "an-o-two-reducers-read": f"""SELECT id, x {SOURCE} DECIDE x(INT)
        SUCH THAT x <= 9 AND sum(x + max(at(previous: t.v) over (t.period within t.k)) by (t.k)) <= 40
        MAXIMIZE sum(x) ORDER BY id""",
    # A collated group's spelling, 'b' in the first piece and 'B' in a later one: the least, folded; 'c' is first
    # spelled in a later piece (DecideSpelling::Fold).
    "a-collated-group": f"""SELECT id, x {COLLATED} DECIDE x(BOOL)
        SUCH THAT per t.g: sum(x) by (t.g) <= 1 MAXIMIZE sum(x) ORDER BY id""",
    # An F node per class, ABS of a group's sum: an entry per row, each an instance of its own, merged as a G's
    # entries are (IsSetNode). It was merged as a set E's, whose reads it never recorded, and crashed.
    "an-f-node-per-class": f"""SELECT id, x {SOURCE} DECIDE x(INT)
        SUCH THAT x <= 3 AND abs(sum(x) by (t.g) - 3) <= 2 MAXIMIZE sum(x) ORDER BY id""",
    # A query-wide Boolean times a query-wide sum: an F node of one instance.
    "a-query-wide-product": f"""SELECT id, x {SOURCE} DECIDE x(INT), per (): b(BOOL)
        SUCH THAT x <= 3 AND b * sum(x) <= 10 MAXIMIZE sum(x) + 5 * b ORDER BY id""",
    # A query-wide factor, a scalar subquery's value, read on the input's first row: the first piece's
    # (MergeDecideDescent).
    "a-query-wide-factor": f"""SELECT id, x {SOURCE} DECIDE x(INT), per (): c(INT)
        SUCH THAT x <= 9 AND c <= 3 AND c * (SELECT max(w) FROM (VALUES (4), (7)) s(w)) + sum(x) <= 60
        MAXIMIZE sum(x) + c ORDER BY id""",
}


def _debug(items):
    """The setup that sets `decide_debug` to `items`."""
    return f"{SETUP}SET decide_debug = '{items}';\n"


def _containers(cli, sql, items=""):
    """The descent's containers once every row is read (DecideDescent::Dump), under the `decide_debug` items `items`
    beside `containers`."""
    result = cli.execute_raw(_debug(",".join(["containers", items] if items else ["containers"])) + sql)
    return result.stderr.split("DECIDB_CONTAINERS_BEGIN\n", 1)[1].split("DECIDB_CONTAINERS_END", 1)[0]


@pytest.mark.parametrize("sql", list(MODEL_CASES.values()), ids=list(MODEL_CASES))
def test_merged_descents_fill_the_containers_one_descent_fills(decidb_cli, tmp_path, sql):
    """Every slot, class, entry, instance, read and routed row, numbered as one descent numbers them, and so the
    model."""
    cli = decidb_cli.without_verify_serializer()
    whole = _containers(cli, sql)
    model = cli.dump_model(SETUP + sql, tmp_path / "whole.dump")
    for rows in (1, 2):
        assert _containers(cli, sql, f"split={rows}") == whole, rows
        assert cli.dump_model(_debug(f"split={rows}") + sql, tmp_path / f"split{rows}.dump") == model, rows


#: An F node holding its definition roots in its own slots, which hold another F: an L0 indicator per class over an
#: ABS per class. Its slots are merged after those of the node they lie in (merge_after_enclosing); merged first, the
#: inner F read the outer's slots before they were found, and crashed.
F_ENCLOSES_F = f"""SELECT id, x, y {SOURCE} DECIDE x(INT), per t.k: y(INT)
    SUCH THAT x <= 3 AND y <= 3 AND norm(y - 1, 0) <= 1 MAXIMIZE sum(x) + sum(per t.k: y) ORDER BY id"""


def test_an_f_node_in_an_f_nodes_slots_merges_as_its_pieces_were_read(decidb_cli, tmp_path):
    """The inner F is reached along several paths within the outer's slots, so one descent numbers its slots path by
    path within a piece of rows: merged pieces number them as one descent reading the same pieces does, and the
    model is the one every row read at once gives."""
    cli = decidb_cli.without_verify_serializer()
    model = cli.dump_model(SETUP + F_ENCLOSES_F, tmp_path / "whole.dump")
    for rows in (1, 2):
        assert _containers(cli, F_ENCLOSES_F, f"split={rows}") == _containers(cli, F_ENCLOSES_F, f"chunk={rows}"), rows
        assert cli.dump_model(_debug(f"split={rows}") + F_ENCLOSES_F, tmp_path / f"split{rows}.dump") == model, rows


def test_a_collated_group_is_spelled_by_its_least_spelling_in_any_piece(decidb_cli):
    """The group's first piece spells it 'b', a later one 'B': the message names 'B' (DecideSpelling::Fold of a
    merged descent's spellings)."""
    rows = ((1, "b"), (2, "B"), (3, "b"))
    sql = f"""SELECT id, x FROM (SELECT id, g COLLATE NOCASE AS g FROM {values(rows, 'v', ('id', 'g'))}) t
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) >= 4 MAXIMIZE SUM(x)"""
    found = _diagnose_relation.rows(
        _diagnose_relation.run(decidb_cli, sql, setup="SET decide_debug = 'split=1';\n", scope="expanded")
    )
    assert [row["group"] for row in found if row["edit_source"] == "expanded_group"] == ["t.g = 'B'"], found


def test_an_error_ranks_at_its_rebased_row(decidb_cli):
    """Rows 2100 and 4100 fail to cast, in pieces of 2048 rows: the first raised ranks first. Unrebased, row 4100's
    piece would place it in the input's first window, before row 2100's (DecideErrorLedger::Rebase)."""
    sql = """SELECT i, x FROM (SELECT i, CASE i WHEN 2100 THEN 'first' WHEN 4100 THEN 'second' ELSE '1' END AS s
        FROM range(4200) r(i)) t
        DECIDE x(INT) SUCH THAT x <= 1 AND sum(x) by (CAST(t.s AS INT)) <= 10 MAXIMIZE sum(x)"""
    cli = decidb_cli.without_verify_serializer()
    whole = cli.execute_raw(SETUP + sql)
    split = cli.execute_raw(_debug("split=2048") + sql)
    assert "'first'" in whole.stderr, whole.stderr
    assert split.stderr == whole.stderr


# ---------------------------------------------------------------------------
# Real threads
# ---------------------------------------------------------------------------

#: A scan hands a thread whole row groups, and DuckDB's Parquet writer writes none smaller than 2048 rows: 8400 rows
#: are five groups, so up to four threads each fill a descent of their own, which Finalize merges.
GROUP_ROWS = 2048
THREAD_ROWS = 8400
THREADS = 4
THREAD_SETUP = f"SET threads = {THREADS}; SET decide_direct_solve = 'off';\n"
#: Runs of a thread case before it fails for want of a merge: on a busy machine one thread can read every row group
#: before the others start, and that run merges nothing.
MERGE_RUNS = 5


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


#: The shapes of ``context/opa_rewrite/tools/threads_test.py``: name -> (decisions, constraints, the objective's
#: decision and whether it is weighted by w, the oracle's model). Each reaches a part of Merge that the threads'
#: descents number differently: a G's slots, a G in a G, an F per class, a keyed decision's classes, a frame's rows, an
#: F node of a keyed decision, an F under two reducers. Classes are many and small (g1 of 40, g2 of 5, g3 of 4), so
#: each group of a clause is a few rows.
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


def _answer_and_merged_descents(cli, sql):
    """One run of `sql` under DECIDB_PROFILE: its answer rows, as dicts, and how many descents the operator merged in
    that run (``execution.descent_parts``): one per thread that read rows."""
    profiled = cli.with_env({"DECIDB_PROFILE": "1"})
    argv = profiled._argv("-cmd", f".mode {_RESULT_MODE}", "-c", f"{THREAD_SETUP}{sql}")
    proc = _run(argv, sql, env=profiled._subprocess_env())
    parts = []
    for line in proc.stderr.splitlines():
        if line.startswith("DECIDB_PROFILE: "):
            event = json.loads(line[len("DECIDB_PROFILE: ") :])
            if event.get("event") == "counter" and event.get("name") == "execution.descent_parts":
                parts.append(event["value"])
        else:
            assert not line.strip() or line.startswith("Warning:"), proc.stderr
    assert len(parts) == 1, proc.stderr
    header = re.search(r"^'", proc.stdout, re.MULTILINE)
    table = _parse_quote_rows(proc.stdout[header.start() :])
    return [dict(zip(table[0], values)) for values in table[1:]], parts[0]


@pytest.mark.correctness
@pytest.mark.parametrize("name", list(THREAD_CASES))
def test_descents_merged_from_real_threads_reach_the_optimum(decidb_cli, scope_solver, tmp_path, name):
    """The rows of five row groups are read by threads of their own, each filling a descent, which Finalize merges:
    within MERGE_RUNS runs one merges more than one descent, and the objective every run's answer reaches is the
    optimum of the oracle's model."""
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
    # One run gives the answer and the count, so the answer checked last is the merged one.
    for _ in range(MERGE_RUNS):
        result, merged = _answer_and_merged_descents(cli, sql)
        assert [row["id"] for row in result] == [row["id"] for row in rows]
        reached = sum(weight * answered[decision] for weight, answered in zip(weights, result))
        assert reached == pytest.approx(expected.objective_value, abs=1e-4, rel=1e-6)
        if merged > 1:
            break
    assert merged > 1, f"none of {MERGE_RUNS} runs merged more than one descent"
