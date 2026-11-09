"""The solver consumes the bottom-up E/G/O values, including an altered child value."""

import json
import os
import re
import subprocess

import pytest

from . import _diagnose_relation
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


def test_a_product_has_no_value_where_a_factor_has_none(decidb_cli):
    """D88 through a product (BilinearLink): group b keeps no row for `sum(when t.c: x)`, so the factor has no value on
    row 2, nor has `b * sum(...)`, and row 2's comparison is skipped: x2 keeps its cap of 9 and b2 earns 1. Row 1 reads
    x1 + b1 * x1 <= 5, where b1 = 0, x1 = 5 earns 5 and b1 = 1, x1 = 2 earns 3. Reached: the factor check of
    DecisionRead, which answered x2 = 5 without it, and the dependency of a product's reader on its factor's G in the
    tree builder's Sources, without which that G is read before it is computed."""
    sql = """SET decide_direct_solve='off';
        SELECT id, x, b FROM (VALUES (1, 'a', true), (2, 'b', false)) t(id, g, c) DECIDE x(INT), b(BOOL)
        SUCH THAT x <= 9 AND x + b * sum(when t.c: x) by (t.g) <= 5 MAXIMIZE sum(x) + sum(b) ORDER BY id"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert answer(cli, sql) == [{"id": 1, "x": 5, "b": False}, {"id": 2, "x": 9, "b": True}]


#: A clause in the grouped shape is one model row per group of its `by` key (language/guide.md §6.4), so a root no `per`
#: makes a class root is a class of that key (decide_tree.cpp, GroupClass), not one instance per row. Each case reaches
#: an arm no other case reaches. Groups a and b keep no row for the reducer's `when`, so each of their 2 instances has
#: no value (D90) and every x keeps its cap of 10. One instance per row counted 5.
GROUP_CLASS_CASES = {
    # GroupClass from Root, through the aggregate shape.
    "an-aggregate-by-a-key": "sum(when t.c > 100: x) by (t.g) <= 5",
    # Through an easy extremum, which is not the aggregate shape (ReadDecideClauseFacts).
    "an-easy-extremum-by-a-key": "max(when t.c > 100: x) by (t.g) <= 5",
}


@pytest.mark.parametrize("case", list(GROUP_CLASS_CASES))
def test_a_grouped_root_is_one_instance_per_group(decidb_cli, case):
    sql = f"""SET decide_direct_solve='off';
        SELECT id, x FROM (VALUES (1, 'a', 3), (2, 'a', 4), (3, 'a', 9), (4, 'b', 2), (5, 'b', 6)) t(id, g, c)
        DECIDE x(INT) SUCH THAT x <= 10 AND {GROUP_CLASS_CASES[case]} MAXIMIZE SUM(x) ORDER BY id"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    rows = answer(cli, sql, warnings=["each of its 2 instances has no value"])
    assert rows == [{"id": i, "x": 10} for i in range(1, 6)]


