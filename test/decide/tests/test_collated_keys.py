"""A key groups rows as GROUP BY does: under its column's collation (B1).

`COLLATE NOCASE` puts 'a' and 'A' in one group, for a `by` group, a `per K:` class, a bound-side reducer and a keyed
decision; a message quotes the group's least spelling ('A'). The oracle states NOCASE on its own: it groups on the
lowercased key `gk`.
"""

import pytest

from . import _diagnose_relation
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import aligned, answer, scope_solver, values  # noqa: F401
from ._scope_oracle import CONSTANT, Decision, Reduce, Scoped, ScopedModel, ScopedObjective
from solver.types import ObjSense, VarType

COLUMNS = ("id", "g", "cap", "p")
ROWS = ((1, "a", 1, 5.0), (2, "A", 2, 4.0), (3, "b", 2, 3.0), (4, "B", 3, 2.0), (5, "b", 2, 1.0))
X = Decision("x", var_type=VarType.BINARY)
MAXIMIZE_PX = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": r["p"]}),))


def _source(rows=ROWS, collation="COLLATE NOCASE"):
    return f"FROM (SELECT id, g {collation} AS g, cap, p FROM {values(rows, 'v', COLUMNS)}) t"


def _oracle_rows(rows=ROWS):
    return [dict(zip(COLUMNS, row), gk=row[1].lower()) for row in rows]


def _judged(cli, solver, sql, decisions, clauses, objective):
    model = ScopedModel(_oracle_rows(), decisions)
    result = aligned(answer(cli, sql), model, "id")
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective)
    )
    return result


@pytest.mark.correctness
def test_a_by_group_groups_its_key_under_the_collation(decidb_cli, scope_solver):
    """`sum(x) by (g) <= 1`: 'a' and 'A' are one group, so one of rows 1 and 2 is picked, and one of rows 3-5."""
    sql = f"SELECT id, x {_source()} DECIDE x(BOOL) SUCH THAT sum(x) by (t.g) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk",)),), "<=", (lambda r: {CONSTANT: 1},))]
    result = _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)
    assert sum(row["x"] for row in result) == 2


@pytest.mark.correctness
def test_a_collated_per_key_determines_its_own_by_group(decidb_cli, scope_solver):
    """`per g: sum(x) by (g) <= 1`: the class and the `by` group both group under NOCASE, so the key holds its own
    `by` element by construction (KeyHoldsField); 'a' and 'A' are still one class."""
    sql = f"""
        SELECT id, x {_source()}
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) by (t.g) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk",)),), "<=", (lambda r: {CONSTANT: 1},), per=("gk",))]
    result = _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)
    assert sum(row["x"] for row in result) == 2


@pytest.mark.correctness
def test_a_by_key_under_another_collation_is_another_key(decidb_cli, scope_solver):
    """`per g: sum(x) by (g COLLATE noaccent) <= 1` over a NOCASE g: the two collations group differently, so they are
    two keys (DecideGroupsAlike: the collations differ) and the class {a, A} holds two `by` groups, one variant each
    (D70): 'a', 'A', 'b' (rows 3 and 5) and 'B' each take one row."""
    sql = f"""
        SELECT id, x {_source()}
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) by (t.g COLLATE noaccent) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("g",)),), "<=", (lambda r: {CONSTANT: 1},), per=("gk",))]
    result = _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)
    assert [row["x"] for row in sorted(result, key=lambda row: row["id"])] == [1, 1, 1, 1, 0]


