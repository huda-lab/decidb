"""Frames composed with reducers, ABS, `norm` and MIN/MAX (``syntax_reference.md`` §5.6;
D81, D82).

A frame that may select no position, inside a reducer over decisions, drops that member
where it selects nothing (deck p. 50): an AVG divides by the members left, and a reducer
left with none is absent -- its comparison skips that instance, a reducer around it drops
the member reading it, and the objective cannot be skipped. Such a reducer is a helper
decision whose members execution lists. Every case below is judged by ``_anr_oracle``,
which reads no SQL, on both solvers where the shape has two formulations: a product
``C`` with one period makes a reducer over ``previous`` absent there.
"""

import pytest

from ._anr_cases import F3_COLUMNS, F3_DATA
from ._anr_oracle import (
    FIRST,
    LAST,
    Abs,
    AnrModel,
    At,
    Col,
    Const,
    Decision,
    Objective,
    Over,
    Range,
    ScopeError,
    Var,
    clause,
    ge,
    le,
    ne,
    next_,
    previous,
    reduce,
)
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import answer, dump_counts, scope_solver, table, table_rows, values  # noqa: F401 -- fixture
from solver.types import ObjSense, SolverStatus

X, SHIP = Var("x"), Var("ship")
MAXIMIZE_X = Objective(ObjSense.MAXIMIZE, reduce("sum", X))
MINIMIZE_X = Objective(ObjSense.MINIMIZE, reduce("sum", X))
BOTH_SOLVERS = pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])

EDGE = ("id", "product", "period", "v", "w")
#: Products A (three periods), B (two) and C (one): `previous` and `next` miss at each
#: timeline's end, and at C's only period.
EDGE_ROWS = ((1, "A", 1, 4, 2), (2, "A", 2, 6, 1), (3, "A", 3, 1, 3), (4, "B", 1, 7, 2), (5, "B", 2, 3, 1),
             (6, "C", 1, 5, 2))
TIMELINE = Over("period", within=("product",))
TL = "over (t.period within t.product)"
CAP = clause(le(X, 9))


def _edge(clause_sql, objective="MAXIMIZE sum(x)", rows=EDGE_ROWS, decide="x(INT)", select="x"):
    return f"""SELECT id, {select} {table('t', EDGE, rows)} DECIDE {decide}
        SUCH THAT x <= 9 AND {clause_sql} {objective} ORDER BY id"""


def _judged(cli, solver, sql, rows, decisions, clauses, objective):
    """DeciDB's answer, judged by the oracle against its own optimum; returns it."""
    model = AnrModel(rows, decisions)
    expected = model.solve(solver, clauses, objective)
    assert expected.status == SolverStatus.OPTIMAL, expected.status
    result = cli.execute_raw(sql)
    assert_no_internal_leak(result, sql)
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    reached = model.judge(answer(cli, sql), clauses, objective)
    assert reached == pytest.approx(expected.objective, abs=1e-6)
    return reached


def _sense(objective: Objective) -> str:
    return "MAXIMIZE" if objective.sense == ObjSense.MAXIMIZE else "MINIMIZE"


PREV_X = At(previous(), X, TIMELINE)
NEXT_X = At(next_(), X, TIMELINE)


# ---------------------------------------------------------------------------
# A member dropped from a reducer over decisions (deck example 3, p. 46)
# ---------------------------------------------------------------------------


