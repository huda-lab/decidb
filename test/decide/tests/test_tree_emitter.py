"""The tree's model emitter (language/guide.md §6.4): under DECIDB_EVALUATOR=tree a query whose every shape has an arm is
evaluated by the tree -- its programs computed over its nodes' entries -- and formulated through flat's code path, and a
query with a shape that has none takes the interim formulator. Each test names the branch it reaches."""

import json
import os
import subprocess

import pytest

ROWS = "(VALUES (1, 'a', 3.0::DOUBLE), (2, 'a', 4.0::DOUBLE), (3, 'b', 5.0::DOUBLE)) t(id, g, c)"
#: Every clause in the per-row shape and no objective: the shapes with an arm.
ROWS_ONLY = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= t.c AND x + 1 >= 2"
PLAIN = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= t.c MAXIMIZE sum(x)"
#: A clause in the aggregate shape, which has no arm yet.
NO_ARM = f"SELECT id, x FROM {ROWS} DECIDE x(REAL) SUCH THAT x <= t.c AND sum(x) by (t.g) <= 5 MAXIMIZE sum(x)"


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


def coverage(cli, sql, tmp_path, evaluator="tree"):
    """What DECIDB_TREE_COVERAGE_LOG says of the query: `covered`, or the counter and the reason it has no arm."""
    log = tmp_path / "coverage.log"
    result = run(cli, sql, evaluator, DECIDB_TREE_COVERAGE_LOG=str(log))
    return log.read_text().splitlines(), result


def models(cli, sql, tmp_path):
    """The model flat builds and the one the tree builds."""
    return [
        cli.with_env({"DECIDB_EVALUATOR": evaluator}).dump_model(sql, tmp_path / f"{evaluator}.dump")
        for evaluator in ("flat", "tree")
    ]


def test_a_query_with_an_arm_is_formulated_as_flat_formulates_it(decidb_cli, tmp_path):
    """The covered branch: the tree emits the clauses, so no query takes the interim formulator. Its descent counts
    the rows it read."""
    result = run(decidb_cli, ROWS_ONLY, DECIDB_PROFILE="1")
    assert result.returncode == 0, result.stderr
    assert counters(result, "execution.tree_fallback") == [], result.stderr
    assert counters(result, "execution.descent_rows") == [3], result.stderr
    flat, tree = models(decidb_cli, ROWS_ONLY, tmp_path)
    assert tree == flat


def test_a_query_with_no_arm_takes_the_interim_formulator(decidb_cli):
    """The uncovered branch: a clause in the aggregate shape has no arm yet."""
    result = run(decidb_cli, NO_ARM, DECIDB_PROFILE="1")
    assert counters(result, "execution.tree_fallback") == [1], result.stderr
    flat = run(decidb_cli, f"{NO_ARM} ORDER BY id", "flat").stdout
    assert run(decidb_cli, f"{NO_ARM} ORDER BY id").stdout == flat