@pytest.mark.correctness
def test_a_by_key_written_under_the_columns_collation_is_the_same_key(decidb_cli, scope_solver):
    """`per g: sum(x) by (g COLLATE nocase) <= 1` over a NOCASE g: one collation, so one key (DecideGroupsAlike: the
    collations are equal), which holds the `by` key."""
    sql = f"""
        SELECT id, x {_source()}
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) by (t.g COLLATE nocase) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk",)),), "<=", (lambda r: {CONSTANT: 1},), per=("gk",))]
    _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_the_default_collation_and_the_same_written_collation_are_one_key(decidb_cli, scope_solver):
    """Under `default_collation='nocase'`, a plain g and `g COLLATE NOCASE` group under one collation function, though
    their types name different collations, so they are one key (Grouping: equal), which holds the `by` key."""
    sql = f"""
        SET default_collation='nocase';
        SELECT id, x {_source(collation="")}
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) by (t.g COLLATE NOCASE) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk",)),), "<=", (lambda r: {CONSTANT: 1},), per=("gk",))]
    _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_a_bound_side_reducer_reads_its_group_under_the_collation(decidb_cli, scope_solver):
    """`sum(x) by (g) <= min(cap) by (g)`: the bound is read over 'a' and 'A' together, as the sum is."""
    sql = f"""
        SELECT id, x {_source()}
        DECIDE x(BOOL) SUCH THAT sum(x) by (t.g) <= min(t.cap) by (t.g) MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [
        Scoped(
            (Reduce("sum", lambda r: {"x": 1}, by=("gk",)),),
            "<=",
            (Reduce("min", lambda r: {CONSTANT: r["cap"]}, by=("gk",)),),
        )
    ]
    _judged(decidb_cli, scope_solver, sql, [X], clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_a_keyed_decision_has_one_value_per_collated_key(decidb_cli, scope_solver):
    """`per g: y` with `y <= cap`: rows 1 ('a', cap 1) and 2 ('A', cap 2) share one decision, so both hold 1."""
    sql = f"""
        SELECT id, y {_source()}
        DECIDE per t.g: y(INT) SUCH THAT y <= t.cap MAXIMIZE SUM(t.p * y) ORDER BY id"""
    clauses = [Scoped((lambda r: {"y": 1},), "<=", (lambda r: {CONSTANT: r["cap"]},))]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"y": r["p"]}),))
    result = _judged(decidb_cli, scope_solver, sql, [Decision("y", key=("gk",))], clauses, objective)
    assert [row["y"] for row in result[:2]] == [1, 1]


@pytest.mark.diagnose
@pytest.mark.parametrize("order", [(0, 1), (1, 0)], ids=["a-first", "A-first"])
def test_a_variant_is_named_by_its_class_s_least_spelling(decidb_cli, order):
    """'a' (cap 1) and 'A' (cap 3) are one class with two variants of `t.cap`, the column `t.cap - 2` reads (D83); the
    one that cannot hold is named by the class's least spelling, 'A', and its own value, whichever row comes first
    (D69, D70)."""
    rows = ((1, "a", 1, 5.0), (2, "A", 3, 4.0))
    rows = tuple(rows[i] for i in order)
    sql = f"SELECT id, y {_source(rows)} DECIDE per t.g: y(INT) SUCH THAT per t.g: y <= t.cap - 2 MAXIMIZE SUM(y)"
    found = _diagnose_relation.rows(_diagnose_relation.run(decidb_cli, sql, scope="expanded"))
    assert [row["group"] for row in found if row["edit_source"] == "expanded_group"] == ["A, 1"], found


KEYED_COLUMNS = ("id", "k", "g", "cap", "p")
# `cap` is one value per NOCASE group, so an inner `per g` reads one per class.
KEYED_ROWS = ((1, 1, "a", 1, 5.0), (2, 1, "A", 1, 4.0), (3, 2, "b", 2, 3.0), (4, 2, "B", 2, 2.0))


def _keyed_source(rows=KEYED_ROWS):
    return f"FROM (SELECT id, k, g COLLATE NOCASE AS g, cap, p FROM {values(rows, 'v', KEYED_COLUMNS)}) t"


def _keyed_judged(cli, solver, sql, clauses, objective, rows=KEYED_ROWS):
    model = ScopedModel([dict(zip(KEYED_COLUMNS, row), gk=row[2].lower()) for row in rows], [X])
    result = aligned(answer(cli, sql), model, "id")
    assert model.judge_scoped(result, clauses, objective) == pytest.approx(
        model.solve_scoped(solver, clauses, objective)
    )


@pytest.mark.correctness
def test_an_inner_per_counts_each_collated_class_once(decidb_cli, scope_solver):
    """`sum(x) <= sum(per g: cap)`: the classes are {a, A} and {b, B}, so the bound is 1 + 2 = 3, not 6."""
    sql = f"""
        SELECT id, x {_keyed_source()}
        DECIDE x(BOOL) SUCH THAT sum(x) <= sum(per t.g: t.cap) MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [
        Scoped(
            (Reduce("sum", lambda r: {"x": 1}),),
            "<=",
            (Reduce("sum", lambda r: {CONSTANT: r["cap"]}, per=("gk",)),),
            per=(),
        )
    ]
    _keyed_judged(decidb_cli, scope_solver, sql, clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_a_per_key_determines_a_collated_by_group(decidb_cli, scope_solver):
    """`per k: sum(x) by (g) <= 1`: class k = 1 holds 'a' and 'A', which are one `by` group, so `k` determines it."""
    sql = f"""
        SELECT id, x {_keyed_source()}
        DECIDE x(BOOL) SUCH THAT per t.k: sum(x) by (t.g) <= 1 MAXIMIZE SUM(t.p * x) ORDER BY id"""
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}, by=("gk",)),), "<=", (lambda r: {CONSTANT: 1},), per=("k",))]
    _keyed_judged(decidb_cli, scope_solver, sql, clauses, MAXIMIZE_PX)