@pytest.mark.correctness
class TestMembersDropped:
    @BOTH_SOLVERS
    @pytest.mark.parametrize("clause_sql, body, objective", [
        pytest.param(f"per t.product: sum(at(previous: x) {TL}) by (t.product) <= 4",
                     clause(le(reduce("sum", PREV_X, by=("product",)), 4), per=("product",)), MAXIMIZE_X,
                     id="deck-example-3"),
        pytest.param(f"sum(at(previous: x) {TL}) by (t.product) >= 1",
                     clause(ge(reduce("sum", PREV_X, by=("product",)), 1)), MINIMIZE_X,
                     id="an-absent-group-has-no-row"),
        pytest.param(f"sum(x + at(previous: t.v) {TL}) by (t.product) <= 12",
                     clause(le(reduce("sum", X + At(previous(), Col("v"), TIMELINE), by=("product",)), 12)),
                     MAXIMIZE_X, id="a-data-frame-drops-the-decision-beside-it"),
        pytest.param(f"avg(at(previous: x) {TL}) by (t.product) <= 3",
                     clause(le(reduce("avg", PREV_X, by=("product",)), 3)), MAXIMIZE_X,
                     id="avg-divides-by-the-members-left"),
        pytest.param(f"max(at(previous: x) {TL}) by (t.product) <= 4",
                     clause(le(reduce("max", PREV_X, by=("product",)), 4)), MAXIMIZE_X, id="max-easy"),
        pytest.param(f"max(at(previous: x) {TL}) by (t.product) >= 8",
                     clause(ge(reduce("max", PREV_X, by=("product",)), 8)), MINIMIZE_X, id="max-hard"),
        pytest.param(f"min(at(next: x) {TL}) by (t.product) <= 2",
                     clause(le(reduce("min", NEXT_X, by=("product",)), 2)), MAXIMIZE_X, id="min-hard"),
        pytest.param(f"min(at(next: x) {TL}) by (t.product) >= 3",
                     clause(ge(reduce("min", NEXT_X, by=("product",)), 3)), MINIMIZE_X, id="min-easy"),
        pytest.param(f"x <= sum(at(previous: x) {TL}) by (t.product)",
                     clause(le(X, reduce("sum", PREV_X, by=("product",)))), MAXIMIZE_X,
                     id="beside-a-decision"),
        pytest.param(f"x + sum(at(previous: x) {TL}) <= 20",
                     clause(le(X + reduce("sum", PREV_X), 20)), MAXIMIZE_X, id="over-every-row"),
        pytest.param(f"abs(sum(at(previous: x) {TL}) by (t.product) - 3) <= 1",
                     clause(le(Abs(reduce("sum", PREV_X, by=("product",)) - 3), 1)), MAXIMIZE_X,
                     id="under-abs-at-the-root"),
        pytest.param(f"sum(at(previous: x) {TL}) by (t.product) <> 0",
                     clause(ne(reduce("sum", PREV_X, by=("product",)), 0)), MINIMIZE_X, id="not-equal"),
        pytest.param(f"sum(when t.w >= 2: at(next: x) {TL}) by (t.product) <= 4",
                     clause(le(reduce("sum", NEXT_X, when=lambda r: r["w"] >= 2, by=("product",)), 4)), MAXIMIZE_X,
                     id="its-own-when"),
        pytest.param(f"sum(per t.v: at(previous: x) {TL}) by (t.product) <= 5",
                     clause(le(reduce("sum", PREV_X, per=("v",), by=("product",)), 5)), MAXIMIZE_X,
                     id="its-own-per"),
        pytest.param(f"sum(t.w * at(previous: x) {TL}) by (t.product) <= 6",
                     clause(le(reduce("sum", Col("w") * PREV_X, by=("product",)), 6)), MAXIMIZE_X,
                     id="a-data-factor"),
        pytest.param(f"sum(at(previous: x) {TL} + at(next: x) {TL}) by (t.product) <= 10",
                     clause(le(reduce("sum", PREV_X + NEXT_X, by=("product",)), 10)), MAXIMIZE_X,
                     id="two-frames-in-a-member"),
        pytest.param(f"max(at(previous: x) {TL}) + sum(x) <= 30",
                     clause(le(reduce("max", PREV_X) + reduce("sum", X), 30)), MAXIMIZE_X,
                     id="composed-min-max"),
        pytest.param(f"x <= 5 AND sum(at(previous: x) {TL}) by (t.product) >= 4",
                     [clause(le(X, 5)), clause(ge(reduce("sum", PREV_X, by=("product",)), 4))], MINIMIZE_X,
                     id="and-splits"),
    ])
    def test_a_member_is_dropped_where_its_frame_selects_nothing(self, request, scope_solver, cli_fixture,
                                                                 clause_sql, body, objective):
        """C's group reads nothing -- its reducer is absent, so its instance is skipped --
        and every other group reads the members whose frame selects a position."""
        bodies = body if isinstance(body, list) else [body]
        _judged(request.getfixturevalue(cli_fixture), scope_solver, _edge(clause_sql, f"{_sense(objective)} sum(x)"),
                table_rows(EDGE, EDGE_ROWS), [Decision("x")], [CAP] + bodies, objective)

    @pytest.mark.parametrize("clause_sql, body, objective", [
        pytest.param(f"sum(sum(from next to 2 next: x) {TL}) by (t.product) <= 6",
                     clause(le(reduce("sum", Range("sum", next_(), next_(2), X, TIMELINE), by=("product",)), 6)),
                     MAXIMIZE_X, id="every-position-missing"),
        pytest.param(f"sum(avg(from previous to next all: x) {TL}) by (t.product) <= 4",
                     clause(le(reduce("sum", Range("avg", previous(), next_(), X, TIMELINE, all=True),
                                      by=("product",)), 4)), MAXIMIZE_X, id="all"),
        pytest.param(f"sum(sum(from next to previous else 0: x) {TL}) by (t.product) <= 0",
                     clause(le(reduce("sum", Range("sum", next_(), previous(), X, TIMELINE, fallback=Const(0)),
                                      by=("product",)), 0)), MAXIMIZE_X, id="reversed"),
        pytest.param(f"max(max(from previous to previous: x) {TL}) by (t.product) >= 7",
                     clause(ge(reduce("max", Range("max", previous(), previous(), X, TIMELINE), by=("product",)),
                               7)), MINIMIZE_X, id="max-of-a-range"),
        pytest.param(f"x <= sum(sum(from previous to previous: t.v) {TL}) by (t.product)",
                     clause(le(X, reduce("sum", Range("sum", previous(), previous(), Col("v"), TIMELINE),
                                         by=("product",)))), MAXIMIZE_X, id="a-range-over-data"),
    ])
    def test_a_range_that_reads_nothing_drops_its_member(self, decidb_cli, scope_solver, clause_sql, body,
                                                         objective):
        """A range reads nothing where every position it requests is missing, where `all`
        meets a missing one, or where its first end comes after its last: the member it
        stands in is dropped there."""
        _judged(decidb_cli, scope_solver, _edge(clause_sql, f"{_sense(objective)} sum(x)"),
                table_rows(EDGE, EDGE_ROWS), [Decision("x")], [CAP, body], objective)

    @pytest.mark.parametrize("clause_sql, body", [
        pytest.param(f"sum(per t.product: max(at(previous: x) {TL}) by (t.product)) <= 8",
                     le(reduce("sum", reduce("max", PREV_X, by=("product",)), per=("product",)), 8),
                     id="nested-max"),
        pytest.param(f"per t.product: sum(per t.period: sum(at(previous: x) {TL}) by (t.product, t.period)) "
                     "by (t.product) <= 6",
                     le(reduce("sum", reduce("sum", PREV_X, by=("product", "period")), per=("period",),
                               by=("product",)), 6), id="nested-sum"),
        pytest.param(f"x <= avg(per t.product: min(at(next: x) {TL}) by (t.product)) + 2",
                     le(X, reduce("avg", reduce("min", NEXT_X, by=("product",)), per=("product",)) + 2),
                     id="average-of-nested-minimums"),
    ])
    def test_an_absent_nested_reducer_drops_the_member_reading_it(self, decidb_cli, scope_solver, clause_sql, body):
        """C's inner reducer reads nothing, so the outer one drops the member it would be;
        the outer one is present wherever another product's inner one is."""
        per = ("product",) if clause_sql.startswith("per t.product") else ()
        scoped = clause(body, per=per) if per else clause(body)
        _judged(decidb_cli, scope_solver, _edge(clause_sql), table_rows(EDGE, EDGE_ROWS), [Decision("x")],
                [CAP, scoped], MAXIMIZE_X)

    def test_a_helper_holds_each_group(self, decidb_cli, tmp_path):
        """Deck example 3 over five rows of two products: a helper decision per product,
        each read by its product's bound alone, so inlined (phase 7): the bound sums the
        members the frame keeps, beside the five rows' decisions (their caps are a column
        box), and nothing else -- no row for a product whose every member is dropped (C,
        one period), whose helper is absent."""
        rows = EDGE_ROWS[:5]
        sql = _edge(f"per t.product: sum(at(previous: x) {TL}) by (t.product) <= 4", rows=rows)
        counts = dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))
        assert counts == {"num_vars": 5, "num_rows": 2}, counts
        sql = _edge(f"per t.product: sum(at(previous: x) {TL}) by (t.product) <= 4", rows=EDGE_ROWS)
        counts = dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))
        assert counts["num_rows"] == 2, counts