#: One query per shape with no arm yet, each its query's first: the shape Uncovered names (a closed set that grows only
#: with stage 06), then what the tree computes no value for.
UNCOVERED = {
    "TIGHTEST_BOUND": "x(REAL) SUCH THAT x <= 9 AND sum(x) by (t.g) <= at(previous else 9: t.c) over (t.id within t.g)",
    "NOT_EQUAL_ROWS": "x(INT) SUCH THAT x <= 9 AND x <> 3",
    "DEFINED_READS": "x(REAL) SUCH THAT x <= 9 AND x + abs(x - t.c) <= 20",
    "EXTREMUM_ROWS": "x(REAL) SUCH THAT x <= 9 AND max(x) by (t.g) >= 2",
    "MEMBER_ROWS": "x(REAL) SUCH THAT x <= 9 AND max(x) by (t.g) <= 2",
    "GROUP_ROWS": "x(REAL) SUCH THAT x <= 9 AND sum(x) by (t.g) <= 5",
    "CLASS_ROWS": "x(REAL), y(REAL) SUCH THAT x <= 9 AND per t.g: x + y <= 5",
    "OBJECTIVE_SQUARES": "x(REAL) SUCH THAT x <= t.c MINIMIZE sum(power(x - t.c, 2))",
    "OBJECTIVE_EXTREMUM": "x(REAL) SUCH THAT x <= t.c MINIMIZE max(x)",
    "OBJECTIVE_COMPOSED": "x(REAL) SUCH THAT x <= t.c MAXIMIZE max(x) + min(x)",
    "COMPOSED": "x(REAL) SUCH THAT x <= 9 AND sum(x) + max(x) <= 10",
    "VALUE_LINKS": "x(REAL) SUCH THAT x <= 9 AND x <= sum(x) by (t.g)",
    "FRAME_READS": "x(REAL) SUCH THAT x <= 9 AND x <= at(previous else 1: x) over (t.id)",
    "PRODUCT_LINKS": "x(REAL), b(BOOL) SUCH THAT x <= 9 AND b * x <= 5",
    # t.c takes several values in a class of t.g, so the reducer reads one value per class and value of t.c.
    "KEYED_SIGNATURE": "x(REAL), per t.g: y(REAL) SUCH THAT x + y <= t.c MAXIMIZE sum(per t.g: t.c * y)",
    # A bound's data reducer of DOUBLE values is checked for NaN first, by a hidden root; one of DECIMAL values is not.
    "data value check": "x(REAL) SUCH THAT x <= max(t.c) by (t.g)",
    "reads a data reducer": "x(REAL) SUCH THAT x <= max(t.c::DECIMAL(4, 1)) by (t.g)",
}

#: The shapes that multiply or square decisions in a clause, or multiply them in the objective, which HiGHS refuses
#: before the query reads a row.
GUROBI_UNCOVERED = {
    "PRODUCT_TERMS": "x(REAL), y(REAL) SUCH THAT x <= 9 AND y <= 9 AND t.c * x * y <= 10",
    "SQUARE_TERMS": "x(REAL) SUCH THAT x <= 9 AND power(x - t.c, 2) <= 50",
    "OBJECTIVE_PRODUCTS": "x(REAL), y(REAL) SUCH THAT x <= t.c AND y <= 9 MAXIMIZE sum(x * y)",
}


def _assert_named(cli, shapes, shape, tmp_path):
    lines, result = coverage(cli, f"SELECT id FROM {ROWS} DECIDE {shapes[shape]}", tmp_path)
    assert lines == [f"execution.tree_fallback: {shape}"], result.stderr


@pytest.mark.parametrize("shape", list(UNCOVERED))
def test_a_shape_with_no_arm_is_named(decidb_cli, shape, tmp_path):
    _assert_named(decidb_cli, UNCOVERED, shape, tmp_path)


@pytest.mark.parametrize("shape", list(GUROBI_UNCOVERED))
def test_a_product_or_square_with_no_arm_is_named(decidb_cli_gurobi, shape, tmp_path):
    _assert_named(decidb_cli_gurobi, GUROBI_UNCOVERED, shape, tmp_path)


def test_a_covered_query_is_logged_covered(decidb_cli, tmp_path):
    lines, result = coverage(decidb_cli, ROWS_ONLY, tmp_path)
    assert lines == ["covered"], result.stderr


