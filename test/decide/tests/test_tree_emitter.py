"""The tree's model emitter (language/guide.md §6.4): under DECIDB_EVALUATOR=tree a query is evaluated by the tree --
its programs computed over its nodes' entries -- and formulated through flat's code path. Each test names the branch
it reaches."""

import json
import os
import re
import subprocess

import pytest

from ._expected import expected

ROWS = "(VALUES (1, 'a', 3.0::DOUBLE), (2, 'a', 4.0::DOUBLE), (3, 'b', 5.0::DOUBLE)) t(id, g, c)"
#: Every clause in the per-row shape and no objective: the shapes with an arm.
ROWS_ONLY = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= t.c AND x + 1 >= 2"
PLAIN = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= t.c MAXIMIZE sum(x)"


def run(cli, sql, evaluator="tree", **env):
    """The query under `evaluator`, in a process of its own: its stderr is read whole."""
    environment = {**os.environ, **(cli.env or {}), "DECIDB_EVALUATOR": evaluator, **env}
    return subprocess.run(
        [cli.exe, "-c", "SET threads=1; SET decide_direct_solve='off'; " + sql],
        env=environment,
        capture_output=True,
        text=True,
        timeout=120,
    )


def counters(result, name):
    """The values of the DECIDB_PROFILE counter `name` a run printed."""
    values = []
    for line in result.stderr.splitlines():
        if line.startswith("DECIDB_PROFILE: "):
            event = json.loads(line[len("DECIDB_PROFILE: ") :])
            if event.get("event") == "counter" and event.get("name") == name:
                values.append(event["value"])
    return values


#: What a stored expectation is not keyed on: the solver, which it is keyed on apart, and switches that change no output.
_UNKEYED = ("DECIDB_FORCE_SOLVER", "DECIDB_EVALUATOR", "DECIDB_TEST_DIRECT_SOLVE", "DECIDB_VERIFY_SERIALIZER")


def _keyed(cli, env):
    return {k: v for k, v in {**(cli.env or {}), **env}.items() if k not in _UNKEYED}


def models(cli, sql, tmp_path, warnings=()):
    """The model the query's stored expectation holds, flat's when it was recorded (_expected.py), and the one the tree
    builds, each printing `warnings`."""

    def dump(evaluator):
        path = tmp_path / f"{evaluator}.dump"
        return cli.with_env({"DECIDB_EVALUATOR": evaluator}).dump_model(sql, path, expect_warnings=warnings)

    key = _keyed(cli, {"warnings": ",".join(warnings)})
    return [expected("model", sql, key, cli.warning_backend(), lambda: dump("flat")), dump("tree")]


def run_model(cli, sql, tmp_path):
    """The model the tree builds for the query."""
    return cli.with_env({"DECIDB_EVALUATOR": "tree"}).dump_model(sql, tmp_path / "tree.dump")


def reference(cli, sql, **env):
    """What the query printed when its expectation was recorded, flat's run (_expected.py), as `run` returns it."""

    def produce():
        result = run(cli, sql, "flat", **env)
        return {"returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr}

    value = expected("run", sql, _keyed(cli, env), cli.warning_backend(), produce)
    return subprocess.CompletedProcess([], value["returncode"], value["stdout"], value["stderr"])


def test_a_query_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path):
    """The tree emits the clauses through flat's code path. Its descent counts the rows it read."""
    result = run(decidb_cli, ROWS_ONLY, DECIDB_PROFILE="1")
    assert result.returncode == 0, result.stderr
    assert counters(result, "execution.descent_rows") == [3], result.stderr
    stored, tree = models(decidb_cli, ROWS_ONLY, tmp_path)
    assert tree == stored


def test_a_null_coefficient_skips_its_row(decidb_cli, tmp_path):
    """A NULL coefficient on a kept row leaves its instance with no value (D67): row 2 has no model row, y's term
    included."""
    sql = """SELECT id, x, y FROM (VALUES (1, 2.0), (2, NULL), (3, 4.0)) t(id, w)
        DECIDE x(REAL), y(REAL) SUCH THAT x <= 10 AND t.w * x + y <= 8"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_keyed_decision_with_no_class_skips_its_row(decidb_cli, tmp_path):
    """A decision declared `per t.g` has no instance at a row whose g is NULL (D84): the row reads none."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 3.0), (2, NULL, 4.0), (3, 'b', 5.0)) t(id, g, c)
        DECIDE per t.g: x(INT) SUCH THAT x <= 10 AND x + 0 * t.c <= t.c"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_rows_in_no_class_share_one_absent_class(decidb_cli, tmp_path):
    """The second row with a NULL g joins the class the first opened (KeyedMapping::absent_class): x has the
    instances flat gives it, one per class and one for both rows."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 3.0), (2, NULL, 4.0), (3, NULL, 5.0), (4, 'b', 6.0)) t(id, g, c)
        DECIDE per t.g: x(INT) SUCH THAT x <= 10 AND x + 0 * t.c <= t.c"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


#: Each case raises flat's error, which the rank of the tree's errors decides among those it holds.
RAISED = {
    # Clause 0's bound (NaN on row 2) before clause 1's coefficient (NaN on row 1): clauses rank in flat's order.
    "a-clause-before-the-next": (
        "SELECT id, x FROM (VALUES (1, 'NaN'::DOUBLE, 1.0), (2, 1.0, 'NaN'::DOUBLE)) t(id, a, b) "
        "DECIDE x(REAL) SUCH THAT x + 1 <= t.b AND t.a * x <= 9",
        "the bound -1 + t.b is NaN on some rows",
    ),
    # Term 1's NaN on row 10, in the first window of 2,048 rows, before term 0's on row 2,500: a window's every term
    # is read before the next window's.
    "a-window-before-the-next": (
        "SELECT id, x FROM (SELECT i AS id, CASE WHEN i = 2500 THEN 'NaN'::DOUBLE ELSE 1.0 END AS a, "
        "CASE WHEN i = 10 THEN 'NaN'::DOUBLE ELSE 1.0 END AS b FROM range(3000) r(i)) t "
        "DECIDE x(REAL), y(REAL) SUCH THAT t.a * x + t.b * y <= 10",
        "t.b is NaN or infinite on some rows",
    ),
    # Term 1's NaN on row 1,000 before term 0's on row 3,000, although the `when` puts both in the clause's first
    # 2,048 entries: a window is flat's, of the input's rows, which the failing entry's row places.
    "a-window-of-rows-not-of-entries": (
        "SELECT id, x FROM (SELECT i AS id, CASE WHEN i = 3000 THEN 'NaN'::DOUBLE ELSE 1.0 END AS a, "
        "CASE WHEN i = 1000 THEN 'NaN'::DOUBLE ELSE 1.0 END AS b FROM range(5000) r(i)) t "
        "DECIDE x(REAL), y(REAL) SUCH THAT x <= 9 AND when t.id % 2 = 0: t.a * x + t.b * y <= 10",
        "t.b is NaN or infinite on some rows",
    ),
    # Term 0's NaN on row 3 before term 1's on row 2: in a window, the terms are read in their order.
    "a-term-before-the-next": (
        "SELECT id, x FROM (VALUES (1, 1.0, 1.0), (2, 1.0, 'NaN'::DOUBLE), (3, 'NaN'::DOUBLE, 1.0)) t(id, a, b) "
        "DECIDE x(REAL), y(REAL) SUCH THAT t.a * x + t.b * y <= 10",
        "t.a is NaN or infinite on some rows",
    ),
    # Term 1's logarithm of 0 on row 3 before term 0's NaN on row 2: a window computes every term before it reads one.
    "an-execution-before-a-value": (
        "SELECT id, x FROM (VALUES (1, 1.0, 1.0), (2, 'NaN'::DOUBLE, 1.0), (3, 1.0, 0.0)) t(id, a, w) "
        "DECIDE x(REAL), y(REAL) SUCH THAT t.a * x + ln(t.w) * y <= 10",
        "cannot take logarithm of zero",
    ),
    # The coefficient's NaN on row 2 before the bound's on row 1: every term is read before the bound.
    "a-term-before-its-bound": (
        "SELECT id, x FROM (VALUES (1, 1.0, 'NaN'::DOUBLE), (2, 'NaN'::DOUBLE, 1.0)) t(id, a, b) "
        "DECIDE x(REAL) SUCH THAT t.a * x <= t.b",
        "t.a is NaN or infinite on some rows",
    ),
    # The clause's `when` (row 2) before its coefficient (row 1): what routes the rows ranks before what they read.
    "a-when-before-its-terms": (
        "SELECT id, x FROM (VALUES (1, 4.0, 0.0), (2, -1.0, 2.0)) t(id, c, w) "
        "DECIDE x(REAL) SUCH THAT x <= 9 AND when ln(t.c) > 0: ln(t.w) * x <= 100",
        "cannot take logarithm of a negative number",
    ),
    # The first entry that raises, row 2's zero before row 3's negative: a window that raises is read entry by entry.
    "the-first-failing-entry": (
        "SELECT id, x FROM (VALUES (1, 2.0), (2, 0.0), (3, -1.0)) t(id, w) "
        "DECIDE x(REAL) SUCH THAT x <= 9 AND ln(t.w) * x <= 100",
        "cannot take logarithm of zero",
    ),
    # The clause's `when` (row 2,500, in the second window) before a reducer's member test (row 10, in the first): the
    # clause's `when` routes its rows, so it ranks before every filter, whatever the windows (DecideNode::filter_group
    # is a reducer's alone).
    "a-clauses-when-before-a-reducers-filter": (
        "SELECT id, x FROM (SELECT range AS id, range % 2 AS g, CASE WHEN range = 2500 THEN -1.0 ELSE 2.0 END AS a, "
        "CASE WHEN range = 10 THEN -1.0 ELSE 2.0 END AS w FROM range(3000)) t "
        "DECIDE x(REAL), per t.g: y(REAL) SUCH THAT x <= 1 AND y <= 1 AND "
        "when sqrt(t.a) > -1: sum(ln(t.w) * x) by (t.g) + 2 * y <= 5 MAXIMIZE sum(x)",
        "cannot take square root of a negative number",
    ),
    # A coefficient DeciDB computed past a DOUBLE from finite values names its clause.
    "a-computed-coefficient-past-a-double": (
        "SELECT id, x FROM (VALUES (1, 1e308, 1e308)) t(id, a, b) DECIDE x(REAL) SUCH THAT t.a * x + t.b * x <= 5",
        "t.a * x + t.b * x <= 5 computes a number too large for a DOUBLE",
    ),
    # A coefficient DeciDB computed (t.a + t.b) names the value it is computed from that is not finite there.
    "a-computed-coefficients-input": (
        "SELECT id, x FROM (VALUES (1, 2.0, 1.0), (2, 1.0, 'NaN'::DOUBLE)) t(id, a, b) "
        "DECIDE x(REAL) SUCH THAT t.a * x + t.b * x <= 5",
        "t.b is NaN or infinite on some rows",
    ),
}


@pytest.mark.parametrize("case", list(RAISED))
def test_the_tree_raises_flats_error(decidb_cli, case):
    sql, message = RAISED[case]
    stored = reference(decidb_cli, sql)
    tree = run(decidb_cli, sql)
    assert message in stored.stderr, stored.stderr
    assert tree.stderr == stored.stderr


#: Clause 0 holds no value on any row, which flat reports once its clauses are read, unless one of them raised.
NOTHING = (
    "SELECT id, x FROM (VALUES (1, NULL::DOUBLE, 1.0), (2, NULL, {d})) t(id, n, d) "
    "DECIDE x(REAL) SUCH THAT x + 2 <= t.n AND x + 1 <= t.d"
)


@pytest.mark.parametrize("d,warned", [("2.0", True), ("'NaN'::DOUBLE", False)], ids=["no-error", "a-raised-error"])
def test_skipped_instances_are_reported_unless_a_clause_raised(decidb_cli, d, warned):
    result = run(decidb_cli, NOTHING.format(d=d))
    assert ("constrains nothing" in result.stderr) == warned, result.stderr
    assert result.stderr == reference(decidb_cli, NOTHING.format(d=d)).stderr


def test_a_folded_bounds_repair_rows_are_emitted_under_diagnose(decidb_cli):
    """`x >= 4` and `x <= 2` are folded into x's box; only the repair model has their rows."""
    sql = """DIAGNOSE SELECT id, x FROM (VALUES (1, 3.0), (2, 4.0)) t(id, c)
        DECIDE x(INT), y(INT) SUCH THAT x >= 4 AND x <= 2 AND x + y <= 20"""
    stored = reference(decidb_cli, sql)
    assert "x >= 4" in stored.stdout, stored.stdout
    assert run(decidb_cli, sql).stdout == stored.stdout


@pytest.mark.parametrize("c", ["5.0", "NULL"], ids=["a-bound-on-every-row", "a-null-bound"])
def test_an_admitted_bound_folds_flats_box(decidb_cli, tmp_path, c):
    """`x <= t.c` admits a range of x on each row, which the model builder folds into x's column; a NULL bound leaves
    its row with no value."""
    sql = f"SELECT id, x FROM (VALUES (1, 3.0), (2, {c}), (3, 4.0)) t(id, c) DECIDE x(REAL) SUCH THAT x <= t.c AND x + 1 >= 2"
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_null_in_a_later_window_marks_its_own_entry(decidb_cli, tmp_path):
    """Row 2,500's NULL bound is read in the second window of 2,048 entries, which marks the entry at its offset in the
    node, not in the window: row 2,500 has no value, and row 452 keeps its range."""
    sql = """SELECT id, x FROM (SELECT i AS id, CASE WHEN i = 2500 THEN NULL ELSE 5.0 END AS c FROM range(3000) r(i)) t
        DECIDE x(REAL) SUCH THAT x <= t.c AND x + 1 >= 2"""
    model = run_model(decidb_cli, sql, tmp_path)
    bounds = dict(re.findall(r"(?m)^col (\d+): lb=1 ub=(\S+) ", model))
    assert len(bounds) == 3000 and bounds.pop("2500") == "1e+30", model[:300]
    assert set(bounds.values()) == {"5"}, model[:300]


