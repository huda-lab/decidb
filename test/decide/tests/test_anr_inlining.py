"""Helpers written where they are read (D83; ``06_model_formulation/done.md``, "Inlining a
helper").

The value rewrite makes a reducer -- or a range frame -- over decisions a helper decision,
defined once per instance (a `by` group, a window) and read where the clause reads the
reducer. Stage 06 writes a helper's members into the rows, the objective or the other
helper that read it when that adds no nonzero to the model (an instance of ``m`` terms read
``k`` times costs ``m * k`` inlined, ``m + 1 + k`` defined), so the helper has no column and
no definition. A MIN or MAX helper is its member where every group has one (deck p. 112).

Each case pins the model's size, worked out by hand in its docstring, and the answer,
judged by ``_anr_oracle`` (which reads no SQL). The table is phase 6's: products A (three
periods), B (two) and C (one), so `previous` misses at each timeline's start.
"""

import re

import pytest

from ._anr_cases import F1_SUM
from ._anr_oracle import (
    FIRST,
    LAST,
    AnrModel,
    At,
    Col,
    Const,
    Decision,
    Objective,
    Over,
    Range,
    Var,
    clause,
    ge,
    le,
    ne,
    next_,
    previous,
    reduce,
)
from ._diagnose_relation import rows as diagnose_rows
from ._diagnose_relation import run as run_diagnose
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import answer, dump_counts, scope_solver, table, table_rows  # noqa: F401 -- fixture
from .test_anr_composition import CAP, EDGE, EDGE_ROWS, MAXIMIZE_X, MINIMIZE_X, TIMELINE, TL, X, _edge, _judged
from solver.types import ObjSense, VarType

Y = Var("y")
ROWS = table_rows(EDGE, EDGE_ROWS)
BOTH_SOLVERS = pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
_ROW = re.compile(r"^row \d+: sense=(\S) rhs=(\S+) nnz=\d+ \|(.*)$", re.M)


def _rows(dump: str) -> set:
    """The dump's linear rows, each `(sense, rhs, {(column, coefficient)})`, in no order."""
    rows = set()
    for sense, rhs, terms in _ROW.findall(dump):
        pairs = frozenset((int(c), float(v)) for c, v in (term.split(":") for term in terms.split()))
        rows.add((sense, float(rhs), pairs))
    return rows


def _model(cli, sql, tmp_path):
    return dump_counts(cli.dump_model(sql, tmp_path / "model.dump"))


# ---------------------------------------------------------------------------
# Into rows: a range read by its anchor's row
# ---------------------------------------------------------------------------