# ---------------------------------------------------------------------------
# A frame under ABS or norm inside a reducer
# ---------------------------------------------------------------------------


SINGLE = ("id", "period", "v", "w")
SINGLE_ROWS = ((1, 1, 4, 2), (2, 2, 6, None), (3, 3, 1, 3), (4, 4, 5, 1))
LINE = Over("period")
#: `x` within one of each period's value: the clauses of a smoothing objective's case.
BAND = "band"


def _single(clause_sql, objective="MAXIMIZE sum(x)", rows=SINGLE_ROWS):
    return f"""SELECT id, x {table('t', SINGLE, rows)} DECIDE x(INT)
        SUCH THAT x <= 9 AND {clause_sql} {objective} ORDER BY id"""


@pytest.mark.correctness
class TestUnderAbs:
    @BOTH_SOLVERS
    @pytest.mark.parametrize("clause_sql, objective_sql, body, objective", [
        pytest.param("sum(abs(x - at(previous: x) over (t.period))) <= 3", "MAXIMIZE sum(x)",
                     le(reduce("sum", Abs(X - At(previous(), X, LINE))), 3), MAXIMIZE_X, id="sum-abs-bound"),
        pytest.param("x >= t.v AND x <= t.v + 1", "MINIMIZE sum(abs(x - at(previous: x) over (t.period)))",
                     BAND, Objective(ObjSense.MINIMIZE, reduce("sum", Abs(X - At(previous(), X, LINE)))),
                     id="smoothing-objective"),
        pytest.param("norm(x - at(previous: x) over (t.period), 1) <= 4", "MAXIMIZE sum(x)",
                     le(reduce("sum", Abs(X - At(previous(), X, LINE))), 4), MAXIMIZE_X, id="norm-1"),
        pytest.param("x >= t.v AND x <= t.v + 1", "MINIMIZE norm(x - at(previous: x) over (t.period), 1)",
                     BAND, Objective(ObjSense.MINIMIZE, reduce("sum", Abs(X - At(previous(), X, LINE)))),
                     id="norm-1-objective"),
        pytest.param("norm(x - at(next: x) over (t.period), 'inf') <= 2", "MAXIMIZE sum(x)",
                     le(reduce("max", Abs(X - At(next_(), X, LINE))), 2), MAXIMIZE_X, id="norm-inf"),
        pytest.param("x >= t.v AND x <= t.v + 1", "MINIMIZE norm(x - at(next: x) over (t.period), 'inf')",
                     BAND, Objective(ObjSense.MINIMIZE, reduce("max", Abs(X - At(next_(), X, LINE)))),
                     id="norm-inf-objective"),
        pytest.param("sum(abs(x - at(previous: t.v) over (t.period))) <= 5", "MAXIMIZE sum(x)",
                     le(reduce("sum", Abs(X - At(previous(), Col("v"), LINE))), 5), MAXIMIZE_X,
                     id="a-data-frame-under-abs"),
        pytest.param("sum(abs(x - at(previous: x) over (t.period))) >= 6", "MINIMIZE sum(x)",
                     ge(reduce("sum", Abs(X - At(previous(), X, LINE))), 6), MINIMIZE_X, id="sum-abs-pushed-up"),
        pytest.param("max(abs(x - at(previous: x) over (t.period))) <= 2", "MAXIMIZE sum(x)",
                     le(reduce("max", Abs(X - At(previous(), X, LINE))), 2), MAXIMIZE_X, id="max-abs"),
        pytest.param("sum(when t.w is not null: abs(x - at(next: x) over (t.period))) <= 2", "MAXIMIZE sum(x)",
                     le(reduce("sum", Abs(X - At(next_(), X, LINE)), when=lambda r: r["w"] is not None), 2),
                     MAXIMIZE_X, id="abs-under-its-own-when"),
    ])
    def test_the_member_and_the_rows_defining_it_go_together(self, request, scope_solver, cli_fixture, clause_sql,
                                                             objective_sql, body, objective):
        """The first period has no previous one, so `abs(x1 - x0)` is no member: the ABS
        auxiliary standing for it is defined nowhere and read by nothing, where with `else
        0` it would be `abs(x1)`."""
        bodies = [ge(X, Col("v")), le(X, Col("v") + 1)] if body == BAND else [body]
        _judged(request.getfixturevalue(cli_fixture), scope_solver, _single(clause_sql, objective_sql),
                table_rows(SINGLE, SINGLE_ROWS), [Decision("x")], [CAP] + [clause(b) for b in bodies], objective)

    def test_an_l0_norm_counts_the_members_left(self, decidb_cli):
        """`norm(e, 0)` counts the members where `e` is not 0, and the first period is no
        member: with caps 4, 6, 1, 5 and at most one change, the third period's cap of 1
        forces the change into the second or third member -- 4, 4, 1, 1 is 10, beside
        1, 1, 1, 5 at 8. Were the first period a member `x1 - 0`, it would be a change of
        its own, and 1, 1, 1, 1 the best at 4."""
        sql = _single("x <= t.v AND norm(x - at(previous: x) over (t.period), 0) <= 1")
        result = decidb_cli.execute_raw(sql)
        assert result.returncode == 0 and not result.stderr.strip(), result.stderr
        assert [int(row["x"]) for row in answer(decidb_cli, sql)] == [4, 4, 1, 1]

    @pytest.mark.error
    @pytest.mark.parametrize("clause_sql, message", [
        pytest.param(f"sum(abs(x - max(at(previous: x) {TL}) by (t.product))) <= 9",
                     f"DECIDE: max(at(previous: x) {TL}) by (t.product) under abs inside "
                     f"sum(abs(x - max(at(previous: x) {TL}) by (t.product))) may read no member, as "
                     f"at(previous: x) {TL} may select no position, which is not supported yet; give it an else, or "
                     "make its timeline cyclic", id="an-absent-reducer-under-abs-in-a-reducer"),
        pytest.param(f"sum(power(x, 2) + at(previous: t.v) {TL}) <= 90",
                     f"DECIDE: 'sum(power(x, 2) + at(previous: t.v) {TL})' squares or multiplies decisions in a "
                     f"member at(previous: t.v) {TL} may drop, which is not supported yet; give it an else, or make "
                     "its timeline cyclic", id="a-square-beside-the-frame"),
    ])
    def test_what_stays_refused(self, decidb_cli, clause_sql, message):
        """An ABS auxiliary's member can be dropped only by a frame its rows read; a reducer
        under it that may be absent would have to drop it as a decision. A helper's
        definition is linear, so a square beside a member frame has none."""
        result = decidb_cli.execute_raw(_edge(clause_sql))
        assert message in result.stderr, result.stderr
        assert_no_internal_leak(result, clause_sql)