def test_an_ungrouped_root_is_one_model_row(decidb_cli, tmp_path):
    """`SUM(x) <> 0` over three BOOL rows cannot reach below 0, so it is the one row `SUM(x) >= 1`, which x1 meets at
    the least cost. A root per row emitted it once per row."""
    sql = """SET decide_direct_solve='off';
        SELECT id, x FROM (VALUES (1), (2), (3)) t(id) DECIDE x(BOOL) SUCH THAT SUM(x) <> 0
        MINIMIZE SUM(t.id * x) ORDER BY id"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree", "DECIDB_NATIVE_CONSTRUCTS": "off"})
    assert answer(cli, sql) == [{"id": 1, "x": True}, {"id": 2, "x": False}, {"id": 3, "x": False}]
    assert dump_counts(cli.dump_model(sql, tmp_path / "tree.dump"))["num_rows"] == 1


#: An expanded diagnosis names what a grouped instance stands for (decide_tree_formulation.cpp, Provenance). Group a's
#: two BOOL rows reach 2, so `>= 3` loosens to `>= 2` there; group b's three rows meet it. Each case reaches an arm of
#: the class set no other case reaches.
EXPANDED_CASES = {
    # With no `per`, every kept row of its group (SolverInput::ClassSet's KEPT_ROWS), not only the row holding its
    # entry: rows 0 and 1.
    "no-per": (
        "sum(x) by (t.g) >= 3",
        [("expanded_row", "NULL", "0", "sum(x) by (t.g) >= 2"), ("expanded_row", "NULL", "1", "sum(x) by (t.g) >= 2")],
    ),
    # A written `per` names its classes (KEY_VALUES), though its instance also stands for a group: the class a.
    "a-per-key": (
        "per t.g: sum(x) by (t.g) >= 3",
        [("expanded_group", "a", "NULL", "per t.g: sum(x) by (t.g) >= 2")],
    ),
}


@pytest.mark.parametrize("case", list(EXPANDED_CASES))
def test_an_expanded_diagnosis_names_what_a_grouped_instance_stands_for(decidb_cli, case):
    clause, expected = EXPANDED_CASES[case]
    sql = f"""SELECT id, x FROM (VALUES (1, 'a'), (2, 'a'), (3, 'b'), (4, 'b'), (5, 'b')) t(id, g)
        DECIDE x(BOOL) SUCH THAT {clause} MAXIMIZE SUM(x)"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    result = _diagnose_relation.run(cli, sql, setup="SET decide_direct_solve='off';\n", scope="expanded")
    edits = [row for row in _diagnose_relation.rows(result) if row["clause"] == clause]
    found = [(row["edit_source"], row["group"], row["row"], row["suggested_change"]) for row in edits]
    assert found == expected, result.stdout


def test_the_anchors_of_a_group_share_its_class_set(decidb_cli):
    """With no `per`, each anchor of a frame bound is an instance of the group it bounds (D85), and every one stands for
    the group's kept rows: one class set per group (decide_tree_formulation.cpp, Provenance), which merging the anchors
    into the group's one row leaves alone (MergeClasses). A set per anchor, or a merge of the set into itself, took
    over 30 s here, against under a second. The reducer reads row 0 alone, bound by the least `previous` value, 5, so
    every x is 1."""
    sql = """SET decide_direct_solve='off';
        SELECT count(*) AS n, sum(x) AS total FROM (SELECT * FROM (SELECT i AS s, (i % 13) + 5.0 AS c
        FROM range(6000) r(i)) t DECIDE x(REAL) SUCH THAT x <= 1 AND sum(when t.s = 0: x) <= at(previous: t.c)
        over (t.s) MAXIMIZE sum(x))"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    rows, _ = cli.execute(sql, timeout=20)
    assert rows == [(6000, 6000.0)]


#: A floor below 0 that a clause lifts lands on each decision the clause bounds (decide_tree_formulation.cpp,
#: LiftedFloors through Bounded): an easy extremum's members, whichever row holds its group's entry. `p` keeps rows 1
#: and 4, whose x may go down to -5, and rows 2 and 3 keep the default floor of 0, so MINIMIZE takes -5, 0, 0, -5. Each
#: case reaches one side the comparison pushes.
LIFTED_FLOOR_CASES = {
    "a-minimum-bounded-below": "min(when p: x) >= -5",
    "a-maximum-bounded-above": "max(when p: -x) <= 5",
}


@pytest.mark.parametrize("case", list(LIFTED_FLOOR_CASES))
def test_a_lifted_floor_lands_on_each_member(decidb_cli, case):
    sql = f"""SET decide_direct_solve='off';
        SELECT id, x FROM (VALUES (1, true), (2, false), (3, false), (4, true)) t(id, p)
        DECIDE x(INT) SUCH THAT x <= 10 AND {LIFTED_FLOOR_CASES[case]} MINIMIZE SUM(x) ORDER BY id"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert answer(cli, sql) == [{"id": 1, "x": -5}, {"id": 2, "x": 0}, {"id": 3, "x": 0}, {"id": 4, "x": -5}]