@pytest.mark.correctness
class TestRangeIntoRows:
    @BOTH_SOLVERS
    @pytest.mark.parametrize("clause_sql, body, counts", [
        # Each window read by its anchor alone: six rows of x and the two before, no helper.
        pytest.param(f"x >= sum(from 2 previous to previous else 0: x) {TL} - t.w",
                     clause(ge(X, Range("sum", previous(2), previous(), X, TIMELINE, fallback=Const(0)) - Col("w"))),
                     {"num_vars": 6, "num_rows": 6}, id="sum-with-else"),
        # No `else`: the window at each timeline's first period reads nothing, absent, and
        # its anchor's instance is skipped -- A, B and C each lose one: three rows.
        pytest.param(f"x >= sum(from 2 previous to previous: x) {TL} - t.w",
                     clause(ge(X, Range("sum", previous(2), previous(), X, TIMELINE) - Col("w"))),
                     {"num_vars": 6, "num_rows": 3}, id="sum-absent-skips"),
        # The window holds its anchor, so it is never empty: an AVG's 1/n is in its terms.
        # C's window is C itself, so its row is `x - x <= w`, no row at all: five rows.
        pytest.param(f"x <= avg(from previous to next: x) {TL} + t.w",
                     clause(le(X, Range("avg", previous(), next_(), X, TIMELINE) + Col("w"))),
                     {"num_vars": 6, "num_rows": 5}, id="avg"),
        # An `else` constant moves to the row's bound with the rest of its data.
        pytest.param(f"x <= sum(from previous to previous else 2.5: x) {TL} + 1",
                     clause(le(X, Range("sum", previous(), previous(), X, TIMELINE, fallback=Const(2.5)) + 1)),
                     {"num_vars": 6, "num_rows": 6}, id="else-constant"),
    ])
    def test_a_window_read_once_has_no_column(self, request, cli_fixture, scope_solver, tmp_path, clause_sql,
                                              body, counts):
        cli = request.getfixturevalue(cli_fixture)
        sql = _edge(clause_sql)
        assert _model(cli, sql, tmp_path) == counts
        _judged(cli, scope_solver, sql, ROWS, [Decision("x")], [CAP, body], MAXIMIZE_X)

    def test_a_window_every_row_reads_keeps_its_column(self, decidb_cli, scope_solver, tmp_path):
        """One timeline, one window for every anchor (`from first to last`): six terms read by
        six rows would cost 36 nonzeros inlined against 13 defined, so the helper stays --
        one column and its definition beside the six rows."""
        sql = _edge("x <= sum(from first to last: x) over (t.period) - t.w")
        assert _model(decidb_cli, sql, tmp_path) == {"num_vars": 6 + 1, "num_rows": 6 + 1}
        body = clause(le(X, Range("sum", FIRST, LAST, X, Over("period")) - Col("w")))
        _judged(decidb_cli, scope_solver, sql, ROWS, [Decision("x")], [CAP, body], MAXIMIZE_X)

    def test_windows_shared_by_a_few_rows_are_inlined(self, decidb_cli, scope_solver, tmp_path):
        """`from first to last within product`: A's window (three terms, three rows) alone
        would grow, but B's (two, two) and C's (one, one) shrink more -- 14 nonzeros inlined
        against 15 defined -- and a helper is inlined whole: the rows of x alone, five, as
        C's is `x >= x - 10`, no row."""
        sql = _edge(f"x >= sum(from first to last: x) {TL} - 10")
        assert _model(decidb_cli, sql, tmp_path) == {"num_vars": 6, "num_rows": 5}
        body = clause(ge(X, Range("sum", FIRST, LAST, X, TIMELINE) - 10))
        _judged(decidb_cli, scope_solver, sql, ROWS, [Decision("x")], [CAP, body], MAXIMIZE_X)

    def test_in_the_objective(self, decidb_cli, scope_solver, tmp_path):
        """The objective reads each window once: written into it, the model has the six
        decisions and no row (their caps are a box)."""
        objective_sql = f"MAXIMIZE sum(x) - sum(sum(from previous to previous else 0: x) {TL})"
        sql = _edge("x >= 0", objective=objective_sql)
        assert _model(decidb_cli, sql, tmp_path) == {"num_vars": 6, "num_rows": 0}
        window = Range("sum", previous(), previous(), X, TIMELINE, fallback=Const(0))
        objective = Objective(ObjSense.MAXIMIZE, reduce("sum", X) - reduce("sum", window))
        _judged(decidb_cli, scope_solver, sql, ROWS, [Decision("x")], [CAP], objective)


# ---------------------------------------------------------------------------
# Into another helper's members
# ---------------------------------------------------------------------------


@pytest.mark.correctness
class TestIntoMembers:
    def test_the_deck_writes_the_sums_in(self, decidb_cli, tmp_path):
        """F1 with an inner SUM (deck p. 119): each (rid, cid) sum is written into its
        region's members, and each region's into the rows reading it -- West's three terms
        into both of its rows -- so the model is the deck's three rows over the six
        shipments: ship1 + ship2 <= 30 - 7.5, and ship3 + ship5 + ship6 (ship4's `p` is
        false) within 50 - 1.5 and 45 - 7.5."""
        dump = decidb_cli.dump_model(F1_SUM.sql, tmp_path / "f1.dump")
        assert dump_counts(dump) == {"num_vars": 6, "num_rows": 3}
        west = frozenset({(2, 1.0), (4, 1.0), (5, 1.0)})
        assert _rows(dump) == {("<", 22.5, frozenset({(0, 1.0), (1, 1.0)})), ("<", 48.5, west), ("<", 37.5, west)}

    @BOTH_SOLVERS
    def test_group_sums_into_the_objectives_maximum(self, request, cli_fixture, scope_solver, tmp_path):
        """Q4's shape: `MINIMIZE max(per product: sum(x * v) by (product))`. The MAX, pushed
        down, needs its envelope alone, so each product's sum is written into its envelope
        row: the six decisions and the MAX's column; three envelope rows and three rows of
        the clause (whose sums stay in place, the aggregate shape)."""
        cli = request.getfixturevalue(cli_fixture)
        sql = _edge("per t.product: sum(x) by (t.product) >= 2",
                    objective="MINIMIZE max(per t.product: sum(x * t.v) by (t.product))")
        assert _model(cli, sql, tmp_path) == {"num_vars": 6 + 1, "num_rows": 3 + 3}
        body = clause(ge(reduce("sum", X, by=("product",)), 2), per=("product",))
        objective = Objective(ObjSense.MINIMIZE, reduce("max", reduce("sum", X * Col("v"), by=("product",)),
                                                         per=("product",)))
        _judged(cli, scope_solver, sql, ROWS, [Decision("x")], [CAP, body], objective)

    def test_a_range_into_the_reducer_around_it(self, decidb_cli, scope_solver, tmp_path):
        """`sum(x - sum(from previous to previous else 0: x) over ...) <= 6`: each window is
        written into the outer sum's member at its anchor, and the outer sum, read by its
        one row, into that row -- one row over the six decisions."""
        sql = _edge(f"sum(x - sum(from previous to previous else 0: x) {TL}) <= 6")
        assert _model(decidb_cli, sql, tmp_path) == {"num_vars": 6, "num_rows": 1}
        window = Range("sum", previous(), previous(), X, TIMELINE, fallback=Const(0))
        body = clause(le(reduce("sum", X - window), 6))
        _judged(decidb_cli, scope_solver, sql, ROWS, [Decision("x")], [CAP, body], MAXIMIZE_X)