# ---------------------------------------------------------------------------
# The objective
# ---------------------------------------------------------------------------


@pytest.mark.correctness
class TestObjective:
    @BOTH_SOLVERS
    @pytest.mark.parametrize("clause_sql, objective_sql, extra, objective", [
        pytest.param("x <= t.v", f"MAXIMIZE sum(at(previous: x) {TL})", le(X, Col("v")),
                     Objective(ObjSense.MAXIMIZE, reduce("sum", PREV_X)), id="sum"),
        pytest.param("x >= t.w", f"MINIMIZE avg(at(next: x) {TL})", ge(X, Col("w")),
                     Objective(ObjSense.MINIMIZE, reduce("avg", NEXT_X)), id="avg"),
        pytest.param("x <= t.v", f"MAXIMIZE min(at(previous: x) {TL})", le(X, Col("v")),
                     Objective(ObjSense.MAXIMIZE, reduce("min", PREV_X)), id="min"),
        pytest.param("x <= t.v", f"MAXIMIZE sum(per t.product: min(at(previous: x) {TL}) by (t.product))",
                     le(X, Col("v")),
                     Objective(ObjSense.MAXIMIZE, reduce("sum", reduce("min", PREV_X, by=("product",)),
                                                         per=("product",))), id="nested"),
        pytest.param("x >= t.w", f"MINIMIZE max(at(first: x) {TL}) + sum(x)", ge(X, Col("w")),
                     Objective(ObjSense.MINIMIZE, reduce("max", At(FIRST, X, TIMELINE)) + reduce("sum", X)),
                     id="composed-max-with-a-frame"),
        pytest.param("x <= t.v", f"MAXIMIZE min(at(last: x) {TL}) + sum(x)", le(X, Col("v")),
                     Objective(ObjSense.MAXIMIZE, reduce("min", At(LAST, X, TIMELINE)) + reduce("sum", X)),
                     id="composed-min-with-a-frame"),
    ])
    def test_the_objective_reads_the_members_left(self, request, scope_solver, cli_fixture, clause_sql,
                                                  objective_sql, extra, objective):
        _judged(request.getfixturevalue(cli_fixture), scope_solver, _edge(clause_sql, objective_sql),
                table_rows(EDGE, EDGE_ROWS), [Decision("x")], [CAP, clause(extra)], objective)

    @pytest.mark.error
    def test_an_objective_reducer_left_with_no_member_is_an_error(self, decidb_cli):
        """Every product has one period: the objective's sum reads nothing, and the
        objective, produced once, cannot be skipped (§3.6)."""
        rows = ((1, "A", 1, 4, 2), (2, "B", 1, 6, 1))
        result = decidb_cli.execute_raw(_edge("x <= t.v", f"MAXIMIZE sum(at(previous: x) {TL})", rows=rows))
        assert (f"DECIDE objective: at(previous: x) {TL} selects no position on any member of "
                f"sum(at(previous: x) {TL}); the objective is produced once and cannot be skipped; give it an else, "
                "or make its timeline cyclic.") in result.stderr, result.stderr
        assert ScopeError.PHRASES["objective_absent"] in result.stderr
        model = AnrModel(table_rows(EDGE, rows), [Decision("x")])
        with pytest.raises(ScopeError) as error:
            model.objective_form(Objective(ObjSense.MAXIMIZE, reduce("sum", PREV_X)))
        assert error.value.kind == "objective_absent"