@pytest.mark.correctness
def test_an_objective_reads_one_collated_by_group(decidb_cli, scope_solver):
    """`MAXIMIZE sum(p * x) by (g)` over rows 'a' and 'A' alone: one `by` group, so the objective is produced once."""
    rows = KEYED_ROWS[:2]
    sql = f"""
        SELECT id, x {_keyed_source(rows)}
        DECIDE x(BOOL) SUCH THAT sum(x) <= 1 MAXIMIZE sum(t.p * x) by (t.g) ORDER BY id"""
    clauses = [Scoped((Reduce("sum", lambda r: {"x": 1}),), "<=", (lambda r: {CONSTANT: 1},), per=())]
    objective = ScopedObjective(ObjSense.MAXIMIZE, (Reduce("sum", lambda r: {"x": r["p"]}, by=("gk",)),))
    _keyed_judged(decidb_cli, scope_solver, sql, clauses, objective, rows)


# 'b' is read on rows 4-5 by the `when` clause and on rows 1-5 by the other: one group, spelled 'B' over every row read.
SPELLING_ROWS = ((1, "a"), (2, "A"), (3, "B"), (4, "b"), (5, "b"))
NARROW = "when t.id >= 4 per t.g: sum(x) by (t.g) >= 3"
WIDE = "per t.g: sum(x) by (t.g) <= 1"


@pytest.mark.diagnose
@pytest.mark.parametrize("clauses", [(NARROW, WIDE), (WIDE, NARROW)], ids=["narrow-first", "wide-first"])
def test_every_finding_spells_a_group_by_its_least_spelling_over_the_rows_read(decidb_cli, clauses):
    """The `when` clause alone reads only 'b'; the other clause reads 'B' too. Each finding on the group names it 'B',
    the least spelling over every row the query read the key on, whichever clause reads it first (ResolveGroupNames)."""
    sql = f"""
        SELECT id, x FROM (SELECT id, g COLLATE NOCASE AS g FROM {values(SPELLING_ROWS, 'v', ('id', 'g'))}) t
        DECIDE x(BOOL) SUCH THAT {clauses[0]} AND {clauses[1]} MAXIMIZE SUM(x)"""
    rows = _diagnose_relation.rows(_diagnose_relation.run(decidb_cli, sql, scope="expanded"))
    groups = sorted((row["clause"], row["group"]) for row in rows if row["edit_source"] == "expanded_group")
    assert groups == sorted([(NARROW, "B"), (WIDE, "B")]), rows


@pytest.mark.diagnose
def test_a_finding_spells_a_group_by_its_least_spelling_when_a_later_row_holds_it(decidb_cli):
    """The group's first row spells it 'b', a later row 'B': the finding names 'B', whatever the first row reads."""
    rows = ((1, "b"), (2, "B"), (3, "b"))
    sql = f"""
        SELECT id, x FROM (SELECT id, g COLLATE NOCASE AS g FROM {values(rows, 'v', ('id', 'g'))}) t
        DECIDE x(BOOL) SUCH THAT per t.g: sum(x) >= 4 MAXIMIZE SUM(x)"""
    found = _diagnose_relation.rows(_diagnose_relation.run(decidb_cli, sql, scope="expanded"))
    assert [row["group"] for row in found if row["edit_source"] == "expanded_group"] == ["B"], found


# Under NOCASE 'C' is the greatest and 'aaa' the least of group `a`, though the bytes order them the other way.
TEXT_ROWS = ((1, "a", 1, "aaa"), (2, "a", 2, "C"), (3, "b", 3, "bb"))
TEXT_SOURCE = f"FROM (SELECT id, g, k, n COLLATE NOCASE AS n FROM {values(TEXT_ROWS, 'v', ('id', 'g', 'k', 'n'))}) t"
TIMELINE = "over (t.id within t.g)"
DIRECT_MODES = pytest.mark.parametrize("mode", ["off", "auto"])