def test_a_null_coefficient_skips_its_row(decidb_cli, tmp_path):
    """A NULL coefficient on a kept row leaves its instance with no value (D67): row 2 has no model row, y's term
    included."""
    sql = """SELECT id, x, y FROM (VALUES (1, 2.0), (2, NULL), (3, 4.0)) t(id, w)
        DECIDE x(REAL), y(REAL) SUCH THAT x <= 10 AND t.w * x + y <= 8"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_keyed_decision_with_no_class_skips_its_row(decidb_cli, tmp_path):
    """A decision declared `per t.g` has no instance at a row whose g is NULL (D84): the row reads none."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 3.0), (2, NULL, 4.0), (3, 'b', 5.0)) t(id, g, c)
        DECIDE per t.g: x(INT) SUCH THAT x <= 10 AND x + 0 * t.c <= t.c"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_rows_in_no_class_share_one_absent_class(decidb_cli, tmp_path):
    """The second row with a NULL g joins the class the first opened (KeyedMapping::absent_class): x has the
    instances flat gives it, one per class and one for both rows."""
    sql = """SELECT id, x FROM (VALUES (1, 'a', 3.0), (2, NULL, 4.0), (3, NULL, 5.0), (4, 'b', 6.0)) t(id, g, c)
        DECIDE per t.g: x(INT) SUCH THAT x <= 10 AND x + 0 * t.c <= t.c"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


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
    flat = run(decidb_cli, sql, "flat")
    tree = run(decidb_cli, sql)
    assert message in flat.stderr, flat.stderr
    assert tree.stderr == flat.stderr


#: Clause 0 holds no value on any row, which flat reports once its clauses are read, unless one of them raised.
NOTHING = (
    "SELECT id, x FROM (VALUES (1, NULL::DOUBLE, 1.0), (2, NULL, {d})) t(id, n, d) "
    "DECIDE x(REAL) SUCH THAT x + 2 <= t.n AND x + 1 <= t.d"
)


@pytest.mark.parametrize("d,warned", [("2.0", True), ("'NaN'::DOUBLE", False)], ids=["no-error", "a-raised-error"])
def test_skipped_instances_are_reported_unless_a_clause_raised(decidb_cli, d, warned):
    result = run(decidb_cli, NOTHING.format(d=d))
    assert ("constrains nothing" in result.stderr) == warned, result.stderr
    assert result.stderr == run(decidb_cli, NOTHING.format(d=d), "flat").stderr


def test_a_folded_bounds_repair_rows_are_emitted_under_diagnose(decidb_cli):
    """`x >= 4` and `x <= 2` are folded into x's box; only the repair model has their rows."""
    sql = """DIAGNOSE SELECT id, x FROM (VALUES (1, 3.0), (2, 4.0)) t(id, c)
        DECIDE x(INT), y(INT) SUCH THAT x >= 4 AND x <= 2 AND x + y <= 20"""
    flat = run(decidb_cli, sql, "flat")
    assert "x >= 4" in flat.stdout, flat.stdout
    assert run(decidb_cli, sql).stdout == flat.stdout