def test_the_tree_walks_a_shared_value_once(decidb_cli):
    """Every row of a group reads the group's sum: x_i + S_g <= 10. RequireSupported (decide_tree_evaluation.cpp) runs
    once, in Finalize, and walks a value again only under an earlier rank, so the counter is linear in the rows.
    Without that memo each of the 600 rows walked its group's 200 members (about 124,000 walks); the formulator's
    second call reported a second counter. The cap of 1 is a frame's, which the model emitter has no arm for, so the
    query takes the interim evaluation. The answer: with m the largest x of a group, S_g <= 10 - m and S_g <= 200 m,
    so S_g = 10 - 10/201 and the total is 3 * (10 - 10/201) = 29.850746."""
    rows = 600
    sql = f"""SET threads=1; SET decide_direct_solve='off';
        SELECT round(sum(x), 6) AS total FROM (SELECT i, x FROM (SELECT i, i % 3 AS g, 1 AS one FROM range({rows})
        r(i)) t DECIDE x(REAL) SUCH THAT x <= at(previous else 1: t.one) over (t.i) AND x + sum(x) by (t.g) <= 10
        MAXIMIZE sum(x))"""
    cli = decidb_cli.without_verify_serializer().with_env({"DECIDB_EVALUATOR": "tree", "DECIDB_PROFILE": "1"})
    profiled = cli.execute_raw(sql)
    events = [
        json.loads(line[len("DECIDB_PROFILE: ") :])
        for line in profiled.stderr.splitlines()
        if line.startswith("DECIDB_PROFILE: ")
    ]
    walks = [event["value"] for event in events if event.get("name") == "execution.required_values"]
    assert len(walks) == 1 and walks[0] < 20 * rows, walks
    assert "29.850746" in profiled.stdout, profiled.stdout


def test_a_shared_error_ranks_at_its_earliest_reader(decidb_cli):
    """Position t.p = 1 holds two rows, so the frame raises where row 3 reads it, an error that takes the rank of its
    reader. The ABS auxiliary's definition reads it (KEYED_MAPPING), and so does its clause (CONSTRAINTS) through the
    auxiliary, first. RequireSupported walks it again under the definition's earlier rank, so it raises before the
    sqrt of clause 1, as flat raises its frame peers before its constraints. Kept at its first reader's rank, clause
    2's, it raised the sqrt of row 1 instead, the error this query raises when its positions are distinct."""
    sql = """SET threads=1; SET decide_direct_solve='off';
        SELECT id, x FROM (VALUES (1, 1, -1.0, 5.0), (2, 1, 4.0, 6.0), (3, 2, 9.0, 7.0)) t(id, p, r, v) DECIDE x(REAL)
        SUCH THAT x <= 10 AND sqrt(t.r) * x <= 5 AND abs(x - at(previous else 0: t.v) over (t.p)) <= 30
        MAXIMIZE sum(x)"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert cli.execute_raw(sql).stderr.strip() == (
        "Invalid Input Error: DECIDE: at(previous else 0: t.v) over (t.p) finds 2 rows at t.p = 1; "
        "a position must hold one row."
    )


def test_an_interrupt_stops_the_bottom_up_evaluation(decidb_cli):
    """Twelve frames over data, each read at 5,000 anchors over windows of about 4,000 positions, hold the tree's
    bottom-up evaluation for about 11 s, about 1 s a frame. Evaluate polls the query's interrupt before each node, as
    the descent does before each visit, so a Ctrl-C 1.5 s in stops the query with DuckDB's interrupt. Without that
    poll the evaluation ran to its end, and the solver's poll stopped the query with "DECIDE stopped at your
    request"."""
    frames = " AND ".join(f"x <= sum(from {k} previous to {k} next: t.v) over (t.i)" for k in range(2000, 2012))
    sql = f"""SET threads=1; SET decide_direct_solve='off'; SET decide_frame_limit=1000000000;
        SELECT count(*) FROM (SELECT i, x FROM (SELECT i, (i * 7919) % 100 AS v FROM range(5000) r(i)) t
        DECIDE x(REAL) SUCH THAT {frames} MAXIMIZE sum(x))"""
    result, _ = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"}).execute_interrupted(sql, after=1.5, timeout=60)
    assert "Interrupted!" in result.stderr, result.stderr[:800]