# ---------------------------------------------------------------------------
# A MIN or MAX of one member
# ---------------------------------------------------------------------------


@pytest.mark.correctness
class TestSingleton:
    @BOTH_SOLVERS
    def test_a_window_of_one_position_is_its_member(self, request, cli_fixture, scope_solver, tmp_path):
        """`max(from previous to previous else 0: x)` reads one position or its `else`: every
        window is one member, so the MAX is linear and written into its anchor's row --
        no binary, no Big-M, six rows."""
        cli = request.getfixturevalue(cli_fixture)
        sql = _edge(f"x <= max(from previous to previous else 0: x) {TL} + t.w")
        dump = cli.dump_model(sql, tmp_path / "model.dump")
        assert dump_counts(dump) == {"num_vars": 6, "num_rows": 6}
        assert "bin=1" not in dump, dump
        body = clause(le(X, Range("max", previous(), previous(), X, TIMELINE, fallback=Const(0)) + Col("w")))
        _judged(cli, scope_solver, sql, ROWS, [Decision("x")], [CAP, body], MAXIMIZE_X)

    def test_one_member_needs_no_bound_to_close(self, decidb_cli_highs, scope_solver):
        """`x <= max(... : y)` pushes the MAX up, so it must be pinned to a member from above,
        which a Big-M over `y`'s box does -- and `y` has none. A window of one member is that
        member: no Big-M is asked for, and HiGHS (which lowers every MIN/MAX) answers."""
        sql = f"""SELECT id, x, y {table('t', EDGE, EDGE_ROWS)} DECIDE x(INT), y(INT)
            SUCH THAT x <= 9 AND x <= max(from previous to previous: y) {TL}
            MAXIMIZE sum(x) - sum(y) ORDER BY id"""
        body = clause(le(X, Range("max", previous(), previous(), Y, TIMELINE)))
        objective = Objective(ObjSense.MAXIMIZE, reduce("sum", X) - reduce("sum", Y))
        _judged(decidb_cli_highs, scope_solver, sql, ROWS, [Decision("x"), Decision("y")], [CAP, body], objective)


# ---------------------------------------------------------------------------
# What keeps a helper's column
# ---------------------------------------------------------------------------


@pytest.mark.correctness
class TestKept:
    @pytest.mark.parametrize("clause_sql, body", [
        # `<>` states each instance in two rows of its disjunction.
        pytest.param(f"x <> sum(from previous to previous else 0: x) {TL}",
                     clause(ne(X, Range("sum", previous(), previous(), X, TIMELINE, fallback=Const(0)))),
                     id="not-equal"),
        # A bound on the helper alone is its column's box, which an inlined helper has not.
        pytest.param(f"sum(from previous to previous else 0: x) {TL} <= 5",
                     clause(le(Range("sum", previous(), previous(), X, TIMELINE, fallback=Const(0)), 5)),
                     id="a-bound-on-the-helper"),
    ])
    def test_a_reader_that_needs_the_column(self, decidb_cli, scope_solver, tmp_path, clause_sql, body):
        sql = _edge(clause_sql)
        assert _model(decidb_cli, sql, tmp_path)["num_vars"] > 6
        _judged(decidb_cli, scope_solver, sql, ROWS, [Decision("x")], [CAP, body], MAXIMIZE_X)

    def test_a_maximum_closing_on_a_sum_two_members_read(self, decidb_cli_gurobi, scope_solver, tmp_path):
        """`max(sum(x + 1) by (t.product)) >= 15` with `x` unbounded: the MAX is pinned from
        above with no Big-M to read, so Gurobi states it natively, over columns. A's sum is
        read by both of A's rows, and written into each it would be a column and a row twice
        where the helper is one: the helpers stay -- six columns, their two definitions. (With
        native constructs off there is no Big-M to lower it with, and the query is refused.)"""
        cli = decidb_cli_gurobi.with_env({"DECIDB_NATIVE_CONSTRUCTS": "on"})
        rows = ((1, "A", 1, 4, 2), (2, "A", 2, 6, 1), (3, "B", 1, 7, 2))
        sql = f"""SELECT id, x {table('t', EDGE, rows)} DECIDE x(INT)
            SUCH THAT max(sum(x + 1) by (t.product)) >= 15 MINIMIZE sum(x) ORDER BY id"""
        assert _model(cli, sql, tmp_path) == {"num_vars": 6, "num_rows": 2}
        body = clause(ge(reduce("max", reduce("sum", X + 1, by=("product",))), 15))
        _judged(cli, scope_solver, sql, table_rows(EDGE, rows), [Decision("x")], [body], MINIMIZE_X)

    def test_a_maximum_of_several_members(self, decidb_cli, scope_solver, tmp_path):
        """`max(from previous to next: x)`: most windows hold two or three members, so the
        MAX keeps its columns, a window of one among them defined by an equality."""
        sql = _edge(f"x >= max(from previous to next: x) {TL} - 1")
        assert _model(decidb_cli, sql, tmp_path)["num_vars"] > 6
        body = clause(ge(X, Range("max", previous(), next_(), X, TIMELINE) - 1))
        _judged(decidb_cli, scope_solver, sql, ROWS, [Decision("x")], [CAP, body], MAXIMIZE_X)