def test_a_folded_bound_with_no_repair_rows_is_no_clause(decidb_cli, tmp_path):
    """A BOOLEAN's bound is its box, with no rows even for a repair model: its root has no clause to emit."""
    sql = """SELECT id, x, b FROM (VALUES (1, 2.0), (2, 5.0)) t(id, c)
        DECIDE x(REAL), b(BOOL) SUCH THAT x + 1 <= t.c AND b <= 1 AND x + b <= 8"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_covered_query_prints_each_dump_it_is_asked_for(decidb_cli):
    """The bottom-up values are the columns' (DecideTreeColumns::Dump): each node's programs on its entries."""
    dumps = {
        "DECIDB_DUMP_TREE": "1",
        "DECIDB_DUMP_BODIES": "1",
        "DECIDB_DUMP_CONTAINERS": "1",
        "DECIDB_DUMP_VALUES": "1",
    }
    result = run(decidb_cli, ROWS_ONLY, **dumps)
    for dump in ("TREE", "BODIES", "CONTAINERS", "VALUES"):
        assert result.stderr.count(f"DECIDB_{dump}_BEGIN") == 1, result.stderr
    assert " x <= t.c\n  row 0 -> " in result.stderr, result.stderr


def test_a_linear_objective_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path):
    """The objective's arm: each term a column over the members of the G it ranges over."""
    stored, tree = models(decidb_cli, PLAIN, tmp_path)
    assert tree == stored


#: Rows 2 and 3 have p, so the objective keeps them; c is NULL on row 3, and the groups of g hold 2 rows and 1.
OBJECTIVE_ROWS = (
    "(VALUES (1, 'a', 3.0::DOUBLE, false), (2, 'a', 4.0::DOUBLE, true), (3, 'b', NULL, true)) t(id, g, c, p)"
)

#: Each a model flat builds and the tree builds the same, by the branch it reaches.
OBJECTIVES = {
    # y stands outside every reducer: read on each row the objective keeps, at the entry standing for them, and
    # counted once, on the first of them, row 2 (CountTupleTermsOnce).
    "a-term-outside-every-reducer": "when t.p: sum(t.c * x) + y",
    # AVG divides by the rows its reducer reads, which its member test `c IS NOT NULL` keeps: 1/2, not 1/3.
    "an-avg": "avg(t.c * x)",
    # An AVG of a `by` key divides by its own group's members (D81): 1/2 in group a and 1/1 in group b.
    "an-avg-of-a-group": "sum(per t.g: avg(x) by (t.g))",
    # t.c is a term of no decision, which the objective reads and drops: it is absent nowhere
    # (DecideTreeColumns::Absent).
    "a-constant-term": "sum(x + t.c)",
}