# ---------------------------------------------------------------------------
# Composition (§6.7)
# ---------------------------------------------------------------------------


@pytest.mark.correctness
class TestComposition:
    @BOTH_SOLVERS
    @pytest.mark.parametrize("clause_sql, body, objective", [
        pytest.param(f"sum(per t.product: max(at(previous else 0: x) {TL}) by (t.product)) <= 10",
                     le(reduce("sum", reduce("max", At(previous(), X, TIMELINE, fallback=Const(0)), by=("product",)),
                               per=("product",)), 10), MAXIMIZE_X, id="a-frame-inside-a-nested-operand"),
        pytest.param(f"at(previous else 0: x) {TL} + sum(per t.product: max(x) by (t.product)) <= 14",
                     le(At(previous(), X, TIMELINE, fallback=Const(0)) +
                        reduce("sum", reduce("max", X, by=("product",)), per=("product",)), 14), MAXIMIZE_X,
                     id="a-nested-reducer-beside-a-frame"),
        pytest.param(f"abs(x - sum(from previous to next: x) {TL}) <= 3",
                     le(Abs(X - Range("sum", previous(), next_(), X, TIMELINE)), 3), MAXIMIZE_X,
                     id="abs-over-a-range-sum"),
        pytest.param(f"norm(x - at(previous else 0: x) {TL}, 1) <= 8",
                     le(reduce("sum", Abs(X - At(previous(), X, TIMELINE, fallback=Const(0)))), 8), MAXIMIZE_X,
                     id="norm-under-a-bound"),
        pytest.param(f"max(at(first: x) {TL}) + sum(x) <= 20",
                     le(reduce("max", At(FIRST, X, TIMELINE)) + reduce("sum", X), 20), MAXIMIZE_X,
                     id="a-frame-in-a-composed-max"),
        pytest.param(f"min(at(last: x) {TL}) - sum(x) >= -25",
                     ge(reduce("min", At(LAST, X, TIMELINE)) - reduce("sum", X), -25), MAXIMIZE_X,
                     id="a-frame-in-a-composed-min"),
        pytest.param(f"max(at(previous else 9: x) {TL}) + sum(x) <= 20",
                     le(reduce("max", At(previous(), X, TIMELINE, fallback=Const(9))) + reduce("sum", X), 20),
                     MAXIMIZE_X, id="a-fallback-in-a-composed-max"),
    ])
    def test_frames_compose(self, request, scope_solver, cli_fixture, clause_sql, body, objective):
        _judged(request.getfixturevalue(cli_fixture), scope_solver, _edge(clause_sql), table_rows(EDGE, EDGE_ROWS),
                [Decision("x")], [CAP, clause(body)], objective)

    def test_frames_under_per_with_a_by_reducer(self, decidb_cli, scope_solver):
        """`per t.product` reads one timeline per class (D69): its `last` is one value there."""
        body = le(reduce("sum", X, by=("product",)), At(LAST, Col("v"), TIMELINE) + 8)
        _judged(decidb_cli, scope_solver, _edge(f"per t.product: sum(x) by (t.product) <= at(last: t.v) {TL} + 8"),
                table_rows(EDGE, EDGE_ROWS), [Decision("x")], [CAP, clause(body, per=("product",))], MAXIMIZE_X)

    def test_variants_and_frames(self, decidb_cli, scope_solver):
        """F3's depots, each shipment with its period in its depot: per depot, one instance
        per region and position its rows read (D64, D76), each bounding the region's
        shipments and the depot's previous shipment; a depot's first period has none."""
        columns = F3_COLUMNS + ("period",)
        data = ((F3_DATA[0] + (1,)), (F3_DATA[1] + (2,)), (F3_DATA[2] + (3,)), (F3_DATA[3] + (4,)),
                (F3_DATA[4] + (1,)), (F3_DATA[5] + (2,)))
        body = le(reduce("sum", SHIP, by=("region",)) + At(previous(), SHIP, Over("period", within=("depotID",))),
                  Col("lim"))
        sql = f"""SELECT shipment, ship {table('P', columns, data)} DECIDE ship(INT)
            SUCH THAT ship <= 40 AND per P.depotID: sum(ship) by (P.region)
                + at(previous: ship) over (P.period within P.depotID) <= P.lim
            MAXIMIZE sum(ship) ORDER BY shipment"""
        _judged(decidb_cli, scope_solver, sql, table_rows(columns, data), [Decision("ship")],
                [clause(le(SHIP, 40)), clause(body, per=("depotID",))],
                Objective(ObjSense.MAXIMIZE, reduce("sum", SHIP)))

    def test_a_frame_over_duplicate_joined_rows(self, decidb_cli, scope_solver):
        """Each product's period joins two warehouses, so every position holds two rows; a
        decision keyed by the product's period reads one instance there, and a frame over
        it one linear form (D67)."""
        periods = (("A", 1, 4), ("A", 2, 6), ("A", 3, 1), ("B", 1, 7), ("B", 2, 3))
        warehouses = (("A", "w1"), ("A", "w2"), ("B", "w1"), ("B", "w3"))
        sql = f"""SELECT T.product, T.period, W.warehouse, x
            FROM {values(periods, 'T', ('product', 'period', 'v'))} JOIN
                 {values(warehouses, 'W', ('product', 'warehouse'))} USING (product)
            DECIDE per T: x(INT)
            SUCH THAT x <= T.v + 3 AND at(previous: x) over (T.period within T.product) <= x
            MAXIMIZE sum(T: x) ORDER BY T.product, T.period, W.warehouse"""
        joined = [{"product": p, "period": t, "v": v, "warehouse": w}
                  for p, t, v in periods for q, w in warehouses if q == p]
        joined.sort(key=lambda r: (r["product"], r["period"], r["warehouse"]))
        decision = Decision("x", key=("product", "period", "v"))
        clauses = [clause(le(X, Col("v") + 3)),
                   clause(le(At(previous(), X, Over("period", within=("product",))), X))]
        objective = Objective(ObjSense.MAXIMIZE, reduce("sum", X, per=("product", "period", "v")))
        _judged(decidb_cli, scope_solver, sql, joined, [decision], clauses, objective)