def _bounded(cli, mode, clause):
    sql = f"""SET decide_direct_solve='{mode}';
        SELECT id, x {TEXT_SOURCE} DECIDE x(REAL) SUCH THAT x <= 9 AND {clause} MAXIMIZE sum(x) ORDER BY id"""
    return [row["x"] for row in answer(cli, sql)]


@pytest.mark.correctness
class TestACollatedReducerOverData:
    """A collated MIN or MAX reads two arguments, its value and the key it compares (BindMinMax), and every reducer
    route keeps both: the value is the spelling the collation picks. Each bound is worked out by hand from TEXT_ROWS."""

    @DIRECT_MODES
    @pytest.mark.parametrize(
        "bound, expected",
        [
            pytest.param("length(max(t.n))", [1, 1, 1], id="side-table-max"),
            pytest.param("length(min(t.n))", [3, 3, 3], id="side-table-min"),
            pytest.param("length(max(when t.id <> 2: t.n))", [2, 2, 2], id="reads-a-when"),
            pytest.param("length(max(t.n) by (t.g))", [1, 1, 2], id="by-key"),
            pytest.param("length(max(per t.k: t.n) by (t.g))", [1, 1, 2], id="counted-by-class"),
            pytest.param("length(min(max(t.n) by (t.g)))", [2, 2, 2], id="nested"),
        ],
    )
    def test_a_bound_reads_the_spelling_the_collation_picks(self, decidb_cli, mode, bound, expected):
        assert _bounded(decidb_cli, mode, f"x <= {bound}") == expected

    @DIRECT_MODES
    def test_a_group_bound_takes_a_side_table(self, decidb_cli, mode):
        """A rule's group aggregate reads one argument, so a collated reducer is no group bound
        (ReadsOnlyGroupReducers): group `a` sums to 1 and group `b` to 2, through a side table."""
        sums = _bounded(decidb_cli, mode, "sum(x) by (t.g) <= length(max(t.n) by (t.g))")
        assert sums[0] + sums[1] == pytest.approx(1) and sums[2] == pytest.approx(2), sums

    @DIRECT_MODES
    def test_a_coefficient_reads_it_too(self, decidb_cli, mode):
        """`length(max(n))` is 1 here, so the three decisions share a budget of 5 at weight 1."""
        assert sum(_bounded(decidb_cli, mode, "sum(x * length(max(t.n))) <= 5")) == pytest.approx(5)

    def test_the_default_collation_makes_a_plain_column_collated(self, decidb_cli):
        sql = f"""SET default_collation='nocase';
            SELECT id, x FROM {values(TEXT_ROWS, 't', ('id', 'g', 'k', 'n'))}
            DECIDE x(REAL) SUCH THAT x <= length(max(t.n)) MAXIMIZE sum(x) ORDER BY id"""
        assert [row["x"] for row in answer(decidb_cli, sql)] == [1, 1, 1]

    @pytest.mark.parametrize(
        "bound, expected",
        [
            # `at(first)` reads aaa, aaa, bb: its one frame stands once, though the reducer reads it twice.
            pytest.param(f"length(max(at(first: t.n) {TIMELINE}))", [2, 2, 2], id="reducer-over-a-point"),
            pytest.param(f"length(min(at(first: t.n) {TIMELINE}) by (t.g))", [3, 3, 2], id="reducer-by-key"),
            # The range's own reducer compares under the collation, in each timeline.
            pytest.param(f"length(max(from first to last: t.n) {TIMELINE})", [1, 1, 2], id="range-max"),
            pytest.param(f"length(min(from first to last: t.n) {TIMELINE})", [3, 3, 2], id="range-min"),
            pytest.param(f"length(min(max(from first to last: t.n) {TIMELINE}))", [2, 2, 2], id="reducer-over-a-range"),
        ],
    )
    def test_a_frame_reads_it_too(self, decidb_cli, bound, expected):
        assert _bounded(decidb_cli, "off", f"x <= {bound}") == expected

    def test_the_tree_folds_a_range_with_its_bound_reducer(self, decidb_cli):
        """The tree evaluator folds a range with the aggregate its marker is bound to, `arg_max` over the value and
        its key: group `a` reads 'C', where a byte-order `max` reads 'aaa' and gives [3, 3, 2]."""
        cli = decidb_cli.with_env({"DECIDB_EVALUATOR": "tree"})
        assert _bounded(cli, "off", f"x <= length(max(from first to last: t.n) {TIMELINE})") == [1, 1, 2]

    def test_a_range_over_collated_text_takes_no_number_else(self, decidb_cli):
        """A missing position's `else` would need a key under the collation, and a number has none."""
        clause = f"x <= length(max(from previous to next else 0: t.n) {TIMELINE})"
        sql = f"SELECT id, x {TEXT_SOURCE} DECIDE x(REAL) SUCH THAT {clause} MAXIMIZE sum(x)"
        result = decidb_cli.execute_raw(sql)
        assert "else 0 stands in a collated payload" in result.stderr, result.stderr
        assert_no_internal_leak(result, "an else over a collated payload")