@pytest.mark.parametrize("case", list(OBJECTIVES))
def test_an_objective_reads_the_rows_flat_reads(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x, y FROM {OBJECTIVE_ROWS}
        DECIDE x(REAL), per (): y(REAL) SUCH THAT x <= 9 AND y <= 4 MAXIMIZE {OBJECTIVES[case]}"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_qualified_reducer_reads_one_row_per_class(decidb_cli, tmp_path):
    """`sum(per t.g: y)` reads y once per class of t.g, at its first row: rows 1 and 3, the rows flat's qualifier
    keeps."""
    sql = (
        f"SELECT id, x, y FROM {ROWS} DECIDE x(REAL), per t.g: y(REAL) SUCH THAT x + y <= t.c MAXIMIZE sum(per t.g: y)"
    )
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_member_test_keeps_its_terms_from_being_read(decidb_cli, tmp_path):
    """Row 2's `t.v IS NOT NULL` fails, so flat reads no term of the reducer there, and its overflowing coefficient
    raises nothing."""
    sql = """SELECT id, x FROM (VALUES (1, 2::BIGINT, 1::BIGINT, 1.0),
        (2, 4611686018427387904::BIGINT, 4::BIGINT, NULL), (3, 3::BIGINT, 2::BIGINT, 2.0)) t(id, a, b, v)
        DECIDE x(REAL) SUCH THAT x <= 9 MAXIMIZE sum(t.a * t.b * x + t.v * x)"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_member_test_that_raises_raises_flats_error(decidb_cli):
    """The test `ln(t.w) * t.v IS NOT NULL` raises on row 2, which its reducer reads: flat's message."""
    sql = """SELECT id, x FROM (VALUES (1, 2.0, 1.0), (2, 0.0, NULL), (3, 3.0, 2.0)) t(id, w, v)
        DECIDE x(REAL) SUCH THAT x <= 9 MAXIMIZE sum(ln(t.w) * t.v * x)"""
    stored = reference(decidb_cli, sql)
    assert "cannot take logarithm of zero" in stored.stderr, stored.stderr
    assert run(decidb_cli, sql).stderr == stored.stderr


#: Flat reads the objective's `when` over every row, then each term's filter, its reducer's member tests among them,
#: window by window and in term order, then the coefficients. Each case raises flat's error among a member test's and
#: another read: row 1's `w` is -1 and row 2's `a` is 0.
OBJECTIVE_RAISED = {
    # The objective's `when` (row 2) before a member test (row 1): the rows are routed before any filter is read.
    "a-when-before-a-member-test": "when ln(t.a) > -100: sum(ln(t.w) * t.v * x)",
    # Term 0's member test (row 2) before term 1's (row 1): a window reads the filters in term order.
    "a-member-test-before-the-next-terms": "sum(ln(t.a) * t.v * x) + sum(ln(t.w) * t.v * x)",
}


@pytest.mark.parametrize("case", list(OBJECTIVE_RAISED))
def test_a_member_test_raises_in_flats_order(decidb_cli, case):
    sql = f"""SELECT id, x FROM (VALUES (1, 2.0, -1.0, 1.0), (2, 0.0, 2.0, 1.0)) t(id, a, w, v)
        DECIDE x(REAL) SUCH THAT x <= 1 MAXIMIZE {OBJECTIVE_RAISED[case]}"""
    stored = reference(decidb_cli, sql)
    assert "cannot take logarithm of zero" in stored.stderr, stored.stderr
    tree = run(decidb_cli, sql)
    assert tree.stderr == stored.stderr


#: Row 2's g is NULL, so it reads no instance of y, declared per t.g. The objective keeps row 2 alone, so y, outside
#: every reducer, is read on no row, and flat drops its term (D84).
ABSENT = {
    # y's term is read on no entry (DecideTreeColumns::ReadEntries).
    "a-term-with-no-instance": "3.0::DOUBLE",
    # Nor is its coefficient read there, where it is NaN: Numbers reads only the entries the term is read on.
    "a-coefficient-it-never-reads": "'NaN'::DOUBLE",
}


@pytest.mark.parametrize("case", list(ABSENT))
def test_a_term_reading_no_instance_is_dropped(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x, y FROM (VALUES (1, 'a', 3.0::DOUBLE), (2, NULL, {ABSENT[case]})) t(id, g, c)
        DECIDE x(REAL), per t.g: y(REAL) SUCH THAT x <= 1 AND y <= 2 MAXIMIZE when t.id = 2: sum(x) + t.c * y"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_an_objective_produced_twice_raises_flats_error(decidb_cli):
    """t.c, outside every reducer, takes two values over the kept rows: the objective has two instances (D72)."""
    sql = f"""SELECT id, x FROM {ROWS} DECIDE x(REAL), per (): y(REAL)
        SUCH THAT x <= 9 AND y <= 4 MAXIMIZE sum(x) + t.c * y"""
    stored = reference(decidb_cli, sql)
    assert "the objective would be produced more than once" in stored.stderr, stored.stderr
    assert run(decidb_cli, sql).stderr == stored.stderr


#: Rows for the aggregate shape: group a holds rows 1 and 2, group b rows 3, 5 and 6, and row 4's g is NULL, so it is in
#: no group and has no y. Row 1's k is NULL, so it is in no class of t.k, though it is group a's first row; class v
#: holds rows 3 and 6 of group b, with two values of c.
GROUPED_ROWS = (
    "(VALUES (1, 'a', NULL, 3.0::DOUBLE, true), (2, 'a', 'u', 4.0, false), (3, 'b', 'v', 5.0, true), "
    "(4, NULL, 'v', 6.0, true), (5, 'b', 'u', 7.0, false), (6, 'b', 'v', 8.0, false)) t(id, g, k, c, p)"
)

#: Each a clause in the aggregate shape, whose model the tree builds as flat builds it, by the branch it reaches
#: (DecideModelEmitter::Groups).
GROUPED = {
    # A model row per group of the `by` key, each named by its value (ByReducer, Name).
    "a-group-per-key": "sum(x) by (t.g) <= 5",
    # No key: one group of every kept row.
    "one-group": "sum(x) <= 12",
    # The reducer's own `when` keeps rows 1, 3 and 4, so a group emits the rows it reads (a filter of kept rows).
    "a-reducers-own-when": "sum(when t.p: x) by (t.g) <= 5",
    # The second reducer reads no row of group a (c is 3 and 4 there), so group a has no value and is skipped
    # (SkipGroupsWithAnEmptyReducer).
    "a-group-a-reducer-reads-no-row-of": "sum(x) by (t.g) + sum(when t.c > 4.5: x) by (t.g) <= 8",
    # z, beside the reducers, is read on each kept row and counted once per group, on its first row
    # (ReadBesideReducers, CountTupleTermsOncePerGroup).
    "a-term-beside-the-reducers": "sum(x) by (t.g) + z <= 5",
    # Row 1 is in no class of t.k but is group a's first row: its root holds it, so z counts there, as flat counts it
    # (DecideNode::holds_rows_in_no_class).
    "a-row-in-no-class-of-its-per": "per t.k: sum(x) by (t.g) + z <= 5",
    # AVG divides by the rows its group reads (ScaleAvgTerms).
    "an-avg": "avg(x) by (t.g) <= 2",
    # The bound is read on each kept row (DescribeBound).
    "a-bound-on-each-row": "sum(x) by (t.g) <= t.c",
    # The clause's `when` keeps rows 2 to 6.
    "a-clauses-when": "when t.c > 3.5: sum(x) by (t.g) <= 5",
    # A data reducer's value in each group: a hole of the root's layout, which its NaN check reads as well.
    "a-data-reducer": "sum(x) by (t.g) <= max(t.c) by (t.g)",
    # Each variant of a class of t.k once in its group: class v of group b reads 5 and 8, class u 7, so group b's
    # bound is 20 (DecideTreeColumns::FirstOfEachInstance).
    "a-data-reducer-per-class": "x <= 30 AND x <= sum(per t.k: t.c) by (t.g)",
    # Row 4 reaches no group of the data reducer, its g NULL: it reads NULL, so it has no bound.
    "a-data-reducer-a-row-reaches-no-group-of": "x <= sum(t.c) by (t.g)",
}


@pytest.mark.parametrize("case", list(GROUPED))
def test_a_grouped_clause_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x FROM {GROUPED_ROWS} DECIDE x(REAL), per t.g: y(REAL), per (): z(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND z <= 9 AND {GROUPED[case]} MAXIMIZE sum(x) + sum(y) + z"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_clause_with_no_class_of_its_per_holds_no_instance(decidb_cli, tmp_path):
    """Every k is NULL, so `per t.k` names no class: the clause holds no instance."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', NULL::VARCHAR), (2, 'b', NULL)) t(id, g, k)
        DECIDE x(REAL) SUCH THAT x <= 9 AND per t.k: sum(x) by (t.g) <= 5 MAXIMIZE sum(x)"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_an_emptied_group_is_counted_as_flat_counts_it(decidb_cli):
    """No row has c above 100, so each group's reducer reads none: each of the clause's 2 instances has no value (D90),
    which the tree counts as flat does."""
    sql = f"""SELECT id, x FROM {GROUPED_ROWS} DECIDE x(REAL)
        SUCH THAT x <= 9 AND sum(when t.c > 100: x) by (t.g) <= 5 MAXIMIZE sum(x)"""
    stored = reference(decidb_cli, sql)
    assert "each of its 2 instances has no value" in stored.stderr, stored.stderr
    assert run(decidb_cli, sql).stderr == stored.stderr


#: Rows 1 and 2 share a NOCASE class of g, spelled 'a' and 'A', which the reducer's member test reads apart.
NOCASE_ROWS = (
    "(SELECT id, g COLLATE NOCASE AS g, p FROM (VALUES (1, 'a', 5.0), (2, 'A', 4.0), (3, 'b', 3.0)) v(id, g, p)) t"
)

#: A relation-qualified reducer reads each class of its qualifier once, at its first row, however many entries its
#: spellings make of the class (FirstOfEachInstance): in a clause, and in the objective.
QUALIFIED = {
    "in-a-clause": "y <= 10 AND sum(per t.g: y) <= 12 MAXIMIZE sum(t.p * y)",
    "in-the-objective": "y <= 10 MAXIMIZE sum(per t.g: y)",
}


@pytest.mark.parametrize("case", list(QUALIFIED))
def test_a_qualified_reducer_reads_each_class_once(decidb_cli, tmp_path, case):
    sql = f"SELECT id, y FROM {NOCASE_ROWS} DECIDE per t.g: y(INT) SUCH THAT {QUALIFIED[case]}"
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_qualified_avg_divides_by_its_classes(decidb_cli, tmp_path):
    """The join repeats t's rows, and `avg(t: x)` reads each of them once in its group: its filter is the
    de-duplication, so AVG divides by the classes it keeps, as flat's qualifier does (ReadInReducer's has_filter)."""
    sql = """SELECT t.id, s.k, x FROM (VALUES (1, 'a', 3.0, true), (2, 'a', 4.0, false), (3, 'b', 5.0, true),
        (4, NULL, 6.0, true)) t(id, g, c, p) JOIN (VALUES (1, 'u'), (1, 'v'), (2, 'u'), (3, 'u'), (3, 'v'), (3, 'w'),
        (4, 'u')) s(id, k) ON t.id = s.id
        DECIDE per t.id: x(REAL) SUCH THAT x <= 9 AND avg(t: x) by (t.g) <= 2 MAXIMIZE sum(t: x)"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_data_check_raises_flats_message_without_side_plans(decidb_cli):
    """Under tree-noside no side plan checks the data reducer's values: its hidden root does, at the check's rank."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 2.0::DOUBLE), (2, 'a', 'NaN'::DOUBLE)) t(id, g, c)
        DECIDE x(REAL) SUCH THAT x <= max(t.c) by (t.g) MAXIMIZE sum(x)"""
    stored = reference(decidb_cli, sql)
    assert "the bound t.c is NaN on some rows" in stored.stderr, stored.stderr
    assert run(decidb_cli, sql, "tree-noside").stderr == stored.stderr


def test_a_folds_error_raises_flats_message_without_side_plans(decidb_cli):
    """Group a's sum of two DECIMAL(38) maxima overflows as the side plan's sum does: the fold raises its error at the
    data reducer's rank (DecideTreeColumns::Folds)."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 99999999999999999999999999999999999999::DECIMAL(38, 0)),
        (2, 'a', 99999999999999999999999999999999999999::DECIMAL(38, 0)), (3, 'b', 5::DECIMAL(38, 0))) t(id, g, c)
        DECIDE x(REAL) SUCH THAT x <= 9 AND x <= sum(t.c) by (t.g) / 1e40 MAXIMIZE sum(x)"""
    stored = reference(decidb_cli, sql)
    assert "Overflow in HUGEINT addition" in stored.stderr, stored.stderr
    assert run(decidb_cli, sql, "tree-noside").stderr == stored.stderr


def test_a_data_check_raises_what_its_window_meets_first(decidb_cli):
    """The check of max(ln(t.w)) reads ln only where the clause's `when` keeps the row, as the side plan's does: its
    window raises the `when`'s error (row 2), which a vectorized evaluation meets before row 1's logarithm."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 2.0::DOUBLE, -1.0), (2, 'a', -3.0, 2.0), (3, 'b', 3.0, 5.0))
        t(id, g, c, w) DECIDE x(REAL) SUCH THAT x <= 9 AND when sqrt(t.c) > -1: x <= max(ln(t.w)) by (t.g)
        MAXIMIZE sum(x)"""
    stored = reference(decidb_cli, sql)
    assert "cannot take square root of a negative number" in stored.stderr, stored.stderr
    assert run(decidb_cli, sql, "tree-noside").stderr == stored.stderr


#: Flat reads a reducer's own `when` in its terms' filter, window by window and term by term, after the routes and
#: before any coefficient: each case raises flat's error, a `when`'s or a member test's, where the descent reads the
#: `when` (DecideNode::filter_group).
FILTER_RAISED = {
    # Term 0's member test (row 2) before term 1's `when` (row 1).
    "a-test-before-the-next-terms-when": (
        "(VALUES (1, 0.0, 2.0, 1.0), (2, 2.0, -1.0, 1.0)) t(id, a, w, v)",
        "sum(ln(t.w) * t.v * x) + sum(when ln(t.a) > -100: x)",
        "logarithm of a negative number",
    ),
    # Term 0's `when` (row 2) before term 1's (row 1).
    "a-when-before-the-next-terms-when": (
        "(VALUES (1, 2.0, -1.0, 1.0), (2, 0.0, 2.0, 1.0)) t(id, a, w, v)",
        "sum(when ln(t.a) > -100: x) + sum(when ln(t.w) > -100: 2 * x)",
        "logarithm of zero",
    ),
    # A reducer's `when` (row 2) before its own member test (row 1): the filter reads the `when` first.
    "a-when-before-its-member-test": (
        "(VALUES (1, 2.0, -1.0, 1.0), (2, 0.0, 2.0, 1.0)) t(id, a, w, v)",
        "sum(when ln(t.a) > -100: ln(t.w) * t.v * x)",
        "logarithm of zero",
    ),
    # A `when` of two conjuncts reads them in their order: the first's error (row 2) before the second's (row 1).
    "a-conjunction-in-its-order": (
        "(VALUES (1, 2.0, -1.0, 1.0), (2, -1.0, 2.0, 1.0)) t(id, a, w, v)",
        "sum(when sqrt(t.a) > -1 AND ln(t.w) > -100: x)",
        "square root of a negative number",
    ),
    # A member test in the first window (row 10) before the `when` in the second (row 2,500).
    "a-window-before-the-next": (
        "(SELECT range AS id, CASE WHEN range = 2500 THEN 0.0 ELSE 2.0 END AS a, "
        "CASE WHEN range = 10 THEN -1.0 ELSE 2.0 END AS w, 1.0 AS v FROM range(3000)) t",
        "sum(when ln(t.a) > -100: ln(t.w) * t.v * x)",
        "logarithm of a negative number",
    ),
}


@pytest.mark.parametrize("case", list(FILTER_RAISED))
def test_a_reducers_when_raises_in_its_filter(decidb_cli, case):
    rows, objective, message = FILTER_RAISED[case]
    sql = f"SELECT id, x FROM {rows} DECIDE x(REAL) SUCH THAT x <= 1 MAXIMIZE {objective}"
    stored = reference(decidb_cli, sql)
    assert message in stored.stderr, stored.stderr
    tree = run(decidb_cli, sql)
    assert tree.stderr == stored.stderr


def test_a_grouped_clauses_reducers_raise_in_term_order(decidb_cli):
    """In a clause, as in the objective: term 0's member test (row 2) before term 1's `when` (row 1)."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 0.0, 2.0, 1.0), (2, 'a', 2.0, -1.0, 1.0)) t(id, g, a, w, v)
        DECIDE x(REAL) SUCH THAT x <= 1 AND sum(ln(t.w) * t.v * x) by (t.g) + sum(when ln(t.a) > -100: x) by (t.g) <= 5
        MAXIMIZE sum(x)"""
    stored = reference(decidb_cli, sql)
    assert "logarithm of a negative number" in stored.stderr, stored.stderr
    tree = run(decidb_cli, sql)
    assert tree.stderr == stored.stderr


#: Flat groups a clause's kept rows by its `by` key after it computes the terms, so a key's error ranks after theirs:
#: under each evaluator that reads the clause, each case raises flat's error (DecideNode::key_block).
KEY_RAISED = {
    # A reducer's own `when` (row 1) before the key (row 2).
    "a-reducers-when-before-its-by-key": (
        "(VALUES (1, 2.0, -1.0), (2, -1.0, 2.0), (3, 3.0, 5.0)) t(id, c, w)",
        "sum(when ln(t.w) > -100: x) by (sqrt(t.c)) <= 8",
        "logarithm of a negative number",
    ),
    # A coefficient (row 1) before the key (row 2).
    "a-coefficient-before-the-by-key": (
        "(VALUES (1, 2.0, -1.0), (2, -1.0, 2.0), (3, 3.0, 5.0)) t(id, c, w)",
        "sum(ln(t.w) * x) by (sqrt(t.c)) <= 8",
        "logarithm of a negative number",
    ),
    # The key raises on row 2, which still reaches the terms, whose coefficient raises there first.
    "a-coefficient-on-a-row-its-key-raises-on": (
        "(VALUES (1, 2.0), (2, -1.0), (3, 3.0)) t(id, c)",
        "sum(ln(t.c) * x) by (sqrt(t.c)) <= 8",
        "logarithm of a negative number",
    ),
    # A key's elements in their order, the logarithm's (row 2) before the square root's (row 1).
    "a-keys-elements-in-their-order": (
        "(VALUES (1, -1.0, 2.0), (2, 2.0, -1.0), (3, 3.0, 5.0)) t(id, c, w)",
        "sum(x) by (sqrt(t.c), ln(t.w)) <= 8",
        "logarithm of a negative number",
    ),
}


@pytest.mark.parametrize("case", list(KEY_RAISED))
def test_a_by_key_raises_after_the_terms(decidb_cli, case):
    rows, clause, message = KEY_RAISED[case]
    sql = f"SELECT id, x FROM {rows} DECIDE x(REAL) SUCH THAT x <= 9 AND {clause} MAXIMIZE sum(x)"
    stored = reference(decidb_cli, sql)
    assert message in stored.stderr, stored.stderr
    result = run(decidb_cli, sql)
    assert result.stderr == stored.stderr


def test_a_row_in_no_group_reaches_the_operand(decidb_cli):
    """Row 2's g is NULL, so it is in no group of the G: it still reaches the G's operand, which flat computes on every
    row the G reads, so its NaN coefficient raises flat's message (DecideDescent::Visit)."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 3.0::DOUBLE), (2, NULL, 'NaN'::DOUBLE), (3, 'b', 5.0)) t(id, g, c)
        DECIDE x(REAL) SUCH THAT x <= 9 AND sum(t.c * x) by (t.g) <= 8 MAXIMIZE sum(x)"""
    stored = reference(decidb_cli, sql)
    assert "t.c is NaN or infinite on some rows" in stored.stderr, stored.stderr
    result = run(decidb_cli, sql)
    assert result.stderr == stored.stderr


def test_the_objective_reads_no_term_at_a_row_in_no_group(decidb_cli):
    """Flat reads the objective's sum of group sums in place (D81), on the rows of each group: row 2's g is NULL, so
    its logarithm of 0 is never computed, and the objective's G does not route the row to its operand."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 1.0), (2, NULL, 0.0), (3, 'b', 2.0)) t(id, g, w)
        DECIDE x(INT) SUCH THAT x <= 10 AND sum(x) <= 10 MAXIMIZE sum(per t.g: sum(ln(t.w) * x) by (t.g))"""
    result = run(decidb_cli, sql)
    assert result.returncode == 0 and result.stderr == "", result.stderr
    assert result.stdout == reference(decidb_cli, sql).stdout


#: A relation's columns compare NULL as a value: rows 2 and 3 are one group of d, which flat numbers after every other.
RELATION_ROWS = "(VALUES (1, 'a', 2), (2, NULL, 4), (3, NULL, 4), (4, 'b', 1))"


def test_a_null_key_value_is_the_last_group(decidb_cli, tmp_path):
    """The tree's groups are numbered as flat numbers them, NULL after every value, so the model's rows come in flat's
    order."""
    sql = f"""SELECT id, x FROM {RELATION_ROWS} t(id, g, c)
        JOIN (SELECT DISTINCT g AS dg, c AS dc FROM {RELATION_ROWS} s(id, g, c)) d
        ON t.g IS NOT DISTINCT FROM d.dg AND t.c = d.dc
        DECIDE x(REAL) SUCH THAT x <= 9 AND sum(x) by (d) <= t.c MAXIMIZE sum(x)"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_relations_null_key_class_is_numbered_last(decidb_cli):
    """T's key compares NULL as a value, so the class of T.k NULL is the last class of y's scope, as flat numbers it
    (DecideDescent::Finish): verify finds the tree's classes flat's."""
    sql = """SELECT T.k, y FROM (VALUES (1, NULL), (2, 4.0), (NULL, 5.0)) T(k, v)
        JOIN (VALUES (1, 'u'), (1, 'w'), (2, 'u'), (NULL, 'z')) W(k, c) ON T.k IS NOT DISTINCT FROM W.k
        DECIDE per T: y(REAL) SUCH THAT y <= 9 MAXIMIZE sum(T: y)"""
    _verified(decidb_cli, sql)


def _verified(cli, sql):
    """The query under the tree prints what its stored expectation holds: flat's output when it was recorded."""
    result, stored = run(cli, sql), reference(cli, sql)
    assert (result.stdout, result.stderr) == (stored.stdout, stored.stderr)


#: A diagnosis of a clause in the aggregate shape reports the classes each group's row stands for
#: (SolverInput::class_sets), which verify compares, by the branch each case reaches (DecideGroupClassSets): x <= 9
#: cannot meet a sum of 100.
GROUP_CLASSES = {
    # With no `per`, a group's row stands for each of its kept rows.
    "kept-rows": "sum(x) by (t.g) >= 100",
    # One group, which holds every kept row.
    "one-group": "sum(x) >= 100",
    # Under `per t.k:`, for the classes of t.k among its kept rows; row 1 is in none, though group a holds it
    # (DecideModelEmitter::KeyClasses).
    "classes-of-its-per": "per t.k: sum(x) by (t.g) >= 100",
    # Under `per ()`, for the whole clause.
    "the-whole-clause": "per (): sum(x) by (t.g) >= 100",
}


@pytest.mark.parametrize("case", list(GROUP_CLASSES))
def test_a_diagnosis_reports_the_classes_a_groups_row_stands_for(decidb_cli, case):
    sql = f"""DIAGNOSE SELECT id, x FROM {GROUPED_ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND {GROUP_CLASSES[case]}
        MAXIMIZE sum(x)"""
    _verified(decidb_cli, sql)


#: Rows for the per-row shape under `per K:` (DecideClauseScope::ONE_ROW_PER_CLASS): class b's rows come before class
#: a's, so flat's numbering by value is not its numbering by row; class a's rows 2 and 4 read one value of c, class b's
#: rows 1 and 3 two; row 5's g is NULL, so it is in no class.
CLASS_ROWS = "(VALUES (1, 'b', 3.0::DOUBLE), (2, 'a', 4.0), (3, 'b', 5.0), (4, 'a', 4.0), (5, NULL, 1.0)) t(id, g, c)"

#: Each a clause with one model row per class or per variant of a class of its `per K:`, which the tree builds, names,
#: counts and reports as flat does (verify compares them, and the classes a diagnosis reports), by the branch it
#: reaches (DecideModelEmitter::Classes).
CLASSES = {
    # One row per class of t.g, at its first row, named by its class (KeyClasses).
    "a-row-per-class": "per t.g: z + y <= 5",
    # One row per value of c in a class: class b's two values make 3 variants of 2 classes, numbered by value, each
    # named by its class and its value (NameVariants) and, under DIAGNOSE, standing for itself; EXPLAIN ANALYZE counts
    # them (RecordVariants).
    "a-row-per-value": "per t.g: z + t.c <= 20",
    # x is per row, so each row is a variant of its own, numbered by its first row (Variants); rows 2 and 4 read one
    # value of c, so each is named by its row too, and by no value of x's instance.
    "a-row-per-decision": "per t.g: x + t.c <= 20",
    # `per ()` has one class with no value, which its variants are named without; under DIAGNOSE they stand for the
    # whole clause.
    "one-class": "per (): z + t.c <= 20",
    # A clause's `when`: its rows do not stand for every row of their class (rows_stand_with_their_class).
    "a-when": "when t.c > 3.5 per t.g: z + y <= 5",
}


@pytest.mark.parametrize("diagnose", ["", "DIAGNOSE "], ids=["solve", "diagnose"])
@pytest.mark.parametrize("case", list(CLASSES))
def test_a_row_per_class_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path, case, diagnose):
    sql = f"""SELECT id, x FROM {CLASS_ROWS} DECIDE x(REAL), per t.g: y(REAL), per (): z(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND z <= 9 AND {CLASSES[case]} MAXIMIZE sum(x) + sum(y) + z"""
    _verified(decidb_cli, diagnose + sql)
    if not diagnose:
        stored, tree = models(decidb_cli, sql, tmp_path)
        assert tree == stored


#: A relation-qualified reducer reads one value per class and value of what its body reads (KeyedScopeInfo::signature):
#: its classes are its operand's variants, numbered as flat numbers them (DecideModelEmitter::SignatureMapping). Row 5's
#: g is NULL, so the reducer reads no value there, which maps it to a class of its own.
SPLIT_CLASSES = {
    # t.c takes two values in class b: 3 values of 2 classes, which EXPLAIN ANALYZE counts. y's instance is a field,
    # so they are numbered by their first rows.
    "by-first-row": "x + y <= t.c MAXIMIZE sum(per t.g: t.c * y)",
    # z is one decision for every row, so they are numbered by value.
    "by-value": "x + z <= t.c MAXIMIZE sum(per t.g: t.c * z)",
}


@pytest.mark.parametrize("case", list(SPLIT_CLASSES))
def test_a_qualified_reducers_classes_are_split_by_its_values(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x FROM {CLASS_ROWS} DECIDE x(REAL), per t.g: y(REAL), per (): z(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND z <= 9 AND {SPLIT_CLASSES[case]}"""
    _verified(decidb_cli, sql)
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


#: A relation-qualified reducer reads its terms on every row its `when` keeps, and keeps no value of a row in no class
#: of its `per` (ApplyQualifierToFilter): its operand holds such a row in a slot of its own
#: (DecideNode::holds_rows_in_no_class), which the reducer reads nothing of (DecideTreeColumns::FirstOfEachInstance) and
#: no variant holds (DecideModelEmitter::Variants). Row 5's g is NULL, and its c is each case's.
NO_CLASS_ROWS = """(VALUES (1, 'b', 'u', 3.0::DOUBLE), (2, 'a', 'u', 4.0), (3, 'b', 'v', 5.0), (4, 'a', 'u', 4.0),
    (5, NULL, 'z', {c})) t(id, g, h, c)"""

IN_NO_CLASS = {
    # Row 5 stays in the clause's one group.
    "in-its-group": ("1.0", "x <= 9 AND sum(per t.g: t.c * x) <= 20 MAXIMIZE sum(x)"),
    # Row 5 alone makes group z, in which the reducer reads no value, so z has none (D68).
    "a-group-of-its-own": ("1.0", "x <= 9 AND sum(per t.g: x) by (t.h) >= 5 MAXIMIZE sum(x)"),
    # Its coefficient is computed there, so its NaN is refused, in a clause and in the objective.
    "a-nan-in-a-clause": ("'NaN'::DOUBLE", "x <= 9 AND sum(per t.g: t.c * x) <= 20 MAXIMIZE sum(x)"),
    "a-nan-in-the-objective": ("'NaN'::DOUBLE", "x <= 9 MAXIMIZE sum(per t.g: t.c * x)"),
}


@pytest.mark.parametrize("case", list(IN_NO_CLASS))
def test_a_qualified_reducer_reads_its_rows_in_no_class_as_flat_does(decidb_cli, case):
    c, clause = IN_NO_CLASS[case]
    sql = f"SELECT id, x FROM {NO_CLASS_ROWS.format(c=c)} DECIDE x(REAL) SUCH THAT {clause}"
    _verified(decidb_cli, sql)
    assert run(decidb_cli, sql).stderr == reference(decidb_cli, sql).stderr


#: A variant is named by what its class key and its fields read at its first row, as written (DecideBody::names), and
#: a collated column of its class by its least spelling among the clause's rows of the class
#: (DecideDescentGroup::spelling), as flat's reader names it; verify compares the names an expanded diagnosis shows.
NAMED_VARIANTS = {
    # The `by` key t.h is collated: its field is spelled as the variant's first row spells it, 'U', not as it groups.
    "a-by-key-as-written": """SELECT id, y FROM (SELECT id, g, h COLLATE NOCASE AS h, c FROM (VALUES (1, 'a', 'U', 3.0),
        (2, 'a', 'v', 4.0), (3, 'b', 'w', 7.0), (4, 'a', 'u', 2.0)) v(id, g, h, c)) t
        DECIDE per t.g: y(BOOL) SUCH THAT per t.g: y + sum(t.c) by (t.h) <= 4.5 MAXIMIZE sum(y)""",
    # -0.0 and 0.0 are one class: each variant is named by its own first row's g.
    "a-class-at-its-first-row": """SELECT id, y FROM (VALUES (1, -0.0::DOUBLE, 4.0), (2, 0.0::DOUBLE, 1.0),
        (3, 1.0::DOUBLE, 7.0)) t(id, g, c) DECIDE per t.g: y(BOOL) SUCH THAT per t.g: y + t.c <= 0.5""",
    # y's key reads row 1's 'A', which the clause's `when` leaves out: its class is named 'a'.
    "a-collated-class-among-its-rows": """SELECT id, y FROM (SELECT id, g COLLATE NOCASE AS g, c, v FROM (VALUES
        (1, 'A', -1, 1.0), (2, 'a', 1, 2.0), (3, 'a', 1, 3.0), (4, 'b', 1, 1.0)) w(id, g, c, v)) t
        DECIDE per t.g: y(BOOL) SUCH THAT when t.c > 0 per t.g: y + t.v >= 10 MAXIMIZE sum(y)""",
}


@pytest.mark.parametrize("case", list(NAMED_VARIANTS))
def test_a_variant_is_named_as_written_at_its_first_row(decidb_cli, case):
    expanded = "PRAGMA diagnose_decide_infeasible_slack_scope='expanded'; "
    _verified(decidb_cli, f"{expanded}DIAGNOSE {NAMED_VARIANTS[case]}")


def test_a_folded_row_of_one_class_has_no_name(decidb_cli):
    """`2 * z >= 30` is folded into z's box, and only a repair model emits its row: one variant of the one class of
    `per ()`, which has no value to name it by (Classes, under DIAGNOSE alone)."""
    sql = f"""DIAGNOSE SELECT id, x FROM {CLASS_ROWS} DECIDE x(REAL), per (): z(REAL)
        SUCH THAT x <= 9 AND z <= 9 AND per (): 2 * z >= 30 MAXIMIZE sum(x) + z"""
    _verified(decidb_cli, sql)


#: Rows for an auxiliary's absence: row 1's c is NULL; row 4's g is NULL, so y has no instance there.
ABSENT_ROWS = "(VALUES (1, 'a', NULL::DOUBLE), (2, 'a', 3.0), (3, 'b', 5.0), (4, NULL, 2.0)) t(id, g, c)"

#: An auxiliary has no value where what defines it reads none (SolverInput::absent_reads, D88), so a row reading it
#: skips its instance, as flat does; verify compares the absence. By the branch each case reaches
#: (DecideModelEmitter::ReadAbsence).
DEFINED = {
    # Row 1's NULL is in its definition's bound, `a >= x - t.c` read as `a - x >= -t.c`: the clause skips row 1.
    "a-null-bound": "x + abs(x - t.c) <= 20",
    # Row 1's NULL is in its definition's coefficient.
    "a-null-coefficient": "x + abs(t.c * x - 1) <= 20",
    # y has no instance on row 4, which its definition reads.
    "a-decision-with-no-instance": "x + abs(y - 1) <= 20",
    # The outer auxiliary's definition reads the inner one, which has no value on row 1.
    "an-auxiliary-under-another": "x + abs(abs(x - t.c) - 1) <= 20",
}


@pytest.mark.parametrize("case", list(DEFINED))
def test_an_auxiliary_has_no_value_where_its_definition_has_none(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x FROM {ABSENT_ROWS} DECIDE x(REAL), per t.g: y(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND {DEFINED[case]} MAXIMIZE sum(x) + sum(y)"""
    _verified(decidb_cli, sql)
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


#: Three thousand rows, two windows of 2,048 rows: row 10's d is 0 and row 2,500's c is -1, so ln(t.d) raises in the
#: first window and sqrt(t.c) in the second.
WINDOWS = """(SELECT i, CASE WHEN i = 2500 THEN -1.0 ELSE 1.0 END AS c, CASE WHEN i = 10 THEN 0.0 ELSE 1.0 END AS d
    FROM range(3000) r(i)) t"""

#: An error a definition raises ranks where flat raises it, under tree and under verify, by the branch each case
#: reaches (DecideNode::value_rank, clause_rank).
DEFINITION_ERRORS = {
    # Flat checks a definition's numbers where it evaluates the row among the clauses (clause_rank), after
    # `x <= t.d`'s: row 1's NaN bound raises, not row 2's NaN in |x - t.c|.
    "its-numbers-at-its-place": """(VALUES (1, 1.0, 'NaN'::DOUBLE), (2, 'NaN'::DOUBLE, 1.0)) t(i, c, d)
        DECIDE x(REAL) SUCH THAT x <= t.d AND x + abs(x - t.c) <= 20""",
    # And after the clause before it reads ln(0): the clause's error raises, not the NaN in |t.c * x - 1|.
    "its-numbers-after-an-earlier-clause": """(VALUES (1, 'NaN'::DOUBLE, 0.0), (2, 1.0, 1.0)) t(i, c, d)
        DECIDE x(REAL) SUCH THAT x <= ln(t.d) + 9 AND x + abs(t.c * x - 1) <= 20""",
    # Flat computes a definition first where it resolves the auxiliary's absence, before any clause
    # (DecideTreeColumns::NullEntries): sqrt(-1) raises, though `x <= ln(t.d) + 9` comes first.
    "its-execution-before-every-clause": """(VALUES (1, 1.0, 0.0), (2, -1.0, 1.0)) t(i, c, d)
        DECIDE x(REAL) SUCH THAT x <= ln(t.d) + 9 AND x + abs(x - sqrt(t.c)) <= 20""",
    # It computes a definition's bound and its coefficients together, the bound first: ln(0) on the same row.
    "its-bound-before-its-coefficient": """(VALUES (1, -1.0, 0.0), (2, 1.0, 1.0)) t(i, c, d)
        DECIDE x(REAL) SUCH THAT x <= 9 AND x + abs(sqrt(t.c) * x - ln(t.d)) <= 20""",
    # A window at a time: ln(0) in the first window, though the coefficient comes before the bound in the clause.
    "its-windows-in-order": f"""{WINDOWS}
        DECIDE x(REAL) SUCH THAT x <= 9 AND x + abs(sqrt(t.c) * x - ln(t.d)) <= 20""",
    # Auxiliary after auxiliary (DecideErrorRank::step): the inner |x - sqrt(t.c)|'s, though the outer's ln(t.d) is in
    # the first window.
    "an-auxiliary-before-the-next": f"""{WINDOWS}
        DECIDE x(REAL) SUCH THAT x <= 9 AND x + abs(abs(x - sqrt(t.c)) - ln(t.d)) <= 20""",
}


@pytest.mark.parametrize("case", list(DEFINITION_ERRORS))
def test_a_definitions_error_ranks_where_flat_raises_it(decidb_cli, case):
    sql = f"SELECT i, x FROM {DEFINITION_ERRORS[case]} MAXIMIZE sum(x)"
    stored = reference(decidb_cli, sql).stderr
    assert "Error" in stored
    assert run(decidb_cli, sql).stderr == stored


def test_the_objective_drops_an_auxiliary_with_no_value(decidb_cli, tmp_path):
    """Row 1's |x - t.c| has no value, so the objective drops its term there (D84; DecideTreeColumns::ReadEntries)."""
    sql = f"SELECT id, x FROM {ABSENT_ROWS} DECIDE x(REAL) SUCH THAT x <= 9 AND x >= 1 MINIMIZE sum(abs(x - t.c))"
    _verified(decidb_cli, sql)
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_not_equal_answers_through_its_implied_bounds(decidb_cli, tmp_path):
    """The sum bounds each x, which stage 06 reads into the `<>` it expands (EvaluatedConstraint::ne_clause_idx): the
    tree's rows are flat's, so it answers as flat does."""
    sql = f"SELECT id, x FROM {GROUPED_ROWS} DECIDE x(INT) SUCH THAT x >= 0 AND sum(x) <= 10 AND x <> 3 MAXIMIZE sum(x)"
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored
    assert run(decidb_cli, f"{sql} ORDER BY id").stdout == reference(decidb_cli, f"{sql} ORDER BY id").stdout


def test_an_avg_under_not_equal_multiplies_its_bound(decidb_cli, tmp_path):
    """An AVG under `<>` keeps its sums and multiplies its bound by its group's count instead
    (HoistAvgIntoNotEqualBound), as flat's model does."""
    sql = f"""SELECT id, x FROM {GROUPED_ROWS} DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 5 AND avg(x) by (t.g) <> 3 MAXIMIZE sum(x)"""
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


#: Rows for the MIN and MAX arms: g's groups a (rows 1, 2) and b (rows 3, 4), whose least c, 3 and 1, a group's bound
#: `<= t.c` reads; w is NULL on row 4, and p keeps rows 1, 3 and 4.
EXTREMA = """(VALUES (1, 'a', 3.0, 2.0, true), (2, 'a', 5.0, 1.0, false), (3, 'b', 1.0, 4.0, true),
    (4, 'b', 4.0, NULL, true)) t(id, g, c, w, p)"""


def _modelled_as_flat(cli, sql, tmp_path):
    """The tree's model is flat's; returns the model."""
    stored, tree = models(cli, sql, tmp_path)
    assert tree == stored
    return tree


def test_an_easy_extremum_is_a_model_row_per_member(decidb_cli, tmp_path):
    """An easy MAX, which its `<=` drives, is formulated in the grouped shape (MEMBER_ROWS): a model row per member of
    its G, each bounded by its group's least c, 3, 3, 1 and 1. A member's terms are read as written on its own row, not
    counted once in its group as a decision beside an aggregate's reducers is (CountTupleTermsOncePerGroup)."""
    sql = f"""SELECT id, x, y FROM {EXTREMA} DECIDE x(INT), y(INT)
        SUCH THAT max(x + y) by (t.g) <= t.c AND x <= 9 AND y <= 9 MAXIMIZE sum(x)"""
    tree = _modelled_as_flat(decidb_cli, sql, tmp_path)
    assert re.findall(r"sense=< rhs=(\S+) nnz=2", tree) == ["3", "3", "1", "1"], tree


def test_an_easy_extremum_of_one_decision_bounds_each_members_column(decidb_cli, tmp_path):
    """`max(x) by (t.g) <= t.c` admits x up to its group's least c on each member of its G, its term's site, which the
    model builder folds into each x's column (DecideModelEmitter::Admit): no row."""
    sql = f"SELECT id, x FROM {EXTREMA} DECIDE x(REAL) SUCH THAT max(x) by (t.g) <= t.c MAXIMIZE sum(x)"
    tree = _modelled_as_flat(decidb_cli, sql, tmp_path)
    assert re.findall(r" ub=(\S+) ", tree) == ["3", "3", "1", "1"] and "num_rows: 0" in tree, tree


def test_a_hard_extremum_is_diagnosed_by_the_bound_as_written(decidb_cli):
    """A hard MAX, which its `>=` does not drive, hands stage 06 its groups' rows (EXTREMUM_ROWS), whose outer row over
    the extremum stage 06 stamps USER_PARAMETER: the diagnosis loosens the bound as written, 2 to 1, as flat's does."""
    sql = (
        f"DIAGNOSE SELECT id, x FROM {EXTREMA} DECIDE x(REAL) SUCH THAT x <= 1 AND max(x) by (t.g) >= 2 MINIMIZE sum(x)"
    )
    result = run(decidb_cli, sql)
    stored = reference(decidb_cli, sql).stdout
    assert "max(x) by (t.g) >= 1" in stored, stored
    assert result.stdout == stored


def test_a_folded_extremums_repair_rows_count_no_instance(decidb_cli):
    """`max(x) <= 5` is folded into x's box, and its rows, which only a repair model emits, are a folded bound's: no
    instance of them is counted, as flat counts none (DecideModelEmitter::Groups' `counted`)."""
    sql = """DIAGNOSE SELECT id, x FROM (VALUES (1, 8), (2, 12)) t(id, lo)
        DECIDE x(REAL) SUCH THAT x >= t.lo AND max(x) <= 5 MAXIMIZE sum(x)"""
    _verified(decidb_cli, sql)


def test_an_extremum_objective_takes_the_rows_its_filters_keep(decidb_cli, tmp_path):
    """A MAX objective's members are the rows its terms' filters keep, which stage 06 reads (OBJECTIVE_EXTREMUM): its
    `when` keeps rows 1, 3 and 4, and w's NULL test drops row 4."""
    sql = f"""SELECT id, x FROM {EXTREMA} DECIDE x(REAL) SUCH THAT x <= t.c AND x >= 1
        MINIMIZE when t.p: max(t.w * x)"""
    _modelled_as_flat(decidb_cli, sql, tmp_path)


#: A join that repeats t's rows: t's classes 1 and 3 stand on two rows each, one entry of an operand over t's classes.
JOINED = """(VALUES (1, 'a', 1), (2, 'a', 2), (3, 'b', 3)) t(id, g, n)
    JOIN (VALUES (1, 'u'), (1, 'v'), (2, 'u'), (3, 'u'), (3, 'v')) s(id, k) ON t.id = s.id"""

#: A MIN or MAX over t's classes reads every row an entry of its operand stands for, as flat reads each of them a
#: member: stage 05 gives its terms no qualifier, since a class's repeats carry its value
#: (DecideModelEmitter::ReadRows). A relation-qualified SUM reads its class's first row alone.
JOINED_REDUCERS = {
    "an-extremum-in-a-clause": "MIN(t: t.n * keep) <= 1 AND SUM(t: keep) >= 2 MAXIMIZE SUM(t: keep)",
    "an-extremum-objective": "SUM(t: keep) >= 2 MINIMIZE MAX(t: t.n * keep)",
    "a-composed-clause": "SUM(t: t.n * keep) + MAX(t: keep) <= 4 MAXIMIZE SUM(t: keep)",
}


@pytest.mark.parametrize("case", list(JOINED_REDUCERS))
def test_a_reducer_over_a_classes_rows_reads_them_as_flat_does(decidb_cli, tmp_path, case):
    sql = f"SELECT t.id, s.k, keep FROM {JOINED} DECIDE per t: keep(BOOL) SUCH THAT {JOINED_REDUCERS[case]}"
    _modelled_as_flat(decidb_cli, sql, tmp_path)


#: A composed term qualified by a scope whose classes the values it reads split, a per-row decision's (D70, D71): its
#: operand is the one the scope's keyed mapping reads (DecideTreeBuilder::Qualified), from a composed clause's terms and
#: from the objective's, each covered with flat's model.
QUALIFIED_COMPOSED = {
    "a-clauses-term-over-a-relation": (
        f"SELECT t.id, s.k, x, y FROM {JOINED}",
        "AND sum(t: t.n * x) + max(s.id * y) <= 10 MAXIMIZE sum(x) + sum(y)",
    ),
    "an-objectives-term-per-key": (f"SELECT id, x, y FROM {EXTREMA}", "MAXIMIZE sum(x) - max(per t.g: t.c * y)"),
}


@pytest.mark.parametrize("case", list(QUALIFIED_COMPOSED))
def test_a_qualified_composed_term_reads_its_scopes_classes(decidb_cli, tmp_path, case):
    select, rest = QUALIFIED_COMPOSED[case]
    sql = f"{select} DECIDE x(REAL), y(REAL) SUCH THAT x <= 9 AND y <= 9 {rest}"
    _modelled_as_flat(decidb_cli, sql, tmp_path)
    _verified(decidb_cli, sql)


#: Each a composed clause, `sum(x) + max(y)` and its kin (COMPOSED), the tree formulates and counts as flat does, by
#: the branch it reaches (DecideModelEmitter::Composed), and whether flat warns that it constrains nothing.
COMPOSED = {
    # A row `z >= y` per member of the MAX, and one outer row over z and the sum.
    "an-outer-row": ("sum(x) + max(y) <= 9", False),
    # A factor, read as flat reads it, on the input's first rows (DecideQueryWideScale).
    "a-factor": ("sum(x) + (SELECT 2.0) * max(y) <= 9", False),
    # A NULL factor makes the clause's one instance NULL: it is skipped, with no outer row.
    "a-null-factor": ("sum(x) + (SELECT NULL::DOUBLE) * max(y) <= 9", True),
    # A reducer over no row is NULL: the clause is skipped, which flat reports once the clauses are read.
    "a-reducer-over-no-row": ("sum(x) + max(when t.c > 100: y) <= 9", True),
    # A `when` that keeps no row leaves the clause no instance, so there is nothing to report; its factor is read on the
    # input's first rows, whatever the clause keeps.
    "a-when-that-keeps-no-row": ("when t.c > 100: sum(x) + (SELECT 2.0) * max(y) <= 9", False),
}


@pytest.mark.parametrize("case", list(COMPOSED))
def test_a_composed_clause_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path, case):
    clause, warned = COMPOSED[case]
    sql = f"""SELECT id, x, y FROM {EXTREMA} DECIDE x(INT), y(INT)
        SUCH THAT {clause} AND x <= 5 AND y <= 5 MAXIMIZE sum(x) + sum(y)"""
    result = run(decidb_cli, sql)
    assert ("constrains nothing" in result.stderr) == warned, result.stderr
    assert result.stderr == reference(decidb_cli, sql).stderr
    stored, tree = models(decidb_cli, sql, tmp_path, ("constrains nothing",) if warned else ())
    assert tree == stored
    if case == "an-outer-row":
        assert len(re.findall(r"sense=> rhs=0 nnz=2", tree)) == 4 and "sense=< rhs=9 nnz=5" in tree, tree
    _verified(decidb_cli, sql)


def test_a_composed_clause_is_diagnosed_per_class_of_its_per(decidb_cli):
    """Under DIAGNOSE a composed clause's one row stands for the classes of its `per t.g:` among its kept rows
    (ComposedConstraint::class_set)."""
    sql = f"""DIAGNOSE SELECT id, x, y FROM {EXTREMA} DECIDE x(INT), y(INT)
        SUCH THAT per t.g: sum(x) + max(y) >= 90 AND x <= 5 AND y <= 5 MAXIMIZE sum(x) + sum(y)"""
    _verified(decidb_cli, sql)
    assert run(decidb_cli, sql).stdout == reference(decidb_cli, sql).stdout


#: Each an objective with composed terms (OBJECTIVE_COMPOSED): its terms, and a term over no row, which is dropped.
COMPOSED_OBJECTIVES = {
    "its-terms": "max(x) + min(y)",
    "a-term-over-no-row": "sum(x) + max(when t.c > 100: y)",
}


@pytest.mark.parametrize("case", list(COMPOSED_OBJECTIVES))
def test_a_composed_objective_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x, y FROM {EXTREMA} DECIDE x(INT), y(INT)
        SUCH THAT x <= t.c AND y <= 5 MAXIMIZE {COMPOSED_OBJECTIVES[case]}"""
    _modelled_as_flat(decidb_cli, sql, tmp_path)


#: Rows with a NaN on row 2 in v and on row 1 in w.
COMPOSED_NANS = "(VALUES (1, 1.0, 'NaN'::DOUBLE), (2, 'NaN'::DOUBLE, 1.0), (3, 1.0, 1.0)) t(id, v, w)"

#: Each a composed clause's error, raised where flat raises it.
COMPOSED_RAISED = {
    # Term 0's NaN on row 2 before term 1's on row 1: flat reads a composed clause's terms one after another, each over
    # every row before the next (DecideErrorRank::part).
    "a-term-before-the-next": ("sum(t.v * x) + max(t.w * y) <= 10", "t.v is NaN"),
    # Term 0's NaN on row 2 before term 1's factor, which flat reads with its term, after term 0.
    "a-term-before-the-next-terms-factor": ("sum(t.v * x) + max(y) / (SELECT 'NaN'::DOUBLE) <= 10", "t.v is NaN"),
    # A clause's NaN bound before a composed clause's factor, which the ledger holds at its rank.
    "a-clause-before-a-factor": ("x <= t.v AND sum(x) + max(y) / (SELECT 'NaN'::DOUBLE) <= 10", "the bound t.v"),
    # A coefficient DeciDB computed past a DOUBLE names the composed clause, as its term's G does
    # (DecideNode::source_clause_id).
    "a-coefficient-past-a-double": ("sum(x) + max(1e308 * 10 * y) <= 10", "max(1e308 * 10 * y) <= 10 computes"),
}


@pytest.mark.parametrize("case", list(COMPOSED_RAISED))
def test_a_composed_clause_raises_flats_error(decidb_cli, case):
    clauses, message = COMPOSED_RAISED[case]
    sql = f"""SELECT id, x, y FROM {COMPOSED_NANS} DECIDE x(REAL), y(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND {clauses} MAXIMIZE sum(x)"""
    stored = reference(decidb_cli, sql)
    assert message in stored.stderr, stored.stderr
    assert run(decidb_cli, sql).stderr == stored.stderr


#: Three thousand rows: la is -1 on row 2,500, in the second window, and lb on row 10, in the first; in the first window
#: too, a is 0 on row 2 and b is -1 on row 1. h is 1 on every row.
COMPOSED_WINDOWS = """(SELECT i AS id, CASE WHEN i = 2500 THEN -1.0 ELSE 2.0 END AS la,
    CASE WHEN i = 10 THEN -1.0 ELSE 2.0 END AS lb, CASE WHEN i = 2 THEN 0.0 ELSE 2.0 END AS a,
    CASE WHEN i = 1 THEN -1.0 ELSE 2.0 END AS b, 1 AS h FROM range(3000) r(i)) t"""

#: A composed clause's `when`, and the objective's where its composed terms alone carry it, is the first part of each
#: term's filter (ScopeComposedTerms), which flat reads window by window, term by term, before any coefficient: each
#: case raises flat's error under tree and verify, by the branch it reaches (DecideNode::filter_group, PartComposed).
COMPOSED_WHEN_RAISED = {
    # A term's `when` in the first window (row 10) before the clause's `when` in the second (row 2,500).
    "a-terms-when-in-an-earlier-window": (
        "AND when ln(t.la) > 0: sum(x) + max(when sqrt(t.lb) > 0: y) <= 9 MAXIMIZE sum(x)",
        "square root of a negative number",
    ),
    # In one window, the clause's `when` (row 2) before its first term's own (row 1): group 0, then the term's.
    "the-clauses-when-before-its-first-terms": (
        "AND when ln(t.a) > 0: sum(when sqrt(t.b) > 0: x) + max(y) <= 9 MAXIMIZE sum(x)",
        "logarithm of zero",
    ),
    # In one window, term 0's `when` (row 2) before term 1's (row 1): each term's filter in the group after its
    # predecessor's.
    "a-terms-when-before-the-next-terms": (
        "AND sum(when ln(t.a) > -100: x) + max(when sqrt(t.b) > 0: y) <= 9 MAXIMIZE sum(x)",
        "logarithm of zero",
    ),
    # An objective of composed terms alone: flat's objective has no term to read its `when` with, so its terms'
    # filters read it, after the composed clauses (LogicalDecide::retained_objective_when).
    "an-objective-of-composed-terms-alone": (
        "MAXIMIZE when ln(t.la) > 0: max(x) + min(when sqrt(t.lb) > 0: y)",
        "square root of a negative number",
    ),
    # An objective with a term of its own reads its `when` with it, before every composed term.
    "an-objective-with-a-term-of-its-own": (
        "MAXIMIZE when ln(t.la) > 0: sum(x) + min(when sqrt(t.lb) > 0: y)",
        "logarithm of a negative number",
    ),
    # An objective that is checked to be produced once, by its `by (t.h)` (D72): the check reads its kept rows before
    # anything else.
    "an-objective-checked-to-be-produced-once": (
        "MAXIMIZE when ln(t.la) > 0: max(x) by (t.h) + min(when sqrt(t.lb) > 0: y)",
        "logarithm of a negative number",
    ),
}


@pytest.mark.parametrize("case", list(COMPOSED_WHEN_RAISED))
def test_a_composed_when_raises_in_its_terms_filters(decidb_cli, case):
    rest, message = COMPOSED_WHEN_RAISED[case]
    sql = f"SELECT id, x, y FROM {COMPOSED_WINDOWS} DECIDE x(REAL), y(REAL) SUCH THAT x <= 9 AND y <= 9 {rest}"
    stored = reference(decidb_cli, sql).stderr
    assert message in stored, stored
    assert run(decidb_cli, sql).stderr == stored


def test_a_square_objective_answers_under_tree(decidb_cli, tmp_path):
    """The objective's squares are a column per term of what each squares (OBJECTIVE_SQUARES), so `**` is formulated
    as flat formulates it: x lands on each row's c."""
    sql = """SELECT id, x FROM (VALUES (1, 3.0), (2, 5.0)) t(id, c) DECIDE x(REAL)
        SUCH THAT x <= 10 MINIMIZE sum((x - t.c) ** 2) ORDER BY id"""
    result = run(decidb_cli, sql)
    values = [round(float(value), 4) for value in re.findall(r"│\s+\d+ │\s+([\d.]+) │", result.stdout)]
    assert values == [3.0, 5.0], result.stdout
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


#: Four rows in two groups; NULL_POLYNOMIAL_ROWS has a NULL g on row 2 and a NULL c on row 4.
POLYNOMIAL_ROWS = """(VALUES (1, 'a', 3.0, 1.0, true), (2, 'a', 5.0, 2.0, false), (3, 'b', 1.0, 3.0, true),
    (4, 'b', 4.0, 2.0, true)) t(id, g, c, w, p)"""
NULL_POLYNOMIAL_ROWS = """(VALUES (1, 'a', 3.0, 1.0, true), (2, NULL, 5.0, 2.0, false), (3, 'b', 1.0, 3.0, true),
    (4, 'b', NULL, 2.0, true)) t(id, g, c, w, p)"""
#: A join that repeats t's rows 1 and 3, whose classes a relation-qualified reducer reads once.
POLYNOMIAL_JOIN = """(VALUES (1, 'a', 3.0, true), (2, 'a', 4.0, false), (3, 'b', 5.0, true), (4, NULL, 6.0, true))
    t(id, g, c, p) JOIN (VALUES (1, 'u'), (1, 'v'), (2, 'u'), (3, 'u'), (3, 'v'), (3, 'w'), (4, 'u')) s(id, k)
    ON t.id = s.id"""

#: Products and squares of decisions, which HiGHS refuses before a row is read, so these run on Gurobi: each covered
#: with flat's model, by the branch it reaches (DecideModelEmitter::Polynomial, Objective).
POLYNOMIALS = {
    # Stage 05 leaves a coefficient of 1 out: the product reads its sign, -1 here.
    "a-product-of-one": (POLYNOMIAL_ROWS, "x(REAL), y(REAL)", "y <= 9 AND 10 - x * y >= 0 MAXIMIZE sum(x + y)"),
    # A NULL coefficient skips its row's instance.
    "a-null-coefficient": (
        NULL_POLYNOMIAL_ROWS,
        "x(REAL), y(REAL)",
        "y <= 9 AND t.c * x * y <= 10 MAXIMIZE sum(x + y)",
    ),
    # A factor with no instance at a row (y's NULL key) skips its row's instance (D84).
    "a-factor-with-no-instance": (
        NULL_POLYNOMIAL_ROWS,
        "x(REAL), per t.g: y(REAL)",
        "y <= 9 AND t.w * x * y <= 10 MAXIMIZE sum(x) + sum(y)",
    ),
    # An AVG of products divides by its group's rows.
    "an-avg-of-products": (
        POLYNOMIAL_ROWS,
        "x(REAL), y(REAL)",
        "y <= 9 AND avg(t.c * x * y) by (t.g) <= 5 MAXIMIZE sum(x + y)",
    ),
    # A product that reads no row of group b leaves it no instance (D68).
    "a-product-over-no-row": (
        POLYNOMIAL_ROWS,
        "x(REAL), y(REAL)",
        "y <= 9 AND sum(when t.g = 'a': t.w * x * y) by (t.g) <= 5 MAXIMIZE sum(x + y)",
    ),
    # A relation-qualified product reads each class once in its group.
    "a-qualified-product": (
        POLYNOMIAL_JOIN,
        "per t.id: x(REAL), per t.id: y(REAL)",
        "y <= 9 AND sum(t: t.c * x * y) by (t.g) <= 20 MAXIMIZE sum(t: x + y)",
    ),
    "an-avg-of-squares": (POLYNOMIAL_ROWS, "x(REAL)", "avg(power(x - t.c, 2)) by (t.g) <= 5 MAXIMIZE sum(x)"),
    "a-square-over-no-row": (
        POLYNOMIAL_ROWS,
        "x(REAL)",
        "sum(when t.g = 'a': power(x - t.c, 2)) by (t.g) <= 5 MAXIMIZE sum(x)",
    ),
    "a-qualified-square": (
        POLYNOMIAL_JOIN,
        "per t.id: x(REAL)",
        "sum(t: power(x - t.c, 2)) by (t.g) <= 20 MAXIMIZE sum(t: x)",
    ),
    # A square's factor, read on the input's first row (ScaleSquaredGroup).
    "a-squares-factor": (POLYNOMIAL_ROWS, "x(REAL)", "sum(power(x - t.c, 2)) / (SELECT 2.0) <= 50 MAXIMIZE sum(x)"),
    # A square with no factor reads its sign, -1 here (SquareSign).
    "a-negated-square": (POLYNOMIAL_ROWS, "x(REAL)", "4 - power(x - t.c, 2) >= 0 MAXIMIZE sum(x)"),
    # A non-Boolean product in the objective, its coefficient of 1 left out.
    "an-objective-product": (POLYNOMIAL_ROWS, "x(REAL), y(REAL)", "x <= t.c AND y <= 9 MAXIMIZE sum(x * y)"),
    "a-qualified-objective-product": (
        POLYNOMIAL_JOIN,
        "per t.id: x(REAL), per t.id: y(REAL)",
        "x <= 9 AND y <= 9 MAXIMIZE sum(t: t.c * x * y)",
    ),
    # An AVG of filtered products divides by the rows its filter keeps, 3 of 4.
    "an-objective-avg-of-filtered-products": (
        POLYNOMIAL_ROWS,
        "x(REAL), y(REAL)",
        "x <= t.c AND y <= 9 MAXIMIZE avg(when t.p: t.w * x * y)",
    ),
    # An AVG of a `by` key divides by its own group's rows (D81).
    "an-objective-avg-of-products-by-key": (
        POLYNOMIAL_ROWS,
        "x(REAL), y(REAL)",
        "x <= t.c AND y <= 9 MAXIMIZE sum(per t.g: avg(t.w * x * y) by (t.g))",
    ),
    # A product outside every reducer counts once, on the first row the objective keeps.
    "an-objective-product-outside-the-reducers": (
        POLYNOMIAL_ROWS,
        "x(REAL), per (): y(REAL), per (): z(REAL)",
        "x <= 9 AND y <= 3 AND z <= 3 MAXIMIZE when t.id > 1: sum(x) + 2 * y * z",
    ),
}


@pytest.mark.parametrize("case", list(POLYNOMIALS))
def test_a_product_or_square_is_formulated_as_flat_formulates_it(decidb_cli_gurobi, tmp_path, case):
    rows, decisions, rest = POLYNOMIALS[case]
    sql = f"SELECT t.id FROM {rows} DECIDE {decisions} SUCH THAT x <= 9 AND {rest}"
    _modelled_as_flat(decidb_cli_gurobi, sql, tmp_path)


def test_a_null_square_factor_skips_its_clause(decidb_cli_gurobi, tmp_path):
    """A NULL factor makes every instance of its clause NULL, which is skipped and reported as flat reports it."""
    sql = f"""SELECT t.id FROM {POLYNOMIAL_ROWS} DECIDE x(REAL)
        SUCH THAT x <= 9 AND (SELECT NULL::DOUBLE) * sum(power(x - t.c, 2)) <= 50 MAXIMIZE sum(x)"""
    result = run(decidb_cli_gurobi, sql)
    assert "constrains nothing" in result.stderr
    assert result.stderr == reference(decidb_cli_gurobi, sql).stderr
    stored, tree = models(decidb_cli_gurobi, sql, tmp_path, ("constrains nothing",))
    assert tree == stored


def test_a_data_norm_is_folded_as_its_reducer(decidb_cli, tmp_path):
    """A norm of data folds its desugared reducer, a sum of squares here, in each group of its `by` key, and a NaN in
    its body is its data check's to refuse: it is covered with flat's model."""
    sql = f"SELECT t.id FROM {POLYNOMIAL_ROWS} DECIDE x(REAL) SUCH THAT x <= norm(t.c, 2) by (t.g) MAXIMIZE sum(x)"
    _modelled_as_flat(decidb_cli, sql, tmp_path)


def test_a_products_auxiliary_has_no_value_where_its_factor_has_none(decidb_cli, tmp_path):
    """A McCormick product's auxiliary has no value where its factor b has none, at row 2's NULL key
    (LogicalDecide::BilinearLink): the clause reading it skips that row, and stage 06 reads its absence."""
    sql = f"""SELECT t.id FROM {NULL_POLYNOMIAL_ROWS} DECIDE x(REAL), per t.g: b(BOOL)
        SUCH THAT x <= 9 AND b * x <= 5 MAXIMIZE sum(x) + sum(b)"""
    _modelled_as_flat(decidb_cli, sql, tmp_path)


#: A row defining an auxiliary that holds a product or a square: an ABS auxiliary's, or the helper per row a MIN or MAX
#: of a product takes, whose row reads the reducer's body on its members. The auxiliary has no value where flat says it
#: has none, and the helper's row stands only on a member, each covered with flat's model, by the branch it reaches
#: (DecideModelEmitter::ReadAbsence, Rows).
POLYNOMIAL_DEFINITIONS = {
    # x and y have no instance on row 2, its NULL g, so neither has |x * y - t.w|: `<= t.w` skips row 2.
    "a-products-factor-with-no-instance": (
        NULL_POLYNOMIAL_ROWS,
        "per t.g: x(REAL), per t.g: y(REAL)",
        "y <= 9 AND x >= 0 AND y >= 0 AND abs(x * y - t.w) <= t.w MAXIMIZE sum(y)",
    ),
    # x has no instance on row 2, which the square's term reads.
    "a-squares-term-with-no-instance": (
        NULL_POLYNOMIAL_ROWS,
        "per t.g: x(REAL)",
        "x >= 0 AND abs((x - t.w) ** 2 - 1) <= t.w MAXIMIZE sum(x)",
    ),
    # Row 4's NULL c is in a square's coefficient.
    "a-null-square-coefficient": (
        NULL_POLYNOMIAL_ROWS,
        "x(REAL)",
        "x >= 0 AND abs((t.c * x - 1) ** 2 - 1) <= 5 MAXIMIZE sum(x)",
    ),
    # The MAX's NULL test keeps row 4, its NULL c, from its members: its helper's row 4 is no model row, and its NULL
    # bound there leaves the helper a value.
    "a-helpers-row-on-its-members": (
        NULL_POLYNOMIAL_ROWS,
        "x(REAL), y(REAL)",
        "y <= 9 AND x >= 0 AND y >= 0 MINIMIZE max(x * y - t.c)",
    ),
    # Row 2's c, which a TINYINT cannot hold, is on a row its NULL tests drop, its NULL g: its cast never raises.
    "an-error-on-a-row-its-tests-drop": (
        "(VALUES (1, 'a', 3.0), (2, NULL, 1000.0), (3, 'b', 1.0)) t(id, g, c)",
        "per t.g: x(REAL), per t.g: y(REAL)",
        "y <= 9 AND x >= 0 AND y >= 0 MINIMIZE max(x * y - t.c::TINYINT)",
    ),
}


@pytest.mark.parametrize("case", list(POLYNOMIAL_DEFINITIONS))
def test_a_definition_holding_a_product_or_square_is_read_as_flat_reads_it(decidb_cli_gurobi, tmp_path, case):
    rows, decisions, rest = POLYNOMIAL_DEFINITIONS[case]
    sql = f"SELECT t.id FROM {rows} DECIDE {decisions} SUCH THAT x <= 9 AND {rest}"
    _modelled_as_flat(decidb_cli_gurobi, sql, tmp_path)


#: Three thousand rows: a is NaN and la is -1 on row 2,500, in the second window; b is NaN and lb is -1 on row 10, in
#: the first.
POLYNOMIAL_WINDOWS = """(SELECT i AS id, CASE WHEN i = 2500 THEN 'NaN'::DOUBLE ELSE 1.0 END AS a,
    CASE WHEN i = 10 THEN 'NaN'::DOUBLE ELSE 1.0 END AS b, CASE WHEN i = 2500 THEN -1.0 ELSE 1.0 END AS la,
    CASE WHEN i = 10 THEN -1.0 ELSE 1.0 END AS lb FROM range(3000) r(i)) t"""
#: Three rows: la is -1 on row 1 and lb on row 2.
POLYNOMIAL_FILTERS = "(VALUES (1, -1.0, 1.0), (2, 1.0, -1.0), (3, 1.0, 1.0)) t(id, la, lb)"

#: Each a product's or a square's error, raised where flat raises it, by the branch it reaches.
POLYNOMIAL_RAISED = {
    # A square's factor of 0, which the clause names.
    "a-factor-of-zero": (
        POLYNOMIAL_ROWS,
        "sum(power(x - t.c, 2)) / (SELECT 0.0) <= 9",
        "division by zero -- the factor on a squared aggregate",
    ),
    # A divisor whose reciprocal passes a DOUBLE names the clause as written, where the clause is read: before the
    # objective's logarithm of a negative number.
    "a-factor-past-a-double": (
        POLYNOMIAL_ROWS,
        "sum(power(x - t.c, 2)) / (SELECT 1e-320) <= 1e308 MAXIMIZE sum(ln(t.w - 2) * x)",
        "sum(power(x - t.c, 2)) / (SELECT 1e-320) <= 1e308 computes a number too large",
    ),
    # Square 0's NaN in the second window before square 1's NaN factor and its NaN in the first: flat reads each square
    # whole, in turn, its factor first.
    "a-square-before-the-next": (
        POLYNOMIAL_WINDOWS,
        "sum(power(t.a * x - 1, 2)) + (SELECT 'nan'::DOUBLE) * sum(power(t.b * y - 1, 2)) <= 5000",
        "t.a is NaN",
    ),
    # In one window, a term's filter (row 2) before a product's (row 1): the product's filter after the terms'.
    "a-terms-filter-before-a-products": (
        POLYNOMIAL_FILTERS,
        "sum(when sqrt(t.la) > -1: x * y) + sum(when ln(t.lb) > -100: x) <= 50",
        "logarithm of a negative number",
    ),
    # In one window, a product's filter (row 2) before a square's (row 1): the square's after the terms' and the
    # products'.
    "a-products-filter-before-a-squares": (
        POLYNOMIAL_FILTERS,
        "sum(x) + sum(when sqrt(t.la) > -1: power(x - 1, 2)) + sum(when ln(t.lb) > -100: x * y) <= 50",
        "logarithm of a negative number",
    ),
    # The objective's term (second window) before its product's filter (first window): flat reads the products'
    # filters after the objective's other terms.
    "an-objective-term-before-a-products-filter": (
        POLYNOMIAL_WINDOWS,
        "x <= 9 MAXIMIZE sum(t.a * x) + sum(when ln(t.lb) > -100: x * y)",
        "t.a is NaN",
    ),
    # The objective's product's filter (second window) before its coefficient (first window).
    "an-objective-products-filter-before-its-coefficient": (
        POLYNOMIAL_WINDOWS,
        "x <= 9 MAXIMIZE sum(x) + sum(when ln(t.la) > -100: t.b * x * y)",
        "logarithm of a negative number",
    ),
}


@pytest.mark.parametrize("case", list(POLYNOMIAL_RAISED))
def test_a_product_or_square_raises_flats_error(decidb_cli_gurobi, case):
    rows, clauses, message = POLYNOMIAL_RAISED[case]
    sql = f"SELECT t.id FROM {rows} DECIDE x(REAL), y(REAL) SUCH THAT x <= 9 AND y <= 9 AND {clauses}"
    stored = reference(decidb_cli_gurobi, sql).stderr
    assert message in stored, stored
    result = run(decidb_cli_gurobi, sql)
    assert result.stderr == stored


LINK_ROWS = """(VALUES (1, 'a', 3.0, true), (2, 'a', 5.0, false), (3, 'b', 1.0, false), (4, NULL, 2.0, true),
    (5, 'c', 4.0, true)) t(id, g, c, p)"""
LINK_JOIN = """(VALUES (1, 'a', 3.0), (2, 'a', 5.0), (3, 'b', 1.0)) t(id, g, c)
    JOIN (VALUES ('a', 1), ('a', 2), ('b', 3)) w(g, k) ON t.g = w.g"""

#: Each a helper decision of a reducer the value rewrite lifted (VALUE_LINKS), its classes the groups of its `by` key
#: that hold a member, formulated as flat formulates it, by the branch it reaches.
VALUE_LINKS = {
    # One group of every row: each row's cap reads it.
    "a-sum": (LINK_ROWS, "x(REAL)", "x <= 0.5 * sum(x) + 1"),
    # A row whose `by` key is NULL is computed on, as flat computes it, but is in no group: it lists no member and
    # reads no helper.
    "a-row-in-no-group": (LINK_ROWS, "x(REAL)", "x <= 0.5 * sum(t.c * x) by (t.g) + 1"),
    # Group b keeps no row for the reducer's `when`, so it has no helper and its rows read none: the groups are
    # numbered among those that hold a member.
    "a-group-with-no-member": (LINK_ROWS, "x(REAL)", "x <= sum(when t.p: x) by (t.g) + 1"),
    # Each member of an AVG divided by its group's members.
    "an-avg": (LINK_ROWS, "x(REAL)", "x >= avg(x) by (t.g) - 1"),
    # A relation-qualified reducer lists each class once in its group, at its first row: row a's two joined rows are
    # one member.
    "a-qualified-reducer": (LINK_JOIN, "per t: x(REAL)", "x <= 0.5 * sum(t: x) by (t.g) + 1"),
    # Group b's helper of `when t.p` has no member, so the auxiliary of `abs` has no value there, and the outer sum, a
    # helper too, lists no member of its rows: the inner helper's classes are read, then the auxiliary's definition,
    # then the outer helper's members.
    "a-member-reading-no-instance": (LINK_ROWS, "x(REAL)", "sum(abs(x - avg(when t.p: x) by (t.g))) <= 4"),
    # A product's factor, which no clause reads but through the product's auxiliary: its helper is a root of its own.
    "a-helper-no-clause-reads": (LINK_ROWS, "x(INT), b(BOOL)", "x + b * sum(when t.p: x) by (t.g) <= 5"),
}


@pytest.mark.parametrize("case", list(VALUE_LINKS))
def test_a_helper_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path, case):
    rows, decisions, clause = VALUE_LINKS[case]
    sql = f"SELECT t.id FROM {rows} DECIDE {decisions} SUCH THAT x <= 9 AND {clause} MAXIMIZE sum(x)"
    stored, tree = models(decidb_cli, sql, tmp_path)
    assert tree == stored


def test_a_helper_over_every_row_is_one_column(decidb_cli, tmp_path):
    """Each of 2,000 rows reads one helper of their sum: each row's two terms, x and the helper, and the helper's own
    row of its 2,000 members and itself, 6,001 terms, where writing the sum into every row made 4,000,000."""
    sql = "SELECT i FROM range(2000) r(i) DECIDE x(REAL) SUCH THAT x <= 9 AND x <= 0.01 * SUM(x) + 1 MAXIMIZE sum(x)"
    model = run_model(decidb_cli, sql, tmp_path)
    counts = [int(n) for n in re.findall(r"(?m)^row \d+: .*? nnz=(\d+)", model)]
    assert counts == [2] * 2000 + [2001] and sum(counts) == 6001, model[:300]


def test_a_helper_read_once_is_written_where_it_is_read(decidb_cli):
    """D89: stage 06 writes the helper's members where its one reader, the auxiliary of `abs`, reads it, and EXPLAIN
    ANALYZE accounts for it (PhysicalDecide::ReportInlinedHelpers), as under flat."""
    sql = f"""EXPLAIN ANALYZE SELECT id FROM {ROWS} DECIDE x(REAL)
        SUCH THAT x <= t.c AND sum(abs(x - avg(x) by (t.g))) <= 1 MAXIMIZE sum(x)"""
    accounts = [re.sub(r"[│\s]+", " ", result.stdout) for result in (reference(decidb_cli, sql), run(decidb_cli, sql))]
    assert "avg(x) by (t.g) is written where it is read, with no column" in accounts[1], accounts[1]
    assert re.sub(r"\d+\.\d+s", "", accounts[1]) == re.sub(r"\d+\.\d+s", "", accounts[0])


LINK_FAULTS = "(VALUES (1, 'a', 4.0, 1.0), (2, NULL, -1.0, 2.0), (3, 'b', 9.0, 3.0)) t(id, g, s, c)"
LINK_HUGE = "(VALUES (1, 'a', 1e308::DOUBLE, 1.0), (2, 'b', 1e308::DOUBLE, 2.0)) t(id, g, s, c)"
LINK_DEFINITIONS = """(SELECT i AS id, CASE WHEN i = 30 THEN -1.0 ELSE 1.0 END AS c,
    CASE WHEN i = 10 THEN 0.0 ELSE 1.0 END AS d, CASE WHEN i = 20 THEN -1.0 ELSE 1.0 END AS e FROM range(100) r(i)) t"""

#: Each a helper's error, raised where flat raises it, by the branch it reaches.
LINK_RAISED = {
    # Flat reads a helper's members' NULL tests where it builds its classes, before any clause: the member's square
    # root before the earlier clause's logarithm.
    "a-members-rows-before-the-clauses": (
        LINK_FAULTS,
        "ln(t.c - 2.5) * x <= 9 AND x <= 0.5 * sum(sqrt(t.s) * x) + 9",
        "cannot take square root of a negative number",
    ),
    # Row 2's `by` key is NULL, so it is in no group, but flat still computes the member there.
    "a-member-on-a-row-in-no-group": (
        LINK_FAULTS,
        "x <= sum(sqrt(t.s) * x) by (t.g) AND ln(t.c - 2.5) * x <= 9",
        "cannot take square root of a negative number",
    ),
    # Flat reads the helper's `by` key where it builds its classes, which the reading clause's own read of it keeps
    # the row for: its square root (row 2) before the earlier clause's logarithm (row 1).
    "a-helpers-by-key-before-the-clauses": (
        LINK_FAULTS,
        "ln(t.c - 2.5) * x <= 9 AND x <= 0.5 * sum(x) by (sqrt(t.s)) + 9",
        "cannot take square root of a negative number",
    ),
    # Flat reads the rows the helper's path keeps, the reading clause's `when`, where it builds its classes.
    "a-clause-when-before-the-clauses": (
        LINK_FAULTS,
        "ln(t.c - 2.5) * x <= 9 AND when sqrt(t.s) > 0: x <= 0.5 * sum(x) + 9",
        "cannot take square root of a negative number",
    ),
    # Of two helpers the clause's `when` gates, flat reads it at the first's classes: its square root (row 2) before
    # that helper's member's logarithm (row 1).
    "a-clause-when-at-its-first-helper": (
        LINK_FAULTS,
        "when sqrt(t.s) > 0: x <= 0.5 * sum(ln(t.c - 1.5) * x) + 0.5 * sum(x) by (t.g) + 9",
        "cannot take square root of a negative number",
    ),
    # Flat reads the helper's `by` key after its members' NULL tests: the member's square root (row 2) before the key's
    # logarithm (row 1).
    "a-helpers-by-key-after-its-members": (
        LINK_FAULTS,
        "x <= 0.5 * sum(sqrt(t.s) * x) by (ln(t.c - 1.5)) + 9",
        "cannot take square root of a negative number",
    ),
    # The helper's member reads the auxiliary of the inner `abs`, whose definition flat reads before the helper's
    # classes, and so before every other auxiliary's: its square root (row 30) before the first `abs`'s logarithm
    # (row 20).
    "an-auxiliary-a-helper-reads": (
        LINK_DEFINITIONS,
        "x + abs(x - ln(t.e)) <= 20 AND x + abs(sum(abs(x - sqrt(t.c))) - ln(t.d) * x) <= 20",
        "cannot take square root of a negative number",
    ),
    # The helper's member's NULL test (row 20) after the definition of the auxiliary the member reads (row 30).
    "a-helpers-rows-after-its-members-auxiliary": (
        LINK_DEFINITIONS,
        "x <= sum(ln(t.e) * abs(x - sqrt(t.c))) + 9",
        "cannot take square root of a negative number",
    ),
    # A member's coefficient past a DOUBLE names the helper's reducer as written, which every reader shares.
    "a-member-past-a-double": (
        LINK_HUGE,
        "t.c * sum(t.s * x + t.s * x) <= 10",
        "sum(t.s * x + t.s * x) computes a number too large for a DOUBLE",
    ),
}


@pytest.mark.parametrize("case", list(LINK_RAISED))
def test_a_helper_raises_flats_error(decidb_cli, case):
    rows, clauses, message = LINK_RAISED[case]
    sql = f"SELECT t.id FROM {rows} DECIDE x(REAL) SUCH THAT x <= 5 AND {clauses} MAXIMIZE sum(x)"
    stored = reference(decidb_cli, sql).stderr
    assert message in stored, stored
    result = run(decidb_cli, sql)
    assert result.stderr == stored


#: Rows for the frames' arm: ordered by id, g's timelines a (rows 1, 2) and b (row 4), row 3's g NULL, so it lies in
#: none; k's classes u and v, row 2's NULL.
FRAME_ROWS = """(VALUES (1, 'a', 'u', 3.0::DOUBLE, 2.0::DOUBLE), (2, 'a', NULL, 4.0::DOUBLE, 1.0::DOUBLE),
    (3, NULL, 'v', 5.0::DOUBLE, 3.0::DOUBLE), (4, 'b', 'v', 6.0::DOUBLE, 4.0::DOUBLE)) t(id, g, k, c, w)"""

#: Each a clause reading a frame (language/guide.md §5.6) the tree formulates as flat does, by the branch it reaches:
#: the frame's map is built from its O's timelines (DecideTreeColumns::FrameMap), its columns through flat's blocks
#: over programs on the O (ComputeDecideFrameColumns), and a reader reads one as a hole of its layout.
FRAMES = {
    # Row 3 selects row 2, whose k is NULL, so z has no instance there and row 3's instance no value.
    "a-selected-row-with-no-instance": "x - at(previous: z) over (t.id) <= 1",
    # A range over data folds its reducer's argument, a program on the O, per window.
    "a-range-over-data": "x <= sum(from previous to next: t.c) over (t.id)",
    # A range over decisions is a helper per window (DecideFrameWindowMapping), its members listed by flat's block
    # (DecideListFrameMembers), its `else` terms reading the O's own FALLBACK column.
    "a-range-over-decisions": "sum(from previous to next else 1: x) over (t.id) <= 7",
    # A reducer over data reading a frame is its G's fold, whose operand reads the frame's value (FrameRead).
    "a-reducer-over-a-frame": "x <= sum(at(previous else 1: t.c) over (t.id) * t.w) by (t.g)",
    # A reducer's NULL test of a frame over decisions reads its DEFINED column.
    "a-null-test-of-a-frame-over-decisions": "sum(at(previous: x) over (t.id)) <= 5",
    # Row 3 selects row 2, whose z has no instance, so the objective drops its term there (ReadEntries), the
    # definition of the auxiliary has no value there (DefinitionAbsence), and the helper lists no member there
    # (Members).
    "an-objective-term-at-a-row-with-no-instance": "y <= 9 MAXIMIZE sum(at(previous: z) over (t.id))",
    "a-definition-at-a-row-with-no-instance": "abs(x - at(previous: z) over (t.id)) <= 1",
    "a-helpers-member-at-a-row-with-no-instance": "x <= 0.5 * sum(at(previous: z) over (t.id)) by (t.g) + 1",
    # A row whose bound is NULL bounds nothing: group a's row 2 binds, and group b, whose one row reads none, has no
    # instance (GroupBound).
    "a-null-anchor-binds-nothing": "sum(x) by (t.g) <= at(previous: t.c) over (t.id within t.g)",
    # Row 2 is in no class of `per t.k:`, so it bounds nothing, nor reads its bound (GroupBound): each group's other
    # rows read none, so neither has an instance.
    "a-tightest-bound-under-per": "per t.k: sum(x) by (t.g) <= at(previous: t.c) over (t.id within t.g)",
    # A NULL factor on a reducer's member leaves its group no value (ReadInReducer), and so beside the reducers
    # (ReadBesideReducers).
    "a-null-factor-in-a-reducer": "(SELECT NULL::DOUBLE) * sum(x) by (t.g) <= 5",
    "a-null-factor-beside-the-reducers": "sum(x) by (t.g) + (SELECT NULL::DOUBLE) * y <= 5",
}
#: The cases whose clause has no instance left, which warns so.
FRAMES_SKIPPED = ("a-tightest-bound-under-per", "a-null-factor-in-a-reducer", "a-null-factor-beside-the-reducers")


@pytest.mark.parametrize("case", list(FRAMES))
def test_a_frame_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path, case):
    clause, _, objective = FRAMES[case].partition(" MAXIMIZE ")
    sql = f"""SELECT id, x FROM {FRAME_ROWS} DECIDE x(REAL), per (): y(REAL), per t.k: z(REAL)
        SUCH THAT x <= 9 AND y <= 9 AND z <= 9 AND {clause} MAXIMIZE {objective or 'sum(x) + y + sum(z)'}"""
    assert run(decidb_cli, sql).stderr == reference(decidb_cli, sql).stderr
    stored, tree = models(decidb_cli, sql, tmp_path, ("constrains nothing",) if case in FRAMES_SKIPPED else ())
    assert tree == stored


#: The rows of test_anr_frames.py's points: product A's periods 1, 2, 3 and B's 1, 2.
EDGE_ROWS = "(VALUES (1, 'A', 1, 4, 2), (2, 'A', 2, 6, 1), (3, 'A', 3, 1, 3), (4, 'B', 1, 7, 2), (5, 'B', 2, 3, 1))"
#: Product A's first period holds two rows, which read their own `t.w` where they reach the `else`.
SPLIT_ROWS = "(VALUES (1, 'A', 1, 4, 5), (2, 'A', 1, 4, 2), (3, 'A', 2, 6, 1), (4, 'B', 1, 7, 3))"
#: Product A's one period holds two rows, and no row reads it as a `previous`.
SPLIT_ONCE_ROWS = "(VALUES (1, 'A', 1, 4, 0), (2, 'A', 1, 2, 0), (3, 'B', 1, 7, 0))"

#: Each a clause in the per-row shape under `per t.product:` whose signature reads a frame (D82), by the branch it
#: reaches: its variants number by first row, an anchor where the frame reads nothing is none
#: (DecideModelEmitter::Variants), and the frame splits them by its keys (Identity) and its `else`'s reads (Fallback);
#: each named by its frame's keys (DecideBody::names), which verify compares.
FRAME_VARIANTS = {
    # The same rows with their periods descending, and a frame its signature's one field: the variants number by
    # their first rows, not by position.
    "a-position-in-row-order": (
        "(VALUES (1, 'A', 3, 1, 3), (2, 'A', 2, 6, 1), (3, 'A', 1, 4, 2), (4, 'B', 2, 3, 1), (5, 'B', 1, 7, 2))",
        "x(INT), per (): y(INT)",
        "per t.product: y <= at(previous: t.v) over (t.period within t.product)",
    ),
    # Row 3 lies in no timeline, so it is no variant, and spans none of class t.w = 2 (D75): the O holds it as an
    # anchor that reads nothing.
    "an-anchor-in-no-timeline": (
        "(VALUES (1, 'A', 1, 4, 2), (2, 'A', 2, 6, 2), (3, NULL, 1, 9, 2))",
        "x(INT)",
        "per t.w: x >= at(first: t.v) over (t.period within t.product)",
    ),
    # Each reaches an `else` reading its own row's x: two variants.
    "a-reached-decision-else": (
        SPLIT_ONCE_ROWS,
        "x(INT), per t.product: y(INT)",
        "x <= t.v AND per t.product: y <= at(previous else x: x) over (t.period within t.product)",
    ),
    # Each reaches an `else` reading z of its row's class, the one class t.w = 0: one variant of A.
    "a-reached-keyed-decision-else": (
        SPLIT_ONCE_ROWS,
        "x(INT), per t.product: y(INT), per t.w: z(INT)",
        "z <= 9 AND per t.product: y <= at(previous else z: x) over (t.period within t.product)",
    ),
    # Class A's one anchor reads nothing, so it holds no variant but is still a class, as flat counts them: two
    # variants of B from two classes, each named by its class alone.
    "a-class-whose-anchors-read-nothing": (
        "(VALUES (1, 'B', 1, 4, 0), (2, 'B', 2, 6, 0), (3, 'B', 3, 6, 0), (4, 'A', 1, 7, 0))",
        "x(INT), per t.product: y(INT)",
        "per t.product: y <= at(previous: t.v) over (t.period within t.product)",
    ),
    # A value field after a frame's, read after the frame's timeline and order key in the signature (SignatureStarts):
    # period 2's two rows, w 1 and 5, are two variants.
    "a-value-after-a-frame": (
        "(VALUES (1, 'A', 1, 4, 1), (2, 'A', 2, 6, 1), (3, 'A', 2, 6, 5), (4, 'A', 3, 2, 2))",
        "x(INT), per t.product: y(INT)",
        "per t.product: y <= at(previous: t.v) over (t.period within t.product) + t.w",
    ),
}


@pytest.mark.parametrize("case", list(FRAME_VARIANTS))
def test_a_frames_variants_are_flats(decidb_cli, tmp_path, case):
    rows, decisions, clause = FRAME_VARIANTS[case]
    sql = f"""SELECT id, x FROM {rows} t(id, product, period, v, w) DECIDE {decisions}
        SUCH THAT x <= 9 AND {clause} MAXIMIZE sum(x)"""
    _modelled_as_flat(decidb_cli, sql, tmp_path)
    _verified(decidb_cli, sql)


def test_a_collated_position_is_one_variant_named_by_its_least_spelling(decidb_cli):
    """'a' and 'A' are one position under NOCASE, so one variant (the frame's peer, not its spelling), which DIAGNOSE
    names 'g, A' (DecideBody::position_names)."""
    sql = """PRAGMA diagnose_decide_infeasible_slack_scope='expanded';
        DIAGNOSE SELECT id, y FROM (SELECT id, 'g' AS g, k COLLATE NOCASE AS k, v FROM
        (VALUES (1, 'a', 10.0), (2, 'A', 10.0), (3, 'b', 30.0)) r(id, k, v)) t DECIDE per t.g: y(INT)
        SUCH THAT per t.g: y = at(previous else 5: t.v) over (t.k) MAXIMIZE sum(y)"""
    tree = run(decidb_cli, sql)
    assert "g, A" in tree.stdout and tree.stdout == reference(decidb_cli, sql).stdout, tree.stdout + tree.stderr


#: Each an error a frame raises, which the tree raises as flat does, by the branch it reaches.
FRAME_RAISED = {
    # The objective reads two positions (D72; DecideModelEmitter::ObjectiveDifference).
    "the-objective-reads-two-positions": (
        EDGE_ROWS,
        "x <= 9",
        "MAXIMIZE when t.product = 'A': at(previous: x) over (t.period within t.product)",
        "reads more than one position over the kept rows (t.period = 2, 3)",
    ),
    # The objective's one class spans two timelines, which D75 refuses first: the D72 message the tree also builds
    # names them, rather than finding its variants agree on every field.
    "the-objective-spans-two-timelines": (
        EDGE_ROWS,
        "x <= 9",
        "MAXIMIZE at(first: x) over (t.period within t.product)",
        "does not determine the timeline t.product of at(first: x)",
    ),
    # Two rows of one position reach the `else`, reading 5 and 2.
    "the-objective-falls-back-twice": (
        SPLIT_ROWS,
        "x <= 9",
        "MAXIMIZE when t.id <= 2: at(previous else t.w: x) over (t.period within t.product)",
        "falls back to more than one value over the kept rows",
    ),
    # They read 5 and a NULL, which is a value of its own as flat groups it (DecideFrameDifference's fallbacks).
    "the-objective-falls-back-to-a-null": (
        "(VALUES (1, 'A', 1, 4, 5), (2, 'A', 1, 4, NULL), (3, 'A', 2, 6, 1), (4, 'B', 1, 7, 3))",
        "x <= 9",
        "MAXIMIZE when t.id <= 2: at(previous else t.w: x) over (t.period within t.product)",
        "falls back to more than one value over the kept rows",
    ),
    # A position frame 1 reads holds two payloads (D73), which flat checks before it reads any clause: before clause
    # 0's square root of row 1.
    "a-position-of-two-payloads": (
        EDGE_ROWS,
        "x <= sqrt(t.v - 5) AND x <= at(next else 0: t.v) over (t.id) + at(previous else 0: t.v) over (t.period)",
        "MAXIMIZE sum(x)",
        "finds 2 rows at t.period = 1",
    ),
    # And after every frame's range limit and partition: frame 1's limit, which ranks in FRAMES, before frame 0's D73
    # (FRAME_PEERS).
    "a-later-frames-limit-before-a-position": (
        EDGE_ROWS,
        "x <= at(previous else 0: t.v) over (t.period) AND x <= sum(from first to last: t.v) over (t.id)",
        "MAXIMIZE sum(x)",
        "reads more than 3 values over its ranges",
    ),
    # Class t.w = 2 spans two timelines (D75).
    "a-class-spanning-two-timelines": (
        EDGE_ROWS,
        "per t.w: x <= at(first: t.v) over (t.period within t.product)",
        "MAXIMIZE sum(x)",
        "does not determine the timeline t.product",
    ),
    # The order key raises on row 3, which flat reads before it lists the range's windows.
    "an-order-key-before-its-limit": (
        EDGE_ROWS,
        "x <= sum(from first to last: t.v) over (ln(t.v - 2))",
        "MAXIMIZE sum(x)",
        "cannot take logarithm of a negative number",
    ),
    # Row 3's order key raises where clause 1 reads the frame, and flat reads every frame's order key before any
    # clause: before clause 0's square root of row 2. The clause's signature reads the frame keeping the row, so the
    # frame's O raises it.
    "an-order-key-before-an-earlier-clause": (
        EDGE_ROWS,
        "x <= sqrt(t.w - 2) AND x <= at(previous else 0: t.v) over (ln(t.v - 2))",
        "MAXIMIZE sum(x)",
        "cannot take logarithm of a negative number",
    ),
    # A range's helper's members are read after every clause (VALUE_LINKS): clause 0's square root of row 1 before
    # the member's logarithm of row 5.
    "a-clause-before-a-range-helpers-member": (
        EDGE_ROWS,
        "x <= sqrt(t.v - 5) AND sum(from last to last: ln(t.w - 1.5) * x) over (t.period within t.product) <= 7",
        "MAXIMIZE sum(x)",
        "cannot take square root of a negative number",
    ),
    # Row 2's payload raises, which row 3 reads.
    "a-payload-read-where-it-raises": (
        EDGE_ROWS,
        "x <= at(previous: ln(t.v - 5)) over (t.period within t.product)",
        "MAXIMIZE sum(x)",
        "cannot take logarithm of a negative number",
    ),
    # An `=` bound taking two values in a group raises at its clause (TakeTightestFrameBound), after clause 0's
    # logarithm of row 1.
    "an-earlier-clause-before-an-equal-bound": (
        EDGE_ROWS,
        "x <= ln(t.v - 5) AND sum(x) by (t.product) = at(first: t.v) over (t.period within t.product) + t.period",
        "MAXIMIZE sum(x)",
        "cannot take logarithm of a negative number",
    ),
}


@pytest.mark.parametrize("case", list(FRAME_RAISED))
def test_a_frame_raises_flats_error(decidb_cli, case):
    rows, clause, objective, message = FRAME_RAISED[case]
    sql = f"""SET decide_frame_limit=3; SELECT id, x FROM {rows} t(id, product, period, v, w) DECIDE x(INT)
        SUCH THAT {clause} {objective}"""
    stored = reference(decidb_cli, sql).stderr
    assert message in stored, stored
    assert run(decidb_cli, sql).stderr == stored


#: Each a frame refusal naming a NOCASE `within` key's timeline, by the branch it reaches, and the spelling each
#: evaluator names it by: flat the one at a row of the timeline, the tree the key's least (D69). Which one is right is
#: open (D12), so verify allows the two runs to raise differently.
COLLATED_FRAME_RAISED = {
    # Position b, 1 holds rows 1 and 2 (D73; DecideTreeColumns::CheckPositions).
    "a-position-of-two-payloads": (
        """(SELECT id, g COLLATE NOCASE AS g, k, p, v FROM (VALUES (1, 'b', 1, 1, 4.0), (2, 'B', 1, 1, 5.0),
        (3, 'b', 1, 2, 6.0), (4, 'a', 1, 1, 7.0)) r(id, g, k, p, v)) t""",
        "x <= at(previous else 0: t.v) over (t.p within t.g)",
        ("finds 2 rows at t.g = 'b', t.p = 1", "finds 2 rows at t.g = 'B', t.p = 1"),
    ),
    # Class t.k = 1 spans timelines b and a (D75; CheckPartition).
    "a-class-spanning-two-timelines": (
        """(SELECT id, g COLLATE NOCASE AS g, k, p, v FROM (VALUES (1, 'b', 1, 1, 4.0), (2, 'B', 2, 2, 5.0),
        (3, 'a', 1, 1, 6.0), (4, 'A', 1, 2, 7.0)) r(id, g, k, p, v)) t""",
        "per t.k: x <= at(first: t.v) over (t.p within t.g)",
        ("t.k = 1 spans t.g = 'b', 'a'", "t.k = 1 spans t.g = 'B', 'A'"),
    ),
}


@pytest.mark.parametrize("case", list(COLLATED_FRAME_RAISED))
def test_a_collated_timeline_is_named_by_each_evaluators_spelling(decidb_cli, case):
    rows, clause, (flat, tree) = COLLATED_FRAME_RAISED[case]
    sql = f"SELECT id, x FROM {rows} DECIDE x(REAL) SUCH THAT x <= 9 AND {clause} MAXIMIZE sum(x)"
    assert flat in reference(decidb_cli, sql).stderr
    assert tree in run(decidb_cli, sql).stderr


#: Each a frame that raises nothing where nothing reads its error, by the branch it reaches.
FRAME_UNRAISED = {
    # Row 1's payload raises, which no row reads: row 1 is no row's `next`.
    "a-payload-no-row-reads": "x <= at(next: ln(t.v - 5)) over (t.period within t.product)",
    # Position w = 2 holds rows 1 and 5, which read different payloads, but `first` reads w = 1 alone (D73).
    "a-position-no-row-reads": "x <= at(first: t.v) over (t.w)",
}


@pytest.mark.parametrize("case", list(FRAME_UNRAISED))
def test_a_frame_raises_nothing_no_read_reaches(decidb_cli, case):
    sql = f"""SELECT id, x FROM (VALUES (1, 'A', 'a', 1, 4, 2), (2, 'A', 'a', 2, 6, 1), (3, 'A', 'a', 3, 6, 3),
        (4, 'B', 'b', 1, 7, 4), (5, 'B', NULL, 2, 8, 2)) t(id, product, g, period, v, w) DECIDE x(REAL)
        SUCH THAT x <= 9 AND {FRAME_UNRAISED[case]} MAXIMIZE sum(x)"""
    tree = run(decidb_cli, sql)
    assert tree.returncode == 0 and tree.stderr == "", tree.stderr
    assert tree.stdout == reference(decidb_cli, sql).stdout


#: Each a reducer over data reading a frame, which no data check reads, so its fold refuses a NaN itself
#: (DecideTreeColumns::Folds), by the branch it reaches.
FRAME_FOLDS = {
    # A norm's definition hides its body's NaN (SIGN reads it as 0), so its fold reads its body.
    "a-norms-body": ("x <= norm(at(first: t.f) over (t.id within t.id), 0)", "reads a NaN on some rows"),
    # A count counts a NaN.
    "a-count": ("x <= count(at(first: t.f) over (t.id within t.id))", ""),
    # The fold refuses it before any clause is read: before clause 1's square root of row 1.
    "a-nan-before-a-clause": (
        "x <= sqrt(t.f - 3) AND x <= min(at(first: t.f) over (t.id within t.id))",
        "reads a NaN on some rows",
    ),
}


@pytest.mark.parametrize("case", list(FRAME_FOLDS))
def test_a_reducer_over_a_frame_refuses_a_nan(decidb_cli, case):
    clause, message = FRAME_FOLDS[case]
    sql = f"""SELECT id, x FROM (VALUES (1, 1.0::DOUBLE), (2, 'NaN'::DOUBLE), (3, 4.0::DOUBLE)) t(id, f)
        DECIDE x(REAL) SUCH THAT x <= 100 AND {clause} MAXIMIZE sum(x)"""
    tree, stored = run(decidb_cli, sql), reference(decidb_cli, sql)
    assert message in tree.stderr and (message or tree.stderr == ""), tree.stderr
    assert (tree.stdout, tree.stderr) == (stored.stdout, stored.stderr)