# ---------------------------------------------------------------------------
# The two defects phase 6 found
# ---------------------------------------------------------------------------


@pytest.mark.correctness
class TestDefects:
    @BOTH_SOLVERS
    @pytest.mark.parametrize("objective", [MAXIMIZE_X, MINIMIZE_X], ids=["maximize", "minimize"])
    def test_a_skipped_instance_keeps_a_reducers_members(self, request, scope_solver, cli_fixture, objective):
        """`sum(abs(x - 3)) + at(previous: x) <= 4`: the first period is no instance, but
        `abs(x1 - 3)` is still a member of the sum the other instances read. Phase 4 skipped
        the rows defining it with the instance, and answered x = (4, 3, 3)."""
        rows = ((1, 1, 4, 2), (2, 2, 6, 1), (3, 3, 1, 3))
        body = le(reduce("sum", Abs(X - 3)) + At(previous(), X, LINE), 4)
        sql = _single("x >= 0 AND sum(abs(x - 3)) + at(previous: x) over (t.period) <= 4",
                      f"{_sense(objective)} sum(x)", rows=rows)
        _judged(request.getfixturevalue(cli_fixture), scope_solver, sql, table_rows(SINGLE, rows), [Decision("x")],
                [CAP, clause(body)], objective)

    def test_a_skipped_instance_keeps_a_products_rows(self, decidb_cli):
        """The same for a Boolean product inside the sum: its rows define each member.
        With b fixed to 1, sum(b * x) is sum(x), and the instances of periods 2 and 3 hold
        sum(x) + x1 <= 4 and sum(x) + x2 <= 4: the best is 0, 0, 4."""
        rows = ((1, 1, 4, 2), (2, 2, 6, 1), (3, 3, 1, 3))
        sql = f"""SELECT id, x, b {table('t', SINGLE, rows)} DECIDE x(INT), b(BOOL)
            SUCH THAT x <= 9 AND b >= 1 AND sum(b * x) + at(previous: x) over (t.period) <= 4
            MAXIMIZE sum(x) ORDER BY id"""
        result = decidb_cli.execute_raw(sql)
        assert result.returncode == 0 and not result.stderr.strip(), result.stderr
        assert [int(row["x"]) for row in answer(decidb_cli, sql)] == [0, 0, 4]

    @BOTH_SOLVERS
    @pytest.mark.parametrize("clause_sql, body, objective", [
        pytest.param("sum(abs(x - at(last: x) over (t.period))) >= 3",
                     ge(reduce("sum", Abs(X - At(LAST, X, LINE))), 3), MINIMIZE_X, id="a-read-of-the-row-itself"),
        pytest.param("abs(x - at(previous else 50: x) over (t.period)) >= 1",
                     ge(Abs(X - At(previous(), X, LINE, fallback=Const(50))), 1), MAXIMIZE_X,
                     id="an-else-beyond-the-box"),
        pytest.param("min(abs(x - at(previous else 20: x) over (t.period))) >= 2",
                     ge(reduce("min", Abs(X - At(previous(), X, LINE, fallback=Const(20)))), 2), MAXIMIZE_X,
                     id="an-else-under-a-pinned-min"),
    ])
    def test_an_exact_abs_reads_its_argument_whole(self, request, scope_solver, cli_fixture, clause_sql, body,
                                                   objective):
        """An ABS pinned exactly (pushed up, or under a MIN pushed down) states its argument
        as a row and its auxiliary's Big-M off its reach. The last period's `x - at(last: x)`
        names one column twice, which Gurobi refused as a row; and an `else` read as data is
        a term of no decision, which the native row and the Big-M left out, so `|x1 - 50|`
        was read as `|x1|` and the query called infeasible (phase 4)."""
        rows = ((1, 1, 4, 2), (2, 2, 6, 1), (3, 3, 1, 3))
        _judged(request.getfixturevalue(cli_fixture), scope_solver,
                _single(clause_sql, f"{_sense(objective)} sum(x)", rows=rows), table_rows(SINGLE, rows),
                [Decision("x")], [CAP, clause(body)], objective)

    @BOTH_SOLVERS
    def test_abs_inside_a_reducer_reads_only_its_rows(self, request, scope_solver, cli_fixture):
        """`sum(when t.w is not null: abs(x - t.w))` reads `w` only where it is not NULL:
        the rows standing for each `abs` are the reducer's own, not every row its clause
        keeps, where a NULL `w` raised an error."""
        body = le(reduce("sum", Abs(X - Col("w")), when=lambda r: r["w"] is not None), 3)
        sql = _single("sum(when t.w is not null: abs(x - t.w)) <= 3")
        _judged(request.getfixturevalue(cli_fixture), scope_solver, sql, table_rows(SINGLE, SINGLE_ROWS),
                [Decision("x")], [CAP, clause(body)], MAXIMIZE_X)


@pytest.mark.explain
def test_explain_names_the_helper(decidb_cli):
    result = decidb_cli.execute_raw("EXPLAIN " + _edge(f"sum(at(previous: x) {TL}) by (t.product) <= 4"))
    assert result.returncode == 0, result.stderr
    text = "".join(result.stdout.split()).replace("│", "")
    assert "becomesahelperdecisionpergroup" in text, result.stdout
    assert_no_internal_leak(result, "EXPLAIN of a reducer whose members a frame drops")