@pytest.mark.parametrize("c", ["5.0", "NULL"], ids=["a-bound-on-every-row", "a-null-bound"])
def test_an_admitted_bound_folds_flats_box(decidb_cli, tmp_path, c):
    """`x <= t.c` admits a range of x on each row, which the model builder folds into x's column; a NULL bound leaves
    its row with no value."""
    sql = f"SELECT id, x FROM (VALUES (1, 3.0), (2, {c}), (3, 4.0)) t(id, c) DECIDE x(REAL) SUCH THAT x <= t.c AND x + 1 >= 2"
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_null_in_a_later_window_marks_its_own_entry(decidb_cli, tmp_path):
    """Row 2,500's NULL bound is read in the second window of 2,048 entries, which marks the entry at its offset in the
    node, not in the window: row 2,500 has no value, and row 452 keeps its range."""
    sql = """SELECT id, x FROM (SELECT i AS id, CASE WHEN i = 2500 THEN NULL ELSE 5.0 END AS c FROM range(3000) r(i)) t
        DECIDE x(REAL) SUCH THAT x <= t.c AND x + 1 >= 2"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_folded_bound_with_no_repair_rows_is_no_clause(decidb_cli, tmp_path):
    """A BOOLEAN's bound is its box, with no rows even for a repair model: its root has no clause to emit."""
    sql = """SELECT id, x, b FROM (VALUES (1, 2.0), (2, 5.0)) t(id, c)
        DECIDE x(REAL), b(BOOL) SUCH THAT x + 1 <= t.c AND b <= 1 AND x + b <= 8"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_covered_query_prints_each_dump_it_is_asked_for(decidb_cli):
    """The bottom-up values are the interim interpreter's, which runs for its dump alone."""
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
    result = run(decidb_cli, PLAIN, DECIDB_PROFILE="1")
    assert counters(result, "execution.tree_fallback") == [], result.stderr
    flat, tree = models(decidb_cli, PLAIN, tmp_path)
    assert tree == flat


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
    result = run(decidb_cli, sql, DECIDB_PROFILE="1")
    assert counters(result, "execution.tree_fallback") == [], result.stderr
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_qualified_reducer_reads_one_row_per_class(decidb_cli, tmp_path):
    """`sum(per t.g: y)` reads y once per class of t.g: its operand holds one entry per class, rows 1 and 3, the rows
    flat's qualifier keeps."""
    sql = (
        f"SELECT id, x, y FROM {ROWS} DECIDE x(REAL), per t.g: y(REAL) SUCH THAT x + y <= t.c MAXIMIZE sum(per t.g: y)"
    )
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_member_test_keeps_its_terms_from_being_read(decidb_cli, tmp_path):
    """Row 2's `t.v IS NOT NULL` fails, so flat reads no term of the reducer there, and its overflowing coefficient
    raises nothing."""
    sql = """SELECT id, x FROM (VALUES (1, 2::BIGINT, 1::BIGINT, 1.0),
        (2, 4611686018427387904::BIGINT, 4::BIGINT, NULL), (3, 3::BIGINT, 2::BIGINT, 2.0)) t(id, a, b, v)
        DECIDE x(REAL) SUCH THAT x <= 9 MAXIMIZE sum(t.a * t.b * x + t.v * x)"""
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_a_member_test_that_raises_raises_flats_error(decidb_cli):
    """The test `ln(t.w) * t.v IS NOT NULL` raises on row 2, which its reducer reads: flat's message."""
    sql = """SELECT id, x FROM (VALUES (1, 2.0, 1.0), (2, 0.0, NULL), (3, 3.0, 2.0)) t(id, w, v)
        DECIDE x(REAL) SUCH THAT x <= 9 MAXIMIZE sum(ln(t.w) * t.v * x)"""
    flat = run(decidb_cli, sql, "flat")
    assert "cannot take logarithm of zero" in flat.stderr, flat.stderr
    assert run(decidb_cli, sql).stderr == flat.stderr


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
def test_a_member_test_raises_in_flats_order(decidb_cli, tmp_path, case):
    sql = f"""SELECT id, x FROM (VALUES (1, 2.0, -1.0, 1.0), (2, 0.0, 2.0, 1.0)) t(id, a, w, v)
        DECIDE x(REAL) SUCH THAT x <= 1 MAXIMIZE {OBJECTIVE_RAISED[case]}"""
    flat = run(decidb_cli, sql, "flat")
    assert "cannot take logarithm of zero" in flat.stderr, flat.stderr
    lines, tree = coverage(decidb_cli, sql, tmp_path)
    assert lines == ["covered"], tree.stderr
    assert tree.stderr == flat.stderr


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
    lines, result = coverage(decidb_cli, sql, tmp_path)
    assert lines == ["covered"], result.stderr
    flat, tree = models(decidb_cli, sql, tmp_path)
    assert tree == flat


def test_an_objective_produced_twice_raises_flats_error(decidb_cli):
    """t.c, outside every reducer, takes two values over the kept rows: the objective has two instances (D72)."""
    sql = f"""SELECT id, x FROM {ROWS} DECIDE x(REAL), per (): y(REAL)
        SUCH THAT x <= 9 AND y <= 4 MAXIMIZE sum(x) + t.c * y"""
    flat = run(decidb_cli, sql, "flat")
    assert "the objective would be produced more than once" in flat.stderr, flat.stderr
    assert run(decidb_cli, sql).stderr == flat.stderr