#: A clause in the grouped shape is one instance per group of its `by` key, whatever else tells its entries apart
#: (language/guide.md §6.4, §7.4; decide_tree_evaluation.cpp, Scope), so D90 counts groups, as flat does: a and b, whose
#: rows the reducer's `when` leaves empty. Each case reaches an arm no other case reaches. Counting entries said 4, and
#: 5 with row 6.
GROUPED_COUNT_ROWS = ((1, "a", 3, 1), (2, "a", 4, 2), (3, "a", 9, 3), (4, "b", 2, 1), (5, "b", 6, 1))
GROUPED_COUNT_CASES = {
    # A `per` class across groups: the root's slot is the class (1, 2, 3), its instance the group a child keyed by
    # `by` holds.
    "a-class-across-groups": ("per t.k: sum(when t.c > 100: x) by (t.g) <= 5", GROUPED_COUNT_ROWS),
    # A bound that varies in a group: its value, a field of sigma, tells entries apart, not instances.
    "a-bound-that-varies-in-a-group": ("sum(when t.c > 100: x) by (t.g) <> t.k", GROUPED_COUNT_ROWS),
    # A row whose `by` key is NULL is in no group (D5), so row 6's entry in class 1 is no instance.
    "a-row-in-no-group": (
        "per t.k: sum(when t.c > 100: x) by (t.g) <= 5",
        GROUPED_COUNT_ROWS + ((6, None, 7, 1),),
    ),
}


