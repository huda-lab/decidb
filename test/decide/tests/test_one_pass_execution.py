"""The solver consumes the bottom-up E/G/O values, including an altered child value."""

import json
import os
import re
import subprocess

import pytest

from ._anr_cases import F1_MAX, F1_SUM, F2_PREVIOUS
from ._scope_cases import answer, dump_counts


@pytest.mark.parametrize(
    "case,counts",
    [
        (F1_MAX, {"num_vars": 8, "num_rows": 7}),
        (F1_SUM, {"num_vars": 6, "num_rows": 3}),
        (F2_PREVIOUS, {"num_vars": 6, "num_rows": 4}),
    ],
    ids=lambda value: value.name if hasattr(value, "name") else None,
)
def test_tree_values_reach_the_decks_model(decidb_cli, tmp_path, case, counts):
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree", "DECIDB_NATIVE_CONSTRUCTS": "off"})
    sql = "SET decide_direct_solve='off'; " + case.sql
    assert case.model().judge(answer(cli, sql), case.clauses, case.objective) == pytest.approx(case.optimum)
    assert dump_counts(cli.dump_model(sql, tmp_path / "tree.dump")) == counts


def test_changing_a_data_child_changes_the_answer(decidb_cli):
    """L7: changing the computed SUM to 8 must flow through the parent and into the solver's bound."""
    sql = """SET decide_direct_solve='off';
        SELECT id,x FROM (VALUES (1,2),(2,3)) t(id,d)
        DECIDE per (): x(REAL) SUCH THAT x <= sum(t.d) MAXIMIZE x ORDER BY id"""
    env = {**os.environ, **(decidb_cli.env or {}), "DECIDB_EVALUATOR": "tree", "DECIDB_DUMP_TREE": "1"}
    original = subprocess.run(
        [decidb_cli.exe, "-json", "-c", sql], env=env, capture_output=True, text=True, timeout=120
    )
    assert original.returncode == 0, original.stderr
    # Find the data G through the actual graph, instead of relying on a fixed planner node number.
    node = re.search(r"(?m)^\s*(\d+) G .*", original.stderr)
    assert node, original.stderr
    changed = subprocess.run(
        [decidb_cli.exe, "-json", "-c", sql],
        env={**env, "DECIDB_TREE_INJECT": node[1] + ":data:8"},
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert changed.returncode == 0, changed.stderr
    assert [row["x"] for row in json.loads(original.stdout)] == [5, 5]
    assert [row["x"] for row in json.loads(changed.stdout)] == [8, 8]


#: Each query reaches one arm of the descent's nesting rule (decide_descent.cpp, Visit: an E's classes lie within the
#: group it feeds, a G's or O's groups within none), with its answer worked by hand. The default suite runs flat.
NESTING_CASES = {
    # The G arm. `max(t.v) by (t.r)` is p: 30, q: 6, so the row bounds are 30, 6, 30, 6, and S3's hidden min over
    # each class of t.g bounds a and b by 6. Grouped within that min's slot, a read only its own rows (10, 4) and
    # got 4. The id weights leave one optimum: x2 = 6 and x4 = 6.
    "a-generated-bound-opens-no-level": (
        """SELECT id, x FROM (VALUES (1, 'a', 'p', 10), (2, 'a', 'q', 4), (3, 'b', 'p', 30), (4, 'b', 'q', 6))
        t(id, g, r, v) DECIDE x(INT) SUCH THAT x <= 100 AND per t.g: sum(x) by (t.g) <= max(t.v) by (t.r)
        MAXIMIZE SUM(t.id * x) ORDER BY id""",
        [{"id": 1, "x": 0}, {"id": 2, "x": 6}, {"id": 3, "x": 0}, {"id": 4, "x": 6}],
    ),
    # The E arm. Group a counts its one class k1 once (5); b counts k1 and k2 (5 + 7 = 12). With b's k1 class in a's,
    # b would read 7.
    "a-class-lies-in-the-group-it-feeds": (
        """SELECT id, x FROM (VALUES (1, 'a', 'k1', 5), (2, 'a', 'k1', 5), (3, 'b', 'k1', 5), (4, 'b', 'k2', 7))
        t(id, g, k, v) DECIDE x(INT) SUCH THAT x <= sum(per t.k: t.v) by (t.g) MAXIMIZE SUM(x) ORDER BY id""",
        [{"id": 1, "x": 5}, {"id": 2, "x": 5}, {"id": 3, "x": 12}, {"id": 4, "x": 12}],
    ),
}


@pytest.mark.parametrize("case", list(NESTING_CASES))
def test_the_tree_nesting_rule(decidb_cli, case):
    sql, expected = NESTING_CASES[case]
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert answer(cli, "SET decide_direct_solve='off'; " + sql) == expected


#: Each query reaches a branch of the tree formulator's column box (decide_tree_formulation.cpp) that no other case
#: reaches, with its answer worked by hand. The default suite runs flat, which never reaches that file.
BOX_CASES = {
    # FoldBound refuses `<>`, which is no interval, and the `<>` is lowered after the auxiliaries, as its Big-M reads
    # the product's range: b = 1 and x = 3.
    "a-not-equal-reads-the-settled-box": (
        """SELECT id, x, b FROM (VALUES (1)) t(id) DECIDE x(INT), per (): b(BOOL)
        SUCH THAT x <= 4 AND b * SUM(x) <> 4 MAXIMIZE SUM(x) + 4 * b""",
        [{"id": 1, "x": 3, "b": True}],
    ),
    # AuxiliaryRange intersects: the folded b * x <= t.k survives the product's derived range [0, 5].
    "a-derived-range-keeps-a-folded-bound": (
        """SELECT id, x, b FROM (VALUES (1, 2), (2, 1)) t(id, k) DECIDE x(INT), b(BOOL)
        SUCH THAT x <= 5 AND b * x <= t.k MAXIMIZE SUM(x) + 4.5 * SUM(b) ORDER BY id""",
        [{"id": 1, "x": 2, "b": True}, {"id": 2, "x": 1, "b": True}],
    ),
    # A floor the user wrote moves into the instance before the auxiliary's floor lowers: |x - q| >= 30 holds.
    "a-lowered-floor-keeps-the-written-floor": (
        """SELECT id, x FROM (VALUES (1, 10.0), (2, 20.0)) t(id, q) DECIDE x(REAL)
        SUCH THAT x <= 100 AND ABS(x - q) >= 30 MINIMIZE SUM(x) ORDER BY id""",
        [{"id": 1, "x": 40.0}, {"id": 2, "x": 50.0}],
    ),
}


@pytest.mark.parametrize("case", list(BOX_CASES))
def test_the_tree_column_box(decidb_cli, case):
    sql, expected = BOX_CASES[case]
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert answer(cli, "SET decide_direct_solve='off'; " + sql) == expected


#: Each query reaches a rule of the tree builder's sigma (decide_tree.cpp, Lower's `carried`) that no other case
#: reaches, with its answer worked by hand: the columns a generated NULL test reads are carried, never fields (D83).
SIGNATURE_CASES = {
    # A bound's nested reducer value is its key alone (deck p. 81), whatever its NULL test reads: k = u is one class of
    # the group upper(g) = 'A', so the bound is 4. Reading g as a field made 'a' and 'A' two instances of it: x <= 8.
    # This is test_anr_nested's function-of-one-column case, run by the tree.
    "a-nested-reducer-value-is-its-key": (
        """SELECT id, x FROM (VALUES (1, 'u', 'a', 4), (2, 'u', 'A', 4)) t(id, k, g, v) DECIDE x(INT)
        SUCH THAT x <= 100 AND x <= sum(per t.k: max(t.v) by (t.k)) by (upper(t.g))
        MAXIMIZE sum(t.id * x) ORDER BY id""",
        [{"id": 1, "x": 4}, {"id": 2, "x": 4}],
    ),
    # A member test reads a NOCASE key's raw spelling: y has the classes {a, A} and {b}, so y_a + y_b <= 12, and
    # 9 y_a + 3 y_b peaks at y_a = 10, y_b = 2. Reading g as a field counted y_a once per spelling: 6, 6, 0.
    "a-member-test-is-no-field": (
        """SELECT id, y FROM (SELECT id, g COLLATE NOCASE AS g, p FROM (VALUES (1, 'a', 5.0), (2, 'A', 4.0),
        (3, 'b', 3.0)) v(id, g, p)) t DECIDE per t.g: y(INT) SUCH THAT y <= 10 AND sum(per t.g: y) <= 12
        MAXIMIZE sum(t.p * y) ORDER BY id""",
        [{"id": 1, "y": 10}, {"id": 2, "y": 10}, {"id": 3, "y": 2}],
    ),
}


@pytest.mark.parametrize("case", list(SIGNATURE_CASES))
def test_the_tree_signature_carries_null_tests(decidb_cli, case):
    sql, expected = SIGNATURE_CASES[case]
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert answer(cli, "SET decide_direct_solve='off'; " + sql) == expected


# A class root's sigma (the objective, `per K`, `per ()`) is the binder's signature of its written clause; with none
# it has no field (decide_tree.cpp, UseSignature's root branch). The lowered body reads more than was written. The
# objective reaches the branch from Build and the `per` clause from Root; a root per row keeps its reads.
def test_a_lowered_auxiliary_is_no_field_of_the_objective(decidb_cli):
    """The ABS argument reads a reducer, so its auxiliary is per row. sum(x) is in [0, 8], so |sum(x) - 9| is
    greatest only at sum(x) = 0: every x is 0. With the auxiliary as a field the objective had three instances and
    was refused."""
    sql = """SELECT id, x FROM (VALUES (1, 3), (2, 3), (3, 2)) t(id, cap)
        DECIDE x(INT) SUCH THAT x <= cap MAXIMIZE 0 * sum(x) + abs(sum(x) - 9) ORDER BY id"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert answer(cli, "SET decide_direct_solve='off'; " + sql) == [{"id": i, "x": 0} for i in (1, 2, 3)]


def test_a_member_test_is_no_field_of_a_per_clause(decidb_cli):
    """`per g` holds `by (g)`, so the clause is one instance per class. The easy MIN's member test reads p, which
    is no field of what was written: as one, class A had an instance for row 2, which is no member, and bound it.
    The members are rows 1 and 3, so x1 + 1 >= 2 and x3 + 1 >= 2, and MINIMIZE takes x = 1, 0, 1."""
    sql = """SELECT id, x FROM (VALUES (1, 'A', true), (2, 'A', false), (3, 'B', true)) t(id, g, p)
        DECIDE x(INT) SUCH THAT x <= 5 AND per g: min(when p: x + 1) by (g) >= 2 MINIMIZE SUM(x) ORDER BY id"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert answer(cli, "SET decide_direct_solve='off'; " + sql) == [
        {"id": 1, "x": 1},
        {"id": 2, "x": 0},
        {"id": 3, "x": 1},
    ]