# ---------------------------------------------------------------------------
# DIAGNOSE through an inlined helper
# ---------------------------------------------------------------------------


@pytest.mark.diagnose
@pytest.mark.parametrize("scope", ["query", "expanded"])
def test_a_diagnosis_reads_the_rows_as_written(decidb_cli, scope):
    """`x >= 3` beside `x + sum(from previous to previous else 0: x) <= 4`: every row with a
    previous one reads 3 + 3. The window of one position is written into its anchor's row,
    so the repair model is the one the point frame builds (`at(previous else 0: x)`, which
    has no helper): the same findings, no definition among them."""
    ranged, point = "sum(from previous to previous else 0: x)", "at(previous else 0: x)"

    def findings(frame):
        sql = _edge(f"x >= 3 AND x + {frame} {TL} <= 4")
        result = run_diagnose(decidb_cli, sql, scope=scope)
        assert result.returncode == 0, result.stderr
        assert_no_internal_leak(result, "a diagnosis", sql)
        # Each quotes its own clause: read in one spelling.
        return [{key: value.replace(point, ranged) for key, value in row.items()} for row in diagnose_rows(result)]
    written = findings(ranged)
    assert written and {row["state"] for row in written} == {"infeasible"}, written
    assert written == findings(point)


@pytest.mark.diagnose
@BOTH_SOLVERS
def test_the_achievable_objective_counts_an_inlined_helpers_constants(request, cli_fixture, scope_solver):
    """The objective sums each window's previous `x`, or its `else 2.5` at a timeline's first
    period -- A's, B's and C's, 7.5 in all. Written into the objective, the windows' constants
    are its offset, as a helper's definition carried them: the achievable objective of the
    repair (`x >= 3` loosened until six rows fit `sum(x) <= 10`) is the oracle's, 7.5 included."""
    cli = request.getfixturevalue(cli_fixture)
    window = Range("sum", previous(), previous(), X, TIMELINE, fallback=Const(2.5))
    sql = _edge("x >= 3 AND sum(x) <= 10", decide="x(REAL)",
                objective=f"MINIMIZE sum(sum(from previous to previous else 2.5: x) {TL})")
    result = run_diagnose(cli, sql)
    assert result.returncode == 0, result.stderr
    assert_no_internal_leak(result, "a diagnosis", sql)
    findings = diagnose_rows(result)
    repairs = [row for row in findings if row["clause"] == "x >= 3"]
    assert len(repairs) == 1, findings
    loosened = 3 - float(repairs[0]["amount"])
    model = AnrModel(ROWS, [Decision("x", var_type=VarType.CONTINUOUS)])
    expected = model.solve(scope_solver, [CAP, clause(ge(X, loosened)), clause(le(reduce("sum", X), 10))],
                           Objective(ObjSense.MINIMIZE, reduce("sum", window)))
    achieved = float(next(row["amount"] for row in findings if row["edit_source"] == "achievable_objective"))
    # The repair's amount is read as displayed (1.33333), so the oracle's optimum is too.
    assert achieved == pytest.approx(expected.objective, abs=1e-3)
    assert achieved == pytest.approx(12.5, abs=1e-6)  # 7.5 of it the windows' constants