@pytest.mark.parametrize("case", list(GROUPED_COUNT_CASES))
def test_a_grouped_clause_counts_its_groups(decidb_cli, case):
    clause, rows = GROUPED_COUNT_CASES[case]
    values = ", ".join(f"({i}, {'NULL' if g is None else repr(g)}, {c}, {k})" for i, g, c, k in rows)
    sql = f"""SET decide_direct_solve='off';
        SELECT id, x FROM (VALUES {values}) t(id, g, c, k)
        DECIDE x(INT) SUCH THAT x <= 10 AND {clause} MAXIMIZE SUM(x) ORDER BY id"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    found = answer(cli, sql, warnings=["each of its 2 instances has no value"])
    assert found == [{"id": row[0], "x": 10} for row in rows]


def _fallback_split(second):
    return f"""SELECT id, x FROM (VALUES (1, 'A', 1, 4, 5), (2, 'A', 1, 4, {second}), (3, 'A', 2, 6, 1),
            (4, 'B', 1, 7, NULL), (5, 'C', 1, 7, NULL)) t(id, product, period, v, w)
        DECIDE per t.product: x(INT) SUCH THAT x <= 9
            AND per t.product: x <= at(previous else t.w: t.v) over (t.period within t.product)
        MAXIMIZE sum(x) ORDER BY id"""


#: D90 counts each instance once, with a value or not (language/guide.md §7.4; decide_tree_evaluation.cpp, Scope), as
#: flat counts. Each case reaches an arm no other case reaches.
INSTANCE_COUNT_CASES = {
    # Under `per t.product`, a variant per fallback its anchors reach (D82). A's first period falls back to its rows'
    # `t.w`, 5 and 2, two instances with a value; its second period reads 4; B and C fall back to a NULL, none.
    # Counting the variants the descent held, A's first period once, said 2 of 4.
    "a-fallback-of-two-values": (_fallback_split("2"), "2of5"),
    # 5 and NULL: one instance with a value and one with none. Counting only the variants the descent held with no
    # value at all said 2 of 4.
    "a-fallback-of-a-value-and-none": (_fallback_split("NULL"), "3of5"),
    # The grouped shape counts a group once however many variants it holds: a has three classes of t.k reading row
    # 3's x, and b no row the `when` keeps. One instance per variant of a said 1 of 4.
    "a-group-of-several-variants": (
        """SELECT id, x FROM (VALUES (1, 'a', 3, 1), (2, 'a', 4, 2), (3, 'a', 9, 3), (4, 'b', 2, 1), (5, 'b', 6, 1))
            t(id, g, c, k)
        DECIDE x(INT) SUCH THAT x <= 10 AND per t.k: sum(when t.c > 6: x) by (t.g) <= 5 MAXIMIZE SUM(x) ORDER BY id""",
        "1of2",
    ),
}


@pytest.mark.parametrize("case", list(INSTANCE_COUNT_CASES))
def test_explain_counts_each_instance_once(decidb_cli, case):
    sql, skipped = INSTANCE_COUNT_CASES[case]
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    result = cli.execute_raw("SET decide_direct_solve='off'; EXPLAIN ANALYZE " + sql)
    assert result.returncode == 0, result.stderr
    text = "".join(line.strip().strip("│").strip() for line in result.stdout.splitlines()).replace(" ", "")
    assert f"↳{skipped}instanceshavenovalueandareskipped" in text, text


def test_a_grouped_clause_folds_only_its_own_groups_entries(decidb_cli, tmp_path):
    """Formulation folds a grouped instance's entries into its row under the tightest bound
    (decide_tree_formulation.cpp, Constraints), one row per group as flat makes. Groups a and b of region R read the
    same terms, 2 y_R + 2 z_R, under 5 and 3: two rows. Folding by terms alone merged them into one."""
    sql = """SET decide_direct_solve='off';
        SELECT id, y, z FROM (VALUES (1, 'R', 'a', 5), (2, 'R', 'a', 7), (3, 'R', 'b', 3), (4, 'R', 'b', 8))
            t(id, r, g, c)
        DECIDE per t.r: y(INT), per t.r: z(INT) SUCH THAT y <= 9 AND z <= 9 AND sum(y + z) by (t.g) <= t.c
        MAXIMIZE sum(2 * y + z) ORDER BY id"""
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
    assert answer(cli, sql) == [{"id": i, "y": 1, "z": 0} for i in range(1, 5)]
    assert dump_counts(cli.dump_model(sql, tmp_path / "tree.dump"))["num_rows"] == 2


#: `<>` keeps each value of a group's bound its own exclusion (language/guide.md §5): formulation folds an entry into
#: an earlier exclusion of its instance only where the two values agree (decide_tree_formulation.cpp, Constraints).
#: Each case reaches one side of that test.
GROUPED_NOT_EQUAL_CASES = {
    # Depots D1 and D2 of region E both exclude 5: one exclusion, as flat makes, where an exclusion per entry made
    # three indicators and six rows.
    "one-value": (
        """SELECT id, x FROM (VALUES (1, 'D1', 'E', 10), (2, 'D1', 'E', 20), (3, 'D2', 'E', 30), (4, 'D3', 'W', 15))
            t(id, depot, region, cap)
        DECIDE x(INT) SUCH THAT x <= t.cap AND per t.depot: sum(x) by (t.region) <> 5 MAXIMIZE SUM(x)""",
        {"num_vars": 6, "num_rows": 4},
    ),
    # Group a excludes 3 and 2, so its three x of at most 1 sum to 1, and b's one x is 0. Folding 2 into 3 let a reach
    # 2.
    "two-values": (
        """SELECT sum(x) AS total FROM (SELECT * FROM (VALUES (1, 'a', 3), (2, 'a', 2), (3, 'a', 3), (4, 'b', 1))
            t(id, g, k)
        DECIDE x(INT) SUCH THAT x <= 1 AND sum(x) by (t.g) <> t.k MAXIMIZE SUM(x))""",
        None,
    ),
}


@pytest.mark.parametrize("case", list(GROUPED_NOT_EQUAL_CASES))
def test_a_grouped_not_equal_excludes_each_value_once(decidb_cli, tmp_path, case):
    sql, counts = GROUPED_NOT_EQUAL_CASES[case]
    sql = "SET decide_direct_solve='off'; " + sql
    cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree", "DECIDB_NATIVE_CONSTRUCTS": "off"})
    if counts:
        assert dump_counts(cli.dump_model(sql, tmp_path / "tree.dump")) == counts
    else:
        assert answer(cli, sql) == [{"total": 1}]