def test_a_root_records_the_classes_of_a_helper_it_reads(decidb_cli):
    """The product b * SUM(x) by (t.g) is a helper keyed on its group, with no defining node: its classes come from
    the nodes that read it (decide_descent.cpp, Visit), not from a field of the reader's sigma, which here is the
    binder's (t.cap). Class a holds b * (x1 + x2) <= 4 and <= 5. With b = 1 that leaves x1 + x2 <= 4 and x3 <= 2,
    and SUM(id * x) takes x2 = 3, x1 = 1, x3 = 2: 13 + 9 = 22, above b = 0's 3 + 6 + 9 = 18."""
    sql = """SELECT id, x, b FROM (VALUES (1, 'a', 4), (2, 'a', 5), (3, 'b', 2)) t(id, g, cap)
        DECIDE x(INT), per (): b(BOOL) SUCH THAT x <= 3 AND per t.g: b * SUM(x) by (t.g) <= t.cap
        MAXIMIZE SUM(id * x) + 9 * b ORDER BY id"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert answer(cli, "SET decide_direct_solve='off'; " + sql) == [
        {"id": 1, "x": 1, "b": True},
        {"id": 2, "x": 3, "b": True},
        {"id": 3, "x": 2, "b": True},
    ]


def test_a_root_per_row_keeps_the_window_of_the_helper_it_defines(decidb_cli):
    """The ABS of a range over decisions is a helper per window, defined by rows per row whose reads (the window)
    key its instances (decide_descent.cpp, DecisionClasses). The windows are x1 + x2, x1 + x2 + x3, x2 + x3 + x4 and
    x3 + x4, with x1 = 2, x2 in [2, 3], x3 in [0, 3] and x4 in [1, 2]. At x2 = 2 the sum of |w - 4| is
    x3 + |x3 + x4 - 2| + |x3 + x4 - 4| >= 2, met only at x3 = 0 and x4 = 2; at x2 = 3 the first two terms are 2 + x3
    and the last two at least 3. So x = 2, 2, 0, 2. With the definitions' sigma cleared, one auxiliary stood for every
    window and was charged 4 * max |w - 4|, which takes x3 = 1."""
    sql = """SELECT id, x FROM (VALUES (1, 2, 2), (2, 2, 3), (3, 0, 3), (4, 1, 2)) t(id, lo, hi)
        DECIDE x(INT) SUCH THAT x >= lo AND x <= hi
        MINIMIZE SUM(abs(sum(from previous to next: x) over (t.id) - 4)) ORDER BY id"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert answer(cli, "SET decide_direct_solve='off'; " + sql) == [
        {"id": 1, "x": 2},
        {"id": 2, "x": 2},
        {"id": 3, "x": 0},
        {"id": 4, "x": 2},
    ]