ORDER_ROWS = ((1, "a", 10.0), (2, "A", 10.0), (3, "b", 30.0))
TWO_VALUES_ROW_ORDERS = (
    ((1, "a", 10.0), (2, "A", 20.0), (3, "b", 30.0)),
    ((2, "A", 20.0), (1, "a", 10.0), (3, "b", 30.0)),
)


def _ordered(clause, rows=ORDER_ROWS, over="t.k", collation="COLLATE NOCASE"):
    source = f"(SELECT id, k {collation} AS k, v FROM {values(rows, 'r', ('id', 'k', 'v'))}) t"
    return f"""SELECT id, x FROM {source} DECIDE x(REAL)
        SUCH THAT x <= 50 AND x <= {clause.format(over=f"over ({over})")} MAXIMIZE sum(x) ORDER BY id"""


@pytest.mark.correctness
class TestACollatedOrderKey:
    """A frame's order key compares as ORDER BY does (D74): under NOCASE 'a' and 'A' are one position, so `dense_rank`
    and a range count 2 positions, and neither reads the other as its `previous`."""

    def test_a_range_counts_positions_as_dense_rank_does(self, decidb_cli):
        sql = _ordered("count(from first to last: t.v) {over}")
        assert [row["x"] for row in answer(decidb_cli, sql)] == [2, 2, 2]

    def test_one_position_reads_no_previous_inside_itself(self, decidb_cli):
        """Rows 1 and 2 are the first position and read the `else`; row 3 reads their one value."""
        sql = _ordered("at(previous else 0: t.v) {over}")
        assert [row["x"] for row in answer(decidb_cli, sql)] == [0, 0, 10]

    def test_a_key_written_under_a_collation_compares_under_it(self, decidb_cli):
        """The column is plain text; the `COLLATE` in the order key alone makes 'a' and 'A' one position."""
        sql = _ordered("at(previous else 0: t.v) {over}", over="t.k COLLATE NOCASE", collation="")
        assert [row["x"] for row in answer(decidb_cli, sql)] == [0, 0, 10]

    def test_plain_text_keeps_its_three_positions(self, decidb_cli):
        """With no collation 'A', 'a' and 'b' are three positions."""
        sql = _ordered("count(from first to last: t.v) {over}", collation="")
        assert [row["x"] for row in answer(decidb_cli, sql)] == [3, 3, 3]

    @pytest.mark.parametrize(
        "bound_under, run_under, expected",
        [("''", "'nocase'", [3, 3, 3]), ("'nocase'", "''", [2, 2, 2])],
        ids=["bound-binary", "bound-nocase"],
    )
    def test_a_prepared_frame_compares_as_it_was_bound(self, decidb_cli, bound_under, run_under, expected):
        """A prepared ORDER BY keeps the collation binding settled, so a later `SET` moves neither it nor a frame."""
        prepared = _ordered("count(from first to last: t.v) {over}", collation="")
        sql = f"""SET default_collation={bound_under}; PREPARE f AS {prepared};
            SET default_collation={run_under}; EXECUTE f;"""
        assert [row["x"] for row in answer(decidb_cli, sql)] == expected

    def test_a_plan_round_trip_keeps_a_computed_keys_collation(self, decidb_cli):
        """A function's result loses its collated type in a round trip, so the plan holds the collation itself: 'az'
        and 'Az' stay one position, as `dense_rank` keeps them."""
        counted = _ordered("count(from first to last: t.v) {over}", over="(t.k || 'z') COLLATE NOCASE", collation="")
        assert [row["x"] for row in answer(decidb_cli, f"PRAGMA verify_serializer; {counted}")] == [2, 2, 2]

    @pytest.mark.parametrize("rows", [ORDER_ROWS, ORDER_ROWS[1::-1] + ORDER_ROWS[2:]], ids=["a-first", "A-first"])
    def test_a_message_names_two_positions_not_two_spellings(self, decidb_cli, rows):
        """'a' and 'A' are one position, so a pair of them proves nothing: the objective reads 'A' and 'b'."""
        source = f"(SELECT id, k COLLATE NOCASE AS k, v FROM {values(rows, 'r', ('id', 'k', 'v'))}) t"
        sql = f"SELECT id, y FROM {source} DECIDE per (): y(REAL) SUCH THAT y <= 3 MAXIMIZE at(previous else 0: y) over (t.k)"
        result = decidb_cli.execute_raw(sql)
        assert "reads more than one position over the kept rows (t.k = 'A', 'b')" in result.stderr, result.stderr

    @pytest.mark.parametrize("rows", TWO_VALUES_ROW_ORDERS, ids=["a-first", "A-first"])
    def test_a_position_is_named_by_its_least_spelling(self, decidb_cli, rows):
        """D69: the label of a position with two spellings does not depend on which arrives first."""
        result = decidb_cli.execute_raw(_ordered("at(previous else 0: t.v) {over}", rows))
        assert "finds 2 rows at t.k = 'A'" in result.stderr, result.stderr

    @pytest.mark.parametrize("rows", [ORDER_ROWS, ORDER_ROWS[1::-1] + ORDER_ROWS[2:]], ids=["a-first", "A-first"])
    def test_a_variant_is_named_by_its_positions_least_spelling(self, decidb_cli, rows):
        """Each position is its own variant of the class, and DIAGNOSE names it `g, A` in either row order."""
        source = f"""(SELECT id, 'g' AS g, k COLLATE NOCASE AS k, v FROM {values(rows, 'r', ('id', 'k', 'v'))}) t"""
        sql = f"""PRAGMA diagnose_decide_infeasible_slack_scope='expanded';
            DIAGNOSE SELECT id, y FROM {source} DECIDE per t.g: y(INT)
            SUCH THAT per t.g: y = at(previous else 5: t.v) over (t.k) MAXIMIZE sum(y)"""
        result = decidb_cli.execute_raw(sql)
        assert "g, A" in result.stdout + result.stderr, result.stdout + result.stderr
        assert "g, a" not in result.stdout + result.stderr, result.stdout + result.stderr

    @pytest.mark.parametrize("first", ["1 month", "30 days"])
    def test_every_message_gives_a_position_one_name(self, decidb_cli, first):
        """'1 month' and '30 days' are one interval, and `Value` comparison ties them, so the least is the sort key's, as
        DIAGNOSE reads it: raw order puts 30 days first."""
        spellings = [first, "30 days" if first == "1 month" else "1 month", "2 months"]
        rows = values(tuple((i + 1, k, 10.0 * (i + 1)) for i, k in enumerate(spellings)), "r", ("id", "k", "v"))
        source = f"(SELECT id, CAST(k AS INTERVAL) AS k, v FROM {rows}) t"
        result = decidb_cli.execute_raw(
            f"SELECT id, x FROM {source} DECIDE x(REAL) SUCH THAT x <= at(previous else 0: t.v) over (t.k) MAXIMIZE sum(x)"
        )
        assert "finds 2 rows at t.k = '30 days'" in result.stderr, result.stderr
        result = decidb_cli.execute_raw(
            f"SELECT id, y FROM {source} DECIDE per (): y(REAL) SUCH THAT y <= 3 MAXIMIZE at(previous else 0: y) over (t.k)"
        )
        assert "(t.k = '30 days', '2 months')" in result.stderr, result.stderr

    def test_one_position_holds_one_value(self, decidb_cli):
        """D73: 'a' and 'A' are one position, whose two rows read 10 and 20."""
        rows = ((1, "a", 10.0), (2, "A", 20.0), (3, "b", 30.0))
        result = decidb_cli.execute_raw(_ordered("at(previous else 0: t.v) {over}", rows))
        assert "a position must hold one row" in result.stderr, result.stderr
        assert_no_internal_leak(result, "two rows at one collated position")
