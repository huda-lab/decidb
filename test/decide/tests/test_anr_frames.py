"""Frame selectors (phases 3-5 of
``context/descriptions/09_anr_language/anr_algorithm_and_frames_plan.md``, §6.6;
``syntax_reference.md`` §5.6).

A frame reads a payload at a position of the anchor row's timeline: ``at(F [else v]: e)
over (S [desc] [cyclic] [within K])`` for one position, ``agg(from F1 to F2 [every d]
[else v | all]: e) over (...)`` for several. Phase 3 parses, binds and plans every
spelling; phase 4 formulates the point frames, phase 5 the ranges. ``TestSyntax`` holds
the parse, bind and echo contract and every refusal of D74; ``TestPoint`` and
``TestRange`` run every phase-4 and phase-5 fixture of ``_anr_cases``, the deck's examples
and the DeQL comparison through DeciDB, each judged by ``_anr_oracle``, which reads no SQL.
The exact parse errors are ``test/decidb/test_frame_parser.test``'s, the round trips
``test_decidb_plan_serialization.cpp``'s.
"""

import re

import pytest

from ._anr_cases import CASES, F2_CYCLIC, F2_DESC, F2_NEXT, F2_PREVIOUS, F18_CAP_KEPT
from ._anr_oracle import (
    FIRST,
    GLOBAL,
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
    eq,
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
from ._scope_cases import answer, dump_counts, scope_solver, table, table_rows, values  # noqa: F401 -- fixture
from solver.types import ObjSense, SolverStatus, VarType

T = table("t", ("id", "product", "period", "demand", "price"),
          ((1, "A", 1, 5, 2.5), (2, "A", 2, 6, 1.5), (3, "B", 1, 7, 0.5)))


def _query(clause: str, objective: str = "MAXIMIZE sum(x)") -> str:
    return f"SELECT id, x {T} DECIDE x(REAL), per (): s(REAL) SUCH THAT x <= 9 AND {clause} {objective}"


_BOX = "│┌┐└┘├┤┬┴┼─"


def _explained(cli, sql: str) -> str:
    """EXPLAIN's text with the box's glyphs and every space removed, so a phrase the box
    wrapped mid-token still matches."""
    result = cli.execute_raw("EXPLAIN " + sql)
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    return re.sub(r"\s+", "", "".join(" " if ch in _BOX else ch for ch in result.stdout))


def _bare(text: str) -> str:
    return re.sub(r"\s+", "", text)


@pytest.mark.error
class TestSyntax:
    @pytest.mark.parametrize("clause, frame", [
        pytest.param("x >= at(previous: x) over (t.period within t.product)",
                     "at(previous: x) over (t.period within t.product)", id="point"),
        pytest.param("x <= at(first: t.demand) over (t.period desc cyclic)",
                     "at(first: t.demand) over (t.period desc cyclic)", id="data-payload"),
        pytest.param("x >= at(2 next else s: x) over (t.period within t)",
                     "at(2 next else s: x) over (t.period within t)", id="query-wide-fallback"),
        pytest.param("sum(x) by (t.product) <= sum(at(last: t.demand) over (t.period within t.product))",
                     "at(last: t.demand) over (t.period within t.product)", id="inside-a-reducer"),
        pytest.param("t.price * at(previous: x) over (t.period) <= 3",
                     "at(previous: x) over (t.period)", id="data-coefficient"),
        pytest.param("x >= AT(PREVIOUS ELSE 0: x) OVER (t.period CYCLIC)",
                     "at(previous else 0: x) over (t.period cyclic)", id="upper-case-words"),
        pytest.param("x <= at(previous else 0.5: x) over (t.period)",
                     "at(previous else 0.5: x) over (t.period)", id="fractional-fallback"),
        pytest.param("x >= at(1 previous else NULL: x) over (t.period asc within ())",
                     "at(previous: x) over (t.period)", id="defaults-dropped"),
    ])
    def test_a_point_frame_echoes_as_written(self, decidb_cli, clause, frame):
        """EXPLAIN prints a frame as written, its defaults -- `else NULL` among them (D80) --
        left out."""
        assert _bare(frame) in _explained(decidb_cli, _query(clause))

    @pytest.mark.parametrize("clause, frame", [
        pytest.param("sum(from 2 previous to 1 previous every 1: x) over (t.period asc within ()) <= 4",
                     "sum(from 2 previous to previous: x) over (t.period)", id="range-defaults-dropped"),
        pytest.param("avg(from first to last all: x) over (t.period) >= 1",
                     "avg(from first to last all: x) over (t.period)", id="range-all"),
        pytest.param("x <= count(from first to last: t.demand) over (t.period)",
                     "count(from first to last: t.demand) over (t.period)", id="range-count"),
    ])
    def test_a_range_echoes_as_written(self, decidb_cli, clause, frame):
        """EXPLAIN prints a range as written, its defaults left out."""
        assert _bare(frame) in _explained(decidb_cli, _query(clause))

    def test_a_frame_in_the_objective_echoes_as_written(self, decidb_cli):
        sql = _query("x >= 0", "MINIMIZE sum(abs(x - at(previous else 0: x) over (t.period within t.product)))")
        assert _bare("at(previous else 0: x) over (t.period within t.product)") in _explained(decidb_cli, sql)

    @pytest.mark.error_binder
    @pytest.mark.parametrize("clause, message", [
        pytest.param("when at(previous: t.demand) over (t.period) > 1: x <= 3",
                     "DECIDE: a frame selector is not allowed in when, per, by, within or an order key", id="in-when"),
        pytest.param("sum(x) by (at(first: t.product) over (t.period)) <= 3",
                     "DECIDE: a frame selector is not allowed in when, per, by, within or an order key", id="in-by"),
        pytest.param("x <= at(first: t.demand) over (at(first: t.period) over (t.id))",
                     "DECIDE: a frame selector is not allowed in when, per, by, within or an order key",
                     id="in-an-order-key"),
        pytest.param("at(previous: x) over (x) <= 3",
                     "DECIDE: x is a decision; a frame's order key and within may only use data",
                     id="decision-order-key"),
        pytest.param("at(previous: x) over (t.period within x) <= 3",
                     "DECIDE: x is a decision; a frame's order key and within may only use data", id="decision-within"),
        pytest.param("at(previous: sum(x)) over (t.period) <= 3",
                     "DECIDE: a reducer inside at(...) is not supported; reduce outside the frame",
                     id="reducer-in-a-point"),
        pytest.param("sum(from first to last: max(x)) over (t.period) <= 3",
                     "DECIDE: a reducer inside sum(from ...) is not supported; reduce outside the frame",
                     id="reducer-in-a-range"),
        pytest.param("at(previous: at(next: x) over (t.period)) over (t.period) <= 3",
                     "DECIDE: a frame selector inside at(...) is not supported", id="frame-in-a-payload"),
        pytest.param("at(previous else at(next: x) over (t.period): x) over (t.period) <= 3",
                     "DECIDE: a frame selector inside at(...) is not supported", id="frame-in-a-fallback"),
        pytest.param("x * at(previous: t.demand) over (t.period) <= 3",
                     "multiplies a decision by a value from another row, which is not supported yet",
                     id="decision-times-data-frame"),
        pytest.param("sum(x * at(previous: x) over (t.period)) <= 3",
                     "multiplies a decision by a value from another row, which is not supported yet",
                     id="decision-times-decision-frame"),
        pytest.param("x / at(previous: t.demand) over (t.period) <= 3",
                     "multiplies a decision by a value from another row, which is not supported yet",
                     id="decision-over-data-frame"),
        pytest.param("power(at(previous: x) over (t.period), 2) <= 3",
                     "multiplies a decision by a value from another row, which is not supported yet",
                     id="square-of-a-frame"),
        pytest.param("abs(from first to last: x) over (t.period) <= 3",
                     "DECIDE: a range frame reduces with sum, avg, min, max or count, found abs", id="range-reducer"),
        pytest.param("at(previous else 'none': x) over (t.period) <= 3",
                     "DECIDE: in at(previous else 'none': x) over (t.period), else 'none' is not a number",
                     id="fallback-not-a-number"),
        pytest.param("x <= at(previous: x * x) over (t.period)",
                     "DECIDE: '(x * x)' squares or multiplies decisions inside at(...), which is not supported yet",
                     id="square-in-a-payload"),
        pytest.param("x >= at(previous: abs(x - 3)) over (t.period)",
                     "DECIDE: 'abs((x - 3))' inside at(...) is not linear in the decisions, which is not supported "
                     "yet; write it outside the frame", id="abs-in-a-payload"),
        pytest.param("x >= at(previous: t.price / x) over (t.period)",
                     "DECIDE: '(t.price / x)' inside at(...) is not linear in the decisions", id="decision-divisor"),
        pytest.param("x >= at(previous else s: t.demand) over (t.period)",
                     "DECIDE: in at(previous else s: t.demand) over (t.period), else s reads a decision while the "
                     "payload reads none, which is not supported yet", id="decision-fallback-over-data"),
        pytest.param("x <= at(first: t.demand) over (count(*))",
                     "DECIDE: a reducer such as 'count_star()' is not allowed in a frame's order key",
                     id="reducer-in-an-order-key"),
        pytest.param("at(previous: x) over (t.period)",
                     "'at(previous: x) over (t.period)' is not a valid SUCH THAT constraint on its own",
                     id="frame-alone"),
        pytest.param("at(previous: x) over (t.period within t.shelf) <= 3",
                     "DECIDE: t.shelf is neither a column nor a relation of the FROM clause", id="unknown-within"),
    ])
    def test_a_refusal_names_the_construct(self, decidb_cli, clause, message):
        """D74 (§3.6): a frame in a condition or a key, a decision in a frame's keys, a
        reducer or a frame in a payload, a frame term times a decision, and a payload that
        is not linear in the decisions."""
        result = decidb_cli.execute_raw(_query(clause))
        assert message in result.stderr, result.stderr
        assert_no_internal_leak(result, "a frame refusal")

    def test_an_average_range_under_not_equal_is_fractional(self, decidb_cli):
        """A range is a value of the row, not a reducer `<>` could multiply through by its
        count as it does an AVG: an AVG range under `<>` is refused as fractional, in the
        words a fractional column takes."""
        result = decidb_cli.execute_raw(
            f"SELECT id, x {T} DECIDE x(INT) SUCH THAT x <= 9 AND "
            "x + avg(from first to last: x) over (t.period) <> 3 MAXIMIZE sum(x)")
        assert ("Comparison '<>' is not supported here: 'avg(from first to last: x) over (t.period)' has type "
                "DOUBLE and can take fractional values") in result.stderr, result.stderr
        assert_no_internal_leak(result, "an AVG range under <>")

    @pytest.mark.correctness
    def test_ordinary_sql_in_a_clause_keeps_its_meaning(self, decidb_cli):
        """A subquery that starts with FROM or VALUES keeps OVER, WITHIN and a named window
        as SQL's, and a decision may be called `at`."""
        rows, _ = decidb_cli.execute(
            f"SELECT id, at {T} DECIDE at(REAL) SUCH THAT at <= (FROM (SELECT t.demand AS \"within\" {T}) u "
            "WINDOW w AS (ORDER BY u.within) QUALIFY row_number() OVER w = 1) MAXIMIZE sum(at)")
        assert sorted(rows) == [(1, 5.0), (2, 5.0), (3, 5.0)]

    @pytest.mark.correctness
    def test_a_window_in_a_subquery_of_the_clause_still_answers(self, decidb_cli):
        """The lexer reads a frame's tokens at the clause's own level only, so a SQL window
        inside a subquery written in the clause keeps its OVER."""
        rows, _ = decidb_cli.execute(_query(
            "x <= (SELECT max(r) FROM (SELECT row_number() OVER (ORDER BY v.id) AS r "
            f"{table('v', ('id',), ((1,), (2,)))}))"))
        assert sorted(rows) == [(1, 2.0), (2, 2.0), (3, 2.0)]


# ---------------------------------------------------------------------------
# Point selectors (phase 4)
# ---------------------------------------------------------------------------

POINT_CASES = [pytest.param(case, id=case.name) for case in CASES if case.phase == 4]


@pytest.mark.correctness
class TestPoint:
    @pytest.mark.parametrize("case", POINT_CASES)
    def test_the_engine_answers_each_fixture(self, decidb_cli, scope_solver, case):
        """F2, F4, F7-F9, F15, F18, F19, F21 and the laziness and partition cases: the
        hand-computed optimum or status, or the error the data raises (§6.3)."""
        result = decidb_cli.execute_raw(case.sql)
        assert_no_internal_leak(result, case.name)
        if case.error == "conversion":
            assert "Conversion Error" in result.stderr and "none" in result.stderr, result.stderr
            return
        if case.error:
            assert ScopeError.PHRASES[case.error] in result.stderr, result.stderr
            return
        if case.status == SolverStatus.INFEASIBLE:
            assert "is infeasible" in result.stderr, result.stderr
            return
        assert result.returncode == 0 and not result.stderr.strip(), result.stderr
        model = case.model()
        reached = model.judge(answer(decidb_cli, case.sql), case.clauses, case.objective)
        expected = case.optimum
        if expected is None:
            expected = model.solve(scope_solver, case.clauses, case.objective).objective
        assert reached == pytest.approx(expected, abs=1e-6)

    @pytest.mark.parametrize("case, frame_rows", [
        pytest.param(F2_PREVIOUS, 4, id="F2_previous"),
        pytest.param(F2_CYCLIC, 6, id="F2_cyclic"),
        pytest.param(F2_NEXT, 4, id="F2_next"),
        pytest.param(F2_DESC, 4, id="F2_desc"),
    ])
    def test_a_point_frame_reads_another_row_in_place(self, decidb_cli, tmp_path, case, frame_rows):
        """P9 (D72): a decision read at the selected row is that row's own column, so F2 is
        its six inventories and one row per instance -- the frame clause's four (each
        timeline's first period skipped) or, cyclic, six -- beside the six caps: no helper
        decision and no definition row."""
        assert len(case.model().instances(case.clauses[1])) == frame_rows
        counts = dump_counts(decidb_cli.dump_model(case.sql, tmp_path / "model.dump"))
        assert counts == {"num_vars": 6, "num_rows": 6 + frame_rows}


def _judged(cli, solver, sql, rows, decisions, clauses, objective):
    """DeciDB's answer, judged by the oracle against its own optimum; returns it."""
    model = AnrModel(rows, decisions)
    expected = model.solve(solver, clauses, objective)
    assert expected.status == SolverStatus.OPTIMAL, expected.status
    result = cli.execute_raw(sql)
    assert result.returncode == 0 and not result.stderr.strip(), result.stderr
    reached = model.judge(answer(cli, sql), clauses, objective)
    assert reached == pytest.approx(expected.objective, abs=1e-6)
    return reached


INVENTORY, PRODUCE, X, Y, S = Var("inventory"), Var("produce"), Var("x"), Var("y"), Var("s")
MAXIMIZE_X = Objective(ObjSense.MAXIMIZE, reduce("sum", X))

# The deck's T(productID, period, ...) joined with P(productID, ...) (pp. 44-48).
DECK_T = ("tid", "productID", "period", "dueDate", "demand", "cap")
DECK_T_ROWS = ((1, "A", 1, 3, 4, 6), (2, "A", 2, 1, 3, 2), (3, "A", 3, 2, 5, 7), (4, "B", 1, 1, 2, 5),
               (5, "B", 2, 2, 6, 4))
DECK_P = ("productID", "openingStock", "scheduleTolerance", "cyclicPlan", "maxInventoryChange")
DECK_P_ROWS = (("A", 2, 1, True, 2), ("B", 0, 0, False, 1))
DECK_FROM = f"FROM {values(DECK_T_ROWS, 'T', DECK_T)} JOIN {values(DECK_P_ROWS, 'P', DECK_P)} USING (productID)"


def _deck_rows():
    policy = {row[0]: dict(zip(DECK_P, row)) for row in DECK_P_ROWS}
    return [dict(zip(DECK_T, row)) | policy[row[1]] for row in DECK_T_ROWS]


@pytest.mark.correctness
class TestPointDeck:
    def test_example_1_previous_position(self, decidb_cli, scope_solver):
        """Deck p. 44: the inventory balance, its first period falling back to the opening
        stock, an equality per tuple."""
        sql = f"""SELECT tid, inventory, produce {DECK_FROM}
            DECIDE inventory(INT), produce(INT)
            SUCH THAT produce <= 5 AND
                inventory = at(previous else P.openingStock: inventory) over (T.period within T.productID)
                            + produce - T.demand
            MINIMIZE sum(2 * produce + inventory) ORDER BY tid"""
        balance = At(previous(), INVENTORY, Over("period", within=("productID",)), fallback=Col("openingStock"))
        clauses = [clause(le(PRODUCE, 5)), clause(eq(INVENTORY, balance + PRODUCE - Col("demand")))]
        objective = Objective(ObjSense.MINIMIZE, reduce("sum", 2 * PRODUCE + INVENTORY))
        assert _judged(decidb_cli, scope_solver, sql, _deck_rows(), [Decision("inventory"), Decision("produce")],
                       clauses, objective) == 37

    def test_example_4_different_orders(self, decidb_cli, scope_solver):
        """Deck p. 47: two frames over one product, each with its own order key."""
        sql = f"""SELECT tid, produce {DECK_FROM}
            DECIDE produce(INT)
            SUCH THAT produce <= T.cap AND
                abs(at(previous else 0: produce) over (T.period within T.productID)
                    - at(previous else 0: produce) over (T.dueDate within T.productID)) <= P.scheduleTolerance
            MAXIMIZE sum(produce) ORDER BY tid"""
        by_period = At(previous(), PRODUCE, Over("period", within=("productID",)), fallback=Const(0))
        by_due = At(previous(), PRODUCE, Over("dueDate", within=("productID",)), fallback=Const(0))
        clauses = [clause(le(PRODUCE, Col("cap"))), clause(le(Abs(by_period - by_due), Col("scheduleTolerance")))]
        _judged(decidb_cli, scope_solver, sql, _deck_rows(), [Decision("produce")], clauses,
                Objective(ObjSense.MAXIMIZE, reduce("sum", PRODUCE)))

    def test_example_5_cyclic_product_plan(self, decidb_cli, scope_solver):
        """Deck p. 48: the final period wraps to the first, for the products whose plan is
        cyclic; a `when` keeps only those rows, and their timelines are read over them."""
        sql = f"""SELECT tid, inventory {DECK_FROM}
            DECIDE inventory(INT)
            SUCH THAT inventory <= T.cap AND
                when P.cyclicPlan: abs(inventory - at(previous: inventory) over (T.period cyclic within T.productID))
                    <= P.maxInventoryChange
            MAXIMIZE sum(inventory) ORDER BY tid"""
        wrap = At(previous(), INVENTORY, Over("period", cyclic=True, within=("productID",)))
        clauses = [clause(le(INVENTORY, Col("cap"))),
                   clause(le(Abs(INVENTORY - wrap), Col("maxInventoryChange")), when=lambda r: r["cyclicPlan"])]
        _judged(decidb_cli, scope_solver, sql, _deck_rows(), [Decision("inventory")], clauses,
                Objective(ObjSense.MAXIMIZE, reduce("sum", INVENTORY)))

    def test_the_deql_comparison_as_written(self, decidb_cli, scope_solver):
        """Deck pp. 73-75: balance, ramp, opening and closing stock over a join, the decisions
        keyed per product and period, every constraint tuple-wise. The ramp's `else NULL`
        is the default written out (D80): the final period skips its ramp."""
        demand = (("A", 1, 3), ("A", 2, 5), ("A", 3, 2), ("B", 1, 4), ("B", 2, 4))
        policy_columns = ("product", "opening_stock", "safety_stock", "max_rate", "warehouse_cap", "max_ramp",
                          "hold_cost", "run_cost")
        policy = (("A", 2, 1, 6, 10, 2, 1, 3), ("B", 0, 1, 6, 10, 3, 2, 1))
        sql = f"""SELECT D.product, D.period, produce, inventory
            FROM {values(demand, 'D', ('product', 'period', 'demand'))}
            JOIN {values(policy, 'P', policy_columns)} ON P.product = D.product
            DECIDE per D.product, D.period: produce(REAL), per D.product, D.period: inventory(REAL)
            SUCH THAT produce <= P.max_rate AND inventory <= P.warehouse_cap
              AND inventory = at(previous else P.opening_stock: inventory) over (D.period within D.product)
                              + produce - D.demand
              AND at(next else NULL: produce) over (D.period within D.product) - produce <= P.max_ramp
              AND at(first else NULL: inventory) over (D.period within D.product) >= P.safety_stock
              AND at(last else NULL: inventory) over (D.period within D.product) >= P.safety_stock
            MINIMIZE sum(P.hold_cost * inventory + P.run_cost * produce)
            ORDER BY D.product, D.period"""
        by_product = {row[0]: dict(zip(policy_columns, row)) for row in policy}
        rows = [dict(zip(("product", "period", "demand"), row)) | by_product[row[0]] for row in demand]
        timeline = Over("period", within=("product",))
        key = ("product", "period")
        decisions = [Decision("produce", key, VarType.CONTINUOUS), Decision("inventory", key, VarType.CONTINUOUS)]
        clauses = [
            clause(le(PRODUCE, Col("max_rate"))),
            clause(le(INVENTORY, Col("warehouse_cap"))),
            clause(eq(INVENTORY, At(previous(), INVENTORY, timeline, fallback=Col("opening_stock")) + PRODUCE
                      - Col("demand"))),
            clause(le(At(next_(), PRODUCE, timeline) - PRODUCE, Col("max_ramp"))),
            clause(ge(At(FIRST, INVENTORY, timeline), Col("safety_stock"))),
            clause(ge(At(LAST, INVENTORY, timeline), Col("safety_stock"))),
        ]
        objective = Objective(ObjSense.MINIMIZE,
                              reduce("sum", Col("hold_cost") * INVENTORY + Col("run_cost") * PRODUCE))
        _judged(decidb_cli, scope_solver, sql, rows, decisions, clauses, objective)


EDGE = ("id", "product", "period", "v", "w")
EDGE_ROWS = ((1, "A", 1, 4, 2), (2, "A", 2, 6, 1), (3, "A", 3, 1, 3), (4, "B", 1, 7, 2), (5, "B", 2, 3, 1))


def _edge(clause_sql, decide="x(INT)", objective="MAXIMIZE sum(x)", rows=EDGE_ROWS, select="x"):
    return f"""SELECT id, {select} {table('t', EDGE, rows)} DECIDE {decide}
        SUCH THAT x <= 9 AND {clause_sql} {objective} ORDER BY id"""


TIMELINE = Over("period", within=("product",))
CAP = clause(le(X, 9))


@pytest.mark.correctness
class TestPointShapes:
    @pytest.mark.parametrize("clause_sql, body", [
        pytest.param("t.w * at(previous: x) over (t.period within t.product) <= 6",
                     le(Col("w") * At(previous(), X, TIMELINE), 6), id="data-coefficient"),
        pytest.param("x >= at(previous: 2 * x - t.v) over (t.period within t.product)",
                     ge(X, At(previous(), 2 * X - Col("v"), TIMELINE)), id="affine-payload"),
        pytest.param("x <> at(previous: x) over (t.period within t.product)",
                     ne(X, At(previous(), X, TIMELINE)), id="not-equal"),
        pytest.param("x <= at(first: t.v) over (t.period within t.product) "
                     "+ at(last: t.v) over (t.period desc within t.product)",
                     le(X, At(FIRST, Col("v"), TIMELINE) + At(LAST, Col("v"), Over("period", desc=True,
                                                                                    within=("product",)))),
                     id="two-data-frames"),
        pytest.param("x <= at(5 previous: x) over (t.period within t.product)",
                     le(X, At(previous(5), X, TIMELINE)), id="every-position-missing"),
        pytest.param("x - at(previous: x) over (t.period cyclic within t) <= 0",
                     le(X - At(previous(), X, Over("period", cyclic=True, within=("id", "product", "period", "v",
                                                                                   "w"))), 0),
                     id="cyclic-onto-itself"),
    ])
    def test_a_root_frame(self, decidb_cli, scope_solver, clause_sql, body):
        """A frame outside every reducer, judged by the oracle: a data factor on it, an
        affine payload read at the selected row, `<>`, data frames on the bound side, a
        position no anchor has (every instance skipped, no error), and a cyclic timeline of
        one row, where the frame reads the anchor's own column."""
        _judged(decidb_cli, scope_solver, _edge(clause_sql), table_rows(EDGE, EDGE_ROWS), [Decision("x")],
                [CAP, clause(body)], MAXIMIZE_X)

    @pytest.mark.parametrize("clause_sql, body, objective", [
        pytest.param("sum(at(previous else 0: x) over (t.period within t.product)) <= 5",
                     le(reduce("sum", At(previous(), X, TIMELINE, fallback=Const(0))), 5), MAXIMIZE_X, id="sum"),
        pytest.param("avg(at(first: x) over (t.period within t.product)) by (t.product) <= 2",
                     le(reduce("avg", At(FIRST, X, TIMELINE), by=("product",)), 2), MAXIMIZE_X, id="avg-by"),
        pytest.param("max(at(previous else 0: x) over (t.period within t.product)) <= 4",
                     le(reduce("max", At(previous(), X, TIMELINE, fallback=Const(0))), 4), MAXIMIZE_X,
                     id="max-easy"),
        pytest.param("max(at(previous else 0: x) over (t.period within t.product)) >= 8",
                     ge(reduce("max", At(previous(), X, TIMELINE, fallback=Const(0))), 8),
                     Objective(ObjSense.MINIMIZE, reduce("sum", X)), id="max-hard"),
        pytest.param("x <= at(previous else 0: x) over (t.period within t.product) + 0.5 * sum(x)",
                     le(X, At(previous(), X, TIMELINE, fallback=Const(0)) + 0.5 * reduce("sum", X)), MAXIMIZE_X,
                     id="beside-a-reducer"),
    ])
    def test_a_frame_that_cannot_miss_inside_a_reducer(self, decidb_cli, scope_solver, clause_sql, body, objective):
        """Inside a reducer a point frame is read in place too (a choice beyond D72): each
        member reads its selected row, with no helper. One that cannot miss -- `else`,
        `first`, `last` -- is phase 4's; one that can is phase 6's (refused below)."""
        sense = "MINIMIZE" if objective.sense == ObjSense.MINIMIZE else "MAXIMIZE"
        _judged(decidb_cli, scope_solver, _edge(clause_sql, objective=f"{sense} sum(x)"),
                table_rows(EDGE, EDGE_ROWS), [Decision("x")], [CAP, clause(body)], objective)

    @pytest.mark.parametrize("clause_sql, bodies, objective", [
        pytest.param("x BETWEEN at(previous: t.v) over (t.period within t.product) AND 8",
                     [ge(X, At(previous(), Col("v"), TIMELINE)), le(X, 8)], "MINIMIZE sum(x)", id="between"),
        pytest.param("max(x) by (t.product) >= at(previous: t.v) over (t.period within t.product)",
                     [ge(reduce("max", X, by=("product",)), At(previous(), Col("v"), TIMELINE))],
                     "MINIMIZE sum(x)", id="hard-max-bound"),
        pytest.param("min(x) by (t.product) <= at(previous: t.v) over (t.period within t.product) - 4",
                     [le(reduce("min", X, by=("product",)), At(previous(), Col("v"), TIMELINE) - 4)],
                     "MAXIMIZE sum(x)", id="hard-min-bound"),
        pytest.param("x = at(previous: x) over (t.period within t.product) + 1",
                     [eq(X, At(previous(), X, TIMELINE) + 1)], "MAXIMIZE sum(x)", id="equality-chain"),
    ])
    def test_an_absent_bound_skips_its_comparison_only(self, decidb_cli, scope_solver, clause_sql, bodies,
                                                       objective):
        """A `BETWEEN` is its two comparisons (syntax_reference.md §3), so a frame absent in
        one bound skips that half alone; under the tightest-bound collapse an absent anchor
        bounds nothing and a group with none present has no row (D79); and an equality
        chain is skipped at each timeline's first period."""
        sense = ObjSense.MINIMIZE if objective.startswith("MINIMIZE") else ObjSense.MAXIMIZE
        _judged(decidb_cli, scope_solver, _edge(clause_sql, objective=objective), table_rows(EDGE, EDGE_ROWS),
                [Decision("x")], [CAP] + [clause(body) for body in bodies], Objective(sense, reduce("sum", X)))

    def test_a_bound_every_instance_skips_is_no_column_bound(self, decidb_cli, scope_solver):
        """`abs(x - at(previous: x) over (...)) <= -1` can never hold where the frame selects a
        row, but with one period per timeline it selects none, so the clause has no
        instance and the query is feasible: its bound on the ABS auxiliary holds only where
        the frame selects, as under a `when`, and is no column bound (absorption)."""
        rows = ((1, "A", 1, 4, 2), (2, "B", 1, 6, 1))
        sql = _edge("abs(x - at(previous: x) over (t.period within t.product)) <= -1", rows=rows)
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, rows), [Decision("x")],
                [CAP, clause(le(Abs(X - At(previous(), X, TIMELINE)), -1))], MAXIMIZE_X)
        rows = rows + ((3, "A", 2, 5, 1),)
        result = decidb_cli.execute_raw(_edge("abs(x - at(previous: x) over (t.period within t.product)) <= -1",
                                              rows=rows))
        assert "is infeasible" in result.stderr, result.stderr

    def test_a_query_wide_fallback_decision(self, decidb_cli, scope_solver):
        """`else s` with s query-wide: each timeline's first period reads s, the others the
        previous x."""
        sql = _edge("x >= t.v AND s >= 3 AND y >= at(previous else s: x) over (t.period within t.product)",
                    decide="x(INT), y(INT), per (): s(INT)", objective="MINIMIZE sum(y) + 2 * s", select="x, y, s")
        clauses = [CAP, clause(ge(X, Col("v"))),
                   clause(ge(Y, At(previous(), X, TIMELINE, fallback=S))), clause(ge(S, 3))]
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, EDGE_ROWS),
                [Decision("x"), Decision("y"), Decision("s", GLOBAL)], clauses,
                Objective(ObjSense.MINIMIZE, reduce("sum", Y) + 2 * S))

    def test_first_under_per_the_timeline_is_one_instance_per_class(self, decidb_cli, tmp_path, scope_solver):
        """`per t.product` holds `within t.product` (D69), and `first` reads one value per
        timeline, so over a decision keyed per product the clause is one instance per
        product: two rows."""
        sql = _edge("per t.product: x >= at(first: t.v) over (t.period within t.product)",
                    decide="per t.product: x(INT)", objective="MINIMIZE sum(x)")
        body = clause(ge(X, At(FIRST, Col("v"), TIMELINE)), per=("product",))
        decisions = [Decision("x", ("product",))]
        model = AnrModel(table_rows(EDGE, EDGE_ROWS), decisions)
        assert len(model.instances(body)) == 2
        assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == 2
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, EDGE_ROWS), decisions, [CAP, body],
                Objective(ObjSense.MINIMIZE, reduce("sum", X)))

    def test_previous_under_per_is_one_variant_per_position(self, decidb_cli, tmp_path, scope_solver):
        """`previous` reads relative to the anchor, so under `per t.product` the anchor's
        order key is a field of the signature (D76): one instance per period with a
        previous one -- three rows over the two products -- though x is keyed per product."""
        sql = _edge("per t.product: x <= at(previous: t.v) over (t.period within t.product)",
                    decide="per t.product: x(INT)")
        body = clause(le(X, At(previous(), Col("v"), TIMELINE)), per=("product",))
        model = AnrModel(table_rows(EDGE, EDGE_ROWS), [Decision("x", ("product",))])
        assert len(model.instances(body)) == 3
        assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == 3
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, EDGE_ROWS), [Decision("x", ("product",))],
                [CAP, body], MAXIMIZE_X)

    def test_a_reached_else_under_per_is_a_field(self, decidb_cli, tmp_path, scope_solver):
        """D76: what the `else` reads where the anchor reaches it splits the class. Product A's
        first period holds two rows with no previous one, reading their own `t.w`, 5 and 2:
        two instances, so 2 binds -- not the first row's 5."""
        rows = ((1, "A", 1, 4, 5), (2, "A", 1, 4, 2), (3, "A", 2, 6, 1), (4, "B", 1, 7, 3))
        sql = _edge("per t.product: x <= at(previous else t.w: t.v) over (t.period within t.product)",
                    decide="per t.product: x(INT)", rows=rows)
        body = clause(le(X, At(previous(), Col("v"), TIMELINE, fallback=Col("w"))), per=("product",))
        decisions = [Decision("x", ("product",))]
        model = AnrModel(table_rows(EDGE, rows), decisions)
        assert len(model.instances(body)) == 4
        assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == 4
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, rows), decisions, [CAP, body], MAXIMIZE_X)

    @pytest.mark.parametrize("clause_sql, expected", [
        pytest.param("x <= datediff('day', DATE '2020-01-01', at(first: t.d) over (t.id within t.g))", [4, 4, 1],
                     id="date"),
        pytest.param("x <= datediff('day', DATE '2020-01-01', at(previous: t.d) over (t.id within t.g))",
                     [9, 4, 9], id="date-missing"),
        pytest.param("x <= at(previous else true: t.f) over (t.id within t.g)", [1, 0, 1], id="boolean-else"),
    ])
    def test_a_data_frame_reads_its_own_type(self, decidb_cli, clause_sql, expected):
        """A frame over data reads one value of its payload's type, a DATE or a BOOLEAN as much
        as a number; no arithmetic joins its payload to its `else`."""
        sql = f"""SELECT t.id, x FROM (VALUES (1, 'a', DATE '2020-01-05', false), (2, 'a', DATE '2020-01-09', true),
                  (3, 'b', DATE '2020-01-02', true)) t(id, g, d, f)
            DECIDE x(INT) SUCH THAT x <= 9 AND {clause_sql} MAXIMIZE sum(x) ORDER BY t.id"""
        assert [int(row["x"]) for row in answer(decidb_cli, sql)] == expected

    def test_a_decision_frame_in_an_l0_norm(self, decidb_cli):
        """`norm(e, 0)` counts the rows where `e` is not 0; its Big-M reads a frame read's
        base. One change is allowed: x holds 3 through product a, and b stays at 0."""
        sql = """SELECT t.id, x FROM (VALUES (1, 'a', 3), (2, 'a', 4), (3, 'b', 2)) t(id, g, c)
            DECIDE x(INT) SUCH THAT x <= t.c AND norm(x - at(previous else 0: x) over (t.id within t.g), 0) <= 1
            MAXIMIZE sum(x) ORDER BY t.id"""
        assert [int(row["x"]) for row in answer(decidb_cli, sql)] == [3, 3, 0]

    def test_peers_with_one_payload_select_once(self, decidb_cli, scope_solver):
        """D67: a selected position of two rows is no error when both read one payload -- the
        same data, the same instance of a keyed decision."""
        rows = ((1, "A", 1, 4, 2), (2, "A", 1, 4, 2), (3, "A", 2, 6, 1))
        sql = f"""SELECT id, x, y {table('t', EDGE, rows)}
            DECIDE x(INT), per t.product, t.period: y(INT)
            SUCH THAT x <= 9 AND y <= 9 AND x <= at(previous: t.v) over (t.period within t.product)
                AND x <= at(previous: y) over (t.period within t.product)
            MAXIMIZE sum(x) + sum(y) ORDER BY id"""
        decisions = [Decision("x"), Decision("y", ("product", "period"))]
        clauses = [CAP, clause(le(Y, 9)), clause(le(X, At(previous(), Col("v"), TIMELINE))),
                   clause(le(X, At(previous(), Y, TIMELINE)))]
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, rows), decisions, clauses,
                Objective(ObjSense.MAXIMIZE, reduce("sum", X) + reduce("sum", Y)))

    @pytest.mark.parametrize("clause_sql, body, rows", [
        pytest.param("x <= at(previous: t.w * x) over (t.period within t.product) + 3",
                     le(X, At(previous(), Col("w") * X, TIMELINE) + 3),
                     ((1, "A", 1, 4, 0), (2, "A", 1, 5, 0), (3, "A", 2, 6, 1)), id="zero-coefficient"),
        pytest.param("x <= at(previous: s + t.v + t.w) over (t.period within t.product)",
                     le(X, At(previous(), S + Col("v") + Col("w"), TIMELINE)),
                     ((1, "A", 1, 4, 1), (2, "A", 1, 3, 2), (3, "A", 2, 6, 1)), id="constants-alike"),
    ])
    def test_peers_with_one_linear_form_select_once(self, decidb_cli, scope_solver, clause_sql, body, rows):
        """D67 compares payloads as the model reads them: `0 * x` at two rows reads no
        decision, and `s + 4 + 1` is `s + 3 + 2`."""
        sql = _edge(f"s <= 2 AND {clause_sql}", decide="x(INT), per (): s(INT)", rows=rows, select="x, s")
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, rows), [Decision("x"), Decision("s", GLOBAL)],
                [CAP, clause(le(S, 2)), clause(body)], MAXIMIZE_X)

    @pytest.mark.parametrize("clause_sql", [
        pytest.param("x <= at(previous: t.w * x) over (t.period within t.product) + 3", id="coefficient-on-two-rows"),
        pytest.param("x <= at(previous: s + t.v) over (t.period within t.product)", id="two-constants"),
    ])
    def test_peers_with_two_linear_forms_are_ambiguous(self, decidb_cli, clause_sql):
        rows = ((1, "A", 1, 4, 1), (2, "A", 1, 3, 1), (3, "A", 2, 6, 1))
        result = decidb_cli.execute_raw(_edge(f"s <= 2 AND {clause_sql}", decide="x(INT), per (): s(INT)", rows=rows))
        assert "finds 2 rows at t.product = 'A', t.period = 1; a position must hold one row." in result.stderr, \
            result.stderr

    def test_the_objective_reads_one_position(self, decidb_cli, scope_solver):
        """D66: `at(first: x) over (t.period)` reads one row, so the objective is one value."""
        rows = ((1, "A", 1, 4, 2), (2, "A", 2, 6, 1), (3, "A", 3, 1, 3))
        sql = _edge("x <= t.v", objective="MAXIMIZE at(first: x) over (t.period desc)", rows=rows)
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, rows), [Decision("x")],
                [CAP, clause(le(X, Col("v")))],
                Objective(ObjSense.MAXIMIZE, At(FIRST, X, Over("period", desc=True))))


@pytest.mark.error
class TestPointErrors:
    @pytest.mark.parametrize("objective, message", [
        pytest.param("MAXIMIZE at(previous: x) over (t.period within t.product)",
                     "DECIDE objective: 'at(previous: x) over (t.period within t.product)' reads more than one "
                     "position over the kept rows (t.period = 1, 2), so the objective would be produced more than "
                     "once",
                     id="relative"),
        pytest.param("MAXIMIZE at(first: x) over (t.period within t.product)",
                     "DECIDE objective: 'at(first: x) over (t.period within t.product)' reads more than one "
                     "timeline over the kept rows (t.product = 'A', 'B'), so the objective would be produced more "
                     "than once", id="two-timelines"),
    ])
    def test_the_objective_is_produced_once(self, decidb_cli, objective, message):
        result = decidb_cli.execute_raw(_edge("x <= t.v", objective=objective))
        assert message in result.stderr, result.stderr

    def test_the_objective_cannot_skip_a_missing_position(self, decidb_cli):
        """§3.6: a frame at the objective's root that selects no position leaves it no value."""
        rows = ((1, "A", 1, 4, 2),)
        result = decidb_cli.execute_raw(_edge("x <= t.v", objective="MAXIMIZE at(previous: x) over (t.period)",
                                              rows=rows))
        assert ("DECIDE objective: at(previous: x) over (t.period) selects no position; the objective is "
                "produced once and cannot be skipped. Give the frame an else.") in result.stderr, result.stderr

    @pytest.mark.parametrize("clause_sql, message", [
        pytest.param("sum(at(previous: x) over (t.period)) <= 4",
                     "DECIDE: at(previous: x) over (t.period) may select no position inside "
                     "sum(at(previous: x) over (t.period)), a reducer over decisions, which is not supported yet; "
                     "give it an else, or make its timeline cyclic", id="in-a-constraint"),
        pytest.param("sum(x + at(previous: t.v) over (t.period)) <= 4",
                     "may select no position inside", id="data-frame-in-a-decision-reducer"),
        pytest.param("max(at(first: x) over (t.period)) + sum(x) <= 12",
                     "DECIDE: at(first: x) over (t.period) inside MAX beside another term is not supported yet",
                     id="composed-min-max"),
    ])
    def test_phase_6_shapes_are_refused(self, decidb_cli, clause_sql, message):
        """A member a missing position drops from a reducer over decisions, and a frame in a
        composed MIN/MAX, are the ANR plan's phase 6."""
        result = decidb_cli.execute_raw(_edge(clause_sql))
        assert message in result.stderr, result.stderr
        assert_no_internal_leak(result, "a phase-6 refusal")

    def test_a_missing_position_in_the_objective_reducer_is_refused(self, decidb_cli):
        result = decidb_cli.execute_raw(_edge("x <= t.v", objective="MAXIMIZE sum(at(previous: x) over (t.period))"))
        assert "may select no position inside sum(" in result.stderr, result.stderr

    def test_a_decision_frame_beside_max_in_the_objective_is_refused(self, decidb_cli):
        """A composed MIN/MAX objective reads each term at its own row: the ANR plan's phase 6."""
        result = decidb_cli.execute_raw(_edge("x <= t.v", objective="MINIMIZE max(x) - at(first: x) over (t.period)"))
        assert "DECIDE: 'at(first: x) over (t.period)' beside MIN or MAX is not supported yet" in result.stderr, \
            result.stderr
        assert_no_internal_leak(result, "a frame beside MAX")

    def test_a_present_null_payload_is_an_error(self, decidb_cli):
        """D78: a NULL read at the selected row is a NULL bound, as a NULL read at the row is."""
        rows = ((1, "A", 1, None, 2), (2, "A", 2, 6, 1))
        result = decidb_cli.execute_raw(_edge("x <= at(previous: t.v) over (t.period)", rows=rows))
        assert ScopeError.PHRASES["null_value"] in result.stderr, result.stderr
        assert_no_internal_leak(result, "a NULL payload")

    def test_a_null_no_anchor_selects_is_never_read(self, decidb_cli, scope_solver):
        """Only selected rows are read: the last period's NULL is no anchor's previous one."""
        rows = ((1, "A", 1, 4, 2), (2, "A", 2, None, 1))
        _judged(decidb_cli, scope_solver, _edge("x <= at(previous: t.v) over (t.period)", rows=rows),
                [dict(zip(EDGE, row)) for row in rows], [Decision("x")],
                [CAP, clause(le(X, At(previous(), Col("v"), Over("period"))))], MAXIMIZE_X)

    def test_diagnose_names_no_internal(self, decidb_cli):
        """F18_cap_kept is infeasible: the first period keeps its cap of 10 beside x1 >= 11,
        and its frame comparison is skipped, so DIAGNOSE finds the conflict there."""
        result = run_diagnose(decidb_cli, F18_CAP_KEPT.sql)
        assert result.returncode == 0, result.stderr
        assert diagnose_rows(result), result.stdout
        assert_no_internal_leak(result, "DIAGNOSE of a frame clause")


# ---------------------------------------------------------------------------
# Range selectors (phase 5)
# ---------------------------------------------------------------------------

RANGE_CASES = [pytest.param(case, id=case.name) for case in CASES if case.phase == 5]

#: A position as written, and as the oracle states it.
POSITIONS = [("first", FIRST), ("last", LAST), ("previous", previous()), ("2 previous", previous(2)),
             ("next", next_()), ("2 next", next_(2))]
#: `else`, `all`, and neither, as written and as the oracle's keywords.
POLICIES = [("", {}), (" else 1", {"fallback": Const(1)}), (" all", {"all": True})]


def _range_sql(reducer, start, end, every, policy, payload, cyclic=False, within="t.product"):
    step = f" every {every}" if every != 1 else ""
    over = f"t.period{' cyclic' if cyclic else ''} within {within}"
    return f"{reducer}(from {start} to {end}{step}{policy}: {payload}) over ({over})"


def _range(reducer, start, end, every, policy, payload, cyclic=False):
    return Range(reducer, start, end, payload, Over("period", cyclic=cyclic, within=("product",)), every=every,
                 **policy)


@pytest.mark.correctness
class TestRange:
    @pytest.mark.parametrize("case", RANGE_CASES)
    def test_the_engine_answers_each_fixture(self, decidb_cli, scope_solver, case):
        """F5-F7 and the range edge cases: the hand-computed optimum, or the error the data
        raises (§6.3)."""
        result = decidb_cli.execute_raw(case.sql)
        assert_no_internal_leak(result, case.name)
        if case.error:
            assert ScopeError.PHRASES[case.error] in result.stderr, result.stderr
            return
        assert result.returncode == 0 and not result.stderr.strip(), result.stderr
        model = case.model()
        reached = model.judge(answer(decidb_cli, case.sql), case.clauses, case.objective)
        expected = case.optimum
        if expected is None:
            expected = model.solve(scope_solver, case.clauses, case.objective).objective
        assert reached == pytest.approx(expected, abs=1e-6)

    @pytest.mark.parametrize("cyclic", [False, True], ids=["open", "cyclic"])
    @pytest.mark.parametrize("policy_sql, policy", POLICIES, ids=["default", "else", "all"])
    @pytest.mark.parametrize("every", [1, 2])
    @pytest.mark.parametrize("start_sql, start", POSITIONS, ids=[p[0] for p in POSITIONS])
    @pytest.mark.parametrize("end_sql, end", POSITIONS, ids=[p[0] for p in POSITIONS])
    def test_every_endpoint_pair(self, decidb_cli, scope_solver, start_sql, start, end_sql, end, every, policy_sql,
                                 policy, cyclic):
        """Every pair of ends, both steps, each policy, open and cyclic, over decisions: a
        reversed range, `all` meeting a missing position and a range of missing positions
        with no `else` read nothing and skip their instance; the rest are a helper per
        window, judged by the oracle."""
        frame = _range_sql("sum", start_sql, end_sql, every, policy_sql, "x", cyclic)
        body = le(_range("sum", start, end, every, policy, X, cyclic), 7)
        _judged(decidb_cli, scope_solver, _edge(f"{frame} <= 7"), table_rows(EDGE, EDGE_ROWS), [Decision("x")],
                [CAP, clause(body)], MAXIMIZE_X)

    @pytest.mark.parametrize("reducer", ["sum", "avg", "min", "max", "count"])
    @pytest.mark.parametrize("policy_sql, policy", POLICIES, ids=["default", "else", "all"])
    @pytest.mark.parametrize("start_sql, start, end_sql, end", [
        pytest.param("previous", previous(), "next", next_(), id="around"),
        pytest.param("first", FIRST, "last", LAST, id="timeline"),
        pytest.param("2 previous", previous(2), "previous", previous(), id="before"),
        pytest.param("next", next_(), "previous", previous(), id="reversed"),
    ])
    def test_a_range_over_data_is_a_value(self, decidb_cli, scope_solver, reducer, policy_sql, policy, start_sql,
                                          start, end_sql, end):
        """A range over data is one value per anchor, its reducer's: on the bound side, its
        instance skipped where it reads nothing (`count` reads 0 there, but under `all`)."""
        frame = _range_sql(reducer, start_sql, end_sql, 1, policy_sql, "t.v")
        body = le(X, _range(reducer, start, end, 1, policy, Col("v")))
        _judged(decidb_cli, scope_solver, _edge(f"x <= {frame}"), table_rows(EDGE, EDGE_ROWS), [Decision("x")],
                [CAP, clause(body)], MAXIMIZE_X)

    @pytest.mark.parametrize("reducer, sense, bound, objective", [
        pytest.param("max", "<=", 6, MAXIMIZE_X, id="max-easy"),
        pytest.param("max", ">=", 8, Objective(ObjSense.MINIMIZE, reduce("sum", X)), id="max-hard"),
        pytest.param("min", ">=", 2, Objective(ObjSense.MINIMIZE, reduce("sum", X)), id="min-easy"),
        pytest.param("min", "<=", 1, MAXIMIZE_X, id="min-hard"),
        pytest.param("max", "=", 5, MAXIMIZE_X, id="max-equal"),
        pytest.param("avg", ">=", 4.5, Objective(ObjSense.MINIMIZE, reduce("sum", X)), id="avg"),
        pytest.param("sum", "=", 9, MAXIMIZE_X, id="sum-equal"),
    ])
    def test_each_reducer_over_decisions(self, decidb_cli, scope_solver, reducer, sense, bound, objective):
        """A helper per window, pinned on the side its comparison pushes it: a MAX pushed
        down and a MIN pushed up by their envelope, the other way by their closing side,
        both under `=`. A missing position reads the `else`, 2, as one of the values."""
        frame = _range_sql(reducer, "previous", "next", 1, " else 2", "x")
        body = {"<=": le, ">=": ge, "=": eq}[sense](_range(reducer, previous(), next_(), 1, {"fallback": Const(2)},
                                                           X), bound)
        verb = "MINIMIZE" if objective.sense == ObjSense.MINIMIZE else "MAXIMIZE"
        _judged(decidb_cli, scope_solver, _edge(f"{frame} {sense} {bound}", objective=f"{verb} sum(x)"),
                table_rows(EDGE, EDGE_ROWS), [Decision("x")], [CAP, clause(body)], objective)

    @pytest.mark.parametrize("clause_sql, body, rows, counts", [
        pytest.param("x <= sum(from first to last: x) over (t.period within t.product) - 4",
                     le(X, _range("sum", FIRST, LAST, 1, {}, X) - 4), EDGE_ROWS,
                     {"num_vars": 5 + 2, "num_rows": 5 + 2}, id="a-helper-per-timeline"),
        pytest.param("sum(from previous to next: x) over (t.period within t.product) <= 7",
                     le(_range("sum", previous(), next_(), 1, {}, X), 7), EDGE_ROWS,
                     {"num_vars": 5 + 5, "num_rows": 5}, id="a-helper-per-position"),
        pytest.param("sum(from previous to next: x) over (t.period within t.product) <= 7",
                     le(_range("sum", previous(), next_(), 1, {}, X), 7),
                     ((1, "A", 1, 4, 2), (2, "A", 1, 6, 1), (3, "A", 2, 1, 3)),
                     {"num_vars": 3 + 2, "num_rows": 2}, id="tied-rows-share-a-position"),
    ])
    def test_a_helper_per_window(self, decidb_cli, tmp_path, scope_solver, clause_sql, body, rows, counts):
        """A range over decisions is a helper per window, defined once: one per timeline
        when neither end depends on the anchor -- n terms in one definition, not n of them
        -- and one per position otherwise, the tied rows of a position sharing it. A bound
        on the range alone is a bound on its helper's column, so it costs no row."""
        sql = _edge(clause_sql, rows=rows)
        assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump")) == counts
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, rows), [Decision("x")], [CAP, clause(body)],
                MAXIMIZE_X)

    @pytest.mark.parametrize("clause_sql, body, objective", [
        pytest.param("abs(x - avg(from previous to next: x) over (t.period within t.product)) <= 1",
                     le(Abs(X - _range("avg", previous(), next_(), 1, {}, X)), 1), "MAXIMIZE sum(x)", id="abs"),
        pytest.param("x + sum(from previous to next: x) over (t.period within t.product) <> 10",
                     ne(X + _range("sum", previous(), next_(), 1, {}, X), 10), "MAXIMIZE sum(x)",
                     id="not-equal"),
        pytest.param("sum(sum(from previous to next else 0: x) over (t.period within t.product)) by (t.product) "
                     "<= 12", le(reduce("sum", _range("sum", previous(), next_(), 1, {"fallback": Const(0)}, X),
                                        by=("product",)), 12), "MAXIMIZE sum(x)", id="inside-a-reducer"),
        pytest.param("max(min(from first to last: x) over (t.period within t.product)) >= 3",
                     ge(reduce("max", _range("min", FIRST, LAST, 1, {}, X)), 3), "MINIMIZE sum(x)",
                     id="inside-a-max"),
        pytest.param("t.w * sum(from previous to next: x) over (t.period within t.product) <= 9",
                     le(Col("w") * _range("sum", previous(), next_(), 1, {}, X), 9), "MAXIMIZE sum(x)",
                     id="data-coefficient"),
        pytest.param("x <= max(from previous to previous: x) over (t.period within t.product) + 2 "
                     "AND x <= t.v + at(previous else 0: x) over (t.period within t.product)",
                     le(X, _range("max", previous(), previous(), 1, {}, X) + 2), "MAXIMIZE sum(x)",
                     id="beside-a-point-frame"),
    ])
    def test_a_range_composes(self, decidb_cli, scope_solver, clause_sql, body, objective):
        """The helper is an ordinary keyed decision to every rewrite after it: ABS, `<>`, a
        reducer around it, a data factor, a point frame beside it."""
        sense = ObjSense.MINIMIZE if objective.startswith("MINIMIZE") else ObjSense.MAXIMIZE
        clauses = [CAP, clause(body)]
        if "AND x <= t.v +" in clause_sql:
            clauses.append(clause(le(X, Col("v") + At(previous(), X, TIMELINE, fallback=Const(0)))))
        _judged(decidb_cli, scope_solver, _edge(clause_sql, objective=objective), table_rows(EDGE, EDGE_ROWS),
                [Decision("x")], clauses, Objective(sense, reduce("sum", X)))

    @pytest.mark.parametrize("objective_sql, objective", [
        pytest.param("MAXIMIZE sum(x) - 2 * sum(from first to last: x) over (t.period)",
                     reduce("sum", X) - 2 * Range("sum", FIRST, LAST, X, Over("period")), id="beside-a-reducer"),
        pytest.param("MINIMIZE sum(max(from previous to next else 0: x) over (t.period within t.product))",
                     reduce("sum", Range("max", previous(), next_(), X, TIMELINE, fallback=Const(0))),
                     id="a-sum-of-ranges"),
        pytest.param("MAXIMIZE sum(from first to last: t.w * x) over (t.period)",
                     Range("sum", FIRST, LAST, Col("w") * X, Over("period")), id="one-timeline"),
    ])
    def test_a_range_in_the_objective(self, decidb_cli, scope_solver, objective_sql, objective):
        """A range in the objective's root reads one value when its window is the whole
        timeline (D66); a reducer over ranges reads one per anchor."""
        sense = ObjSense.MINIMIZE if objective_sql.startswith("MINIMIZE") else ObjSense.MAXIMIZE
        clauses = [CAP, clause(ge(X, Col("w")))]
        _judged(decidb_cli, scope_solver, _edge("x >= t.w", objective=objective_sql), table_rows(EDGE, EDGE_ROWS),
                [Decision("x")], clauses, Objective(sense, objective))

    def test_a_relative_range_in_the_objective_is_produced_more_than_once(self, decidb_cli):
        result = decidb_cli.execute_raw(_edge("x <= t.v", objective="MAXIMIZE sum(from previous to next: x) "
                                                                     "over (t.period within t.product)"))
        assert ScopeError.PHRASES["objective_variants"] in result.stderr, result.stderr

    def test_a_keyed_payload_and_a_decision_else(self, decidb_cli, scope_solver):
        """A payload over a decision keyed per product reads its instance at each selected
        row; an `else` over the anchor's own decision reads it once per missing position,
        so the window splits at each anchor that reaches it."""
        sql = _edge("y <= 6 AND x <= t.v AND sum(from previous to next else x: y + x) over (t.period within t.product) "
                    "<= 20", decide="x(INT), per t.product: y(INT)", objective="MAXIMIZE sum(x) + sum(y)",
                    select="x, y")
        frame = Range("sum", previous(), next_(), Y + X, TIMELINE, fallback=X)
        clauses = [CAP, clause(le(Y, 6)), clause(le(X, Col("v"))), clause(le(frame, 20))]
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, EDGE_ROWS), [Decision("x"), Decision("y", ("product",))],
                clauses, Objective(ObjSense.MAXIMIZE, reduce("sum", X) + reduce("sum", Y)))

    def test_under_per_a_range_is_one_variant_per_position(self, decidb_cli, tmp_path, scope_solver):
        """D76: a relative range under `per t.product` is a field by the anchor's position, so
        over x keyed per product the clause is one row per position -- five -- while one
        from `first` to `last` reads one value per timeline -- two."""
        decisions = [Decision("x", ("product",))]
        for clause_sql, body, rows in [
            ("per t.product: x <= sum(from previous to next: t.v) over (t.period within t.product)",
             le(X, _range("sum", previous(), next_(), 1, {}, Col("v"))), 5),
            ("per t.product: x <= max(from first to last: t.v) over (t.period within t.product)",
             le(X, _range("max", FIRST, LAST, 1, {}, Col("v"))), 2),
        ]:
            sql = _edge(clause_sql, decide="per t.product: x(INT)")
            scoped = clause(body, per=("product",))
            assert len(AnrModel(table_rows(EDGE, EDGE_ROWS), decisions).instances(scoped)) == rows
            assert dump_counts(decidb_cli.dump_model(sql, tmp_path / "model.dump"))["num_rows"] == rows
            _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, EDGE_ROWS), decisions, [CAP, scoped], MAXIMIZE_X)

    def test_an_unused_else_is_never_evaluated(self, decidb_cli, scope_solver):
        """The `else` is read at the anchors that miss a position, and only there: a cast that
        fails on every other row raises nothing (laziness, §3.4)."""
        rows = ((1, "A", 1, 4, 2), (2, "A", 2, 6, 1), (3, "A", 3, 1, 3))
        texts = ("1", "none", "none")
        sql = f"""SELECT t.id, x FROM (VALUES {', '.join(f"({r[0]}, '{r[1]}', {r[2]}, {r[3]}, '{s}')"
                                                         for r, s in zip(rows, texts))}) t(id, product, period, v, s)
            DECIDE x(INT) SUCH THAT x <= 9 AND
                x <= sum(from previous to previous else CAST(t.s AS INTEGER): t.v) over (t.period)
            MAXIMIZE sum(x) ORDER BY t.id"""
        assert [int(row["x"]) for row in answer(decidb_cli, sql)] == [1, 4, 6]

    def test_a_null_value_is_an_error_and_count_skips_it(self, decidb_cli):
        """D78: a NULL among a range's values is a NULL bound, as a NULL point is; `count`
        counts the values that are not NULL, as SQL's `count(e)` does."""
        rows = ((1, "A", 1, None, 2), (2, "A", 2, 6, 1), (3, "A", 3, 1, 3))
        result = decidb_cli.execute_raw(_edge("x <= sum(from previous to next: t.v) over (t.period)", rows=rows))
        assert ScopeError.PHRASES["null_value"] in result.stderr, result.stderr
        assert_no_internal_leak(result, "a NULL in a range")
        sql = _edge("x <= count(from previous to next: t.v) over (t.period)", rows=rows)
        assert [int(row["x"]) for row in answer(decidb_cli, sql)] == [1, 2, 2]

    @pytest.mark.parametrize("clause_sql, expected", [
        pytest.param("x <= datediff('day', DATE '2020-01-01', max(from previous to next: t.d) over (t.id within t.g))",
                     [8, 8, 1], id="date-max"),
        pytest.param("x <= sum(from previous to next else 0.5: t.n) over (t.id within t.g) * 2", [7, 7, 6],
                     id="fractional-else-over-whole-values"),
        pytest.param("x <= count(from previous to next else true: t.f) over (t.id within t.g)", [3, 3, 3],
                     id="boolean-count"),
    ])
    def test_a_range_over_data_reads_its_reducer_type(self, decidb_cli, clause_sql, expected):
        """A range over data reduces with SQL's own aggregate: a DATE's maximum is a DATE, an
        INTEGER payload with `else 0.5` sums as a decimal (the binder gives both one type)."""
        sql = f"""SELECT t.id, x FROM (VALUES (1, 'a', DATE '2020-01-05', 1, false),
                  (2, 'a', DATE '2020-01-09', 2, true), (3, 'b', DATE '2020-01-02', 2, true)) t(id, g, d, n, f)
            DECIDE x(INT) SUCH THAT x <= 9 AND {clause_sql} MAXIMIZE sum(x) ORDER BY t.id"""
        assert [int(row["x"]) for row in answer(decidb_cli, sql)] == expected


@pytest.mark.correctness
class TestRangeDeck:
    def test_example_2_rolling_range(self, decidb_cli, scope_solver):
        """Deck p. 45: demand over the previous, current and next period of the same product,
        bounded per tuple; a partial frame at each end keeps the positions that exist."""
        sql = f"""SELECT tid, produce {DECK_FROM}
            DECIDE produce(INT)
            SUCH THAT produce <= T.cap AND
                produce + sum(from previous to next: T.demand) over (T.period within T.productID) <= 15
            MAXIMIZE sum(produce) ORDER BY tid"""
        rolling = Range("sum", previous(), next_(), Col("demand"), Over("period", within=("productID",)))
        clauses = [clause(le(PRODUCE, Col("cap"))), clause(le(PRODUCE + rolling, 15))]
        _judged(decidb_cli, scope_solver, sql, _deck_rows(), [Decision("produce")], clauses,
                Objective(ObjSense.MAXIMIZE, reduce("sum", PRODUCE)))

    @pytest.mark.parametrize("policy_sql, policy", POLICIES, ids=["default", "else-0", "all"])
    def test_example_6_boundary_policies(self, decidb_cli, scope_solver, policy_sql, policy):
        """Deck p. 49: the same rolling demand under each policy -- the default keeps a
        partial frame, `else 0` fills it, `all` requires all three positions (the first and
        last periods then have no instance)."""
        policy_sql = policy_sql.replace("else 1", "else 0")
        policy = {"fallback": Const(0)} if "fallback" in policy else policy
        sql = f"""SELECT tid, produce {DECK_FROM}
            DECIDE produce(INT)
            SUCH THAT produce <= T.cap AND
                produce <= sum(from previous to next{policy_sql}: T.demand) over (T.period within T.productID) - 6
            MAXIMIZE sum(produce) ORDER BY tid"""
        rolling = Range("sum", previous(), next_(), Col("demand"), Over("period", within=("productID",)), **policy)
        clauses = [clause(le(PRODUCE, Col("cap"))), clause(le(PRODUCE, rolling - 6))]
        _judged(decidb_cli, scope_solver, sql, _deck_rows(), [Decision("produce")], clauses,
                Objective(ObjSense.MAXIMIZE, reduce("sum", PRODUCE)))


@pytest.mark.error
class TestRangeErrors:
    @pytest.mark.parametrize("clause_sql, message", [
        pytest.param("sum(sum(from previous to next: x) over (t.period)) <= 4",
                     "DECIDE: sum(from previous to next: x) over (t.period) may select no position inside", id="never"),
        pytest.param("sum(sum(from 2 next to 3 next: x) over (t.period)) <= 4",
                     "give it an else, or make its timeline cyclic", id="every-position-missing"),
        pytest.param("sum(sum(from next to previous else 0: x) over (t.period)) <= 4",
                     "its first end may come after its last, which requests no position", id="reversed"),
        pytest.param("sum(avg(from previous to next all: x) over (t.period)) <= 4",
                     "drop all, or make its timeline cyclic", id="all"),
    ])
    def test_a_range_that_may_read_nothing_inside_a_reducer_over_decisions(self, decidb_cli, clause_sql, message):
        """Phase 6 owns a member a range drops from a reducer over decisions. `from previous
        to next` always reads the anchor's own position, so it is accepted; the others are
        refused, each naming what makes it read nothing."""
        result = decidb_cli.execute_raw(_edge(clause_sql))
        if "never" in message or "previous to next:" in clause_sql:
            assert result.returncode == 0 and not result.stderr.strip(), result.stderr
            return
        assert message in result.stderr, result.stderr
        assert_no_internal_leak(result, "a range inside a reducer over decisions")

    def test_count_over_decisions_is_refused(self, decidb_cli):
        result = decidb_cli.execute_raw(_edge("x <= count(from first to last: x) over (t.period)"))
        assert "COUNT over a DECIDE variable is degenerate" in result.stderr, result.stderr

    def test_an_objective_range_that_reads_nothing(self, decidb_cli):
        result = decidb_cli.execute_raw(_edge("x <= t.v", objective="MAXIMIZE sum(x) + sum(from last to first: x) "
                                                                     "over (t.period)"))
        assert ("selects no position; the objective is produced once and cannot be skipped; its first end may come "
                "after its last, which requests no position.") in result.stderr, result.stderr

    def test_the_frame_limit(self, decidb_cli):
        """`decide_frame_limit` caps the values one frame reads over its ranges: every row of
        every selected position, and every `else` standing in for a missing one."""
        sql = _edge("x <= sum(from first to last: t.v) over (t.period within t.product)")
        result = decidb_cli.execute_raw("SET decide_frame_limit = 4; " + sql)
        assert ("DECIDE: sum(from first to last: t.v) over (t.period within t.product) reads more than 4 values over "
                "its ranges, above decide_frame_limit (4). Narrow the range, or raise the setting.") in result.stderr, \
            result.stderr
        assert [int(row["x"]) for row in answer(decidb_cli, "SET decide_frame_limit = 5; " + sql)] == [9, 9, 9, 9, 9]
        result = decidb_cli.execute_raw("SET decide_frame_limit = 0")
        assert "decide_frame_limit must be >= 1" in result.stderr, result.stderr

    def test_explain_names_the_helpers(self, decidb_cli):
        explained = _explained(decidb_cli, _edge("sum(from previous to next: x) over (t.period within t.product) <= 7 "
                                                 "AND x <= max(from first to last: x) over (t.period) - 1"))
        assert _bare("a helper decision per position") in explained, explained
        assert _bare("a helper decision per timeline") in explained, explained

    def test_diagnose_names_no_internal(self, decidb_cli):
        """An infeasible range clause: DIAGNOSE reports it with no internal name."""
        sql = _edge("x >= 5 AND sum(from previous to next: x) over (t.period within t.product) <= 7")
        result = run_diagnose(decidb_cli, sql)
        assert result.returncode == 0, result.stderr
        assert diagnose_rows(result), result.stdout
        assert_no_internal_leak(result, "DIAGNOSE of a range clause")

    @pytest.mark.parametrize("cli_fixture", ["decidb_cli_gurobi", "decidb_cli_highs"])
    def test_an_infeasible_model_is_infeasible_on_both_backends(self, request, cli_fixture):
        """Found by the range fuzzer: `count(min(from next to first: 2) ...)` counts an empty
        range, 0, fixing every x at 0, which leaves `3 * y = 1` over a whole y. HiGHS's
        MIP presolve called a point violating a bound optimal and reported a solve error;
        it is solved once more without presolve, and says infeasible as Gurobi does."""
        cli = request.getfixturevalue(cli_fixture)
        rows = ((1, "b", 4, -1), (2, "a", 3, 0), (3, "b", 1, -1), (4, "a", 4, 1), (5, "b", 6, 3), (6, "b", 7, -3),
                (7, "a", 2, 0))
        sql = f"""SELECT t.id, x, y {table('t', ('id', 'g', 'period', 'w'), rows)}
            DECIDE x(INT), per t.g: y(INT)
            SUCH THAT y <= 4 AND per (): x <= count(min(from next to first: 2) over (t.period within t.g))
              AND t.w * at(3 next: x - y) over (t.period desc within t.g) + x
                  = count(from previous to last every 2: t.w) over (t.period within t.g)
            MINIMIZE sum(abs(x - at(next: x - y) over (t.period desc cyclic within t.g))) ORDER BY t.id"""
        result = cli.execute_raw(sql)
        assert "DECIDE optimization is infeasible" in result.stderr, result.stderr


@pytest.mark.correctness
class TestRangeReview:
    """Found by the architecture review of phase 5 (`09_anr_language/done.md`, "phase 5")."""

    @pytest.mark.parametrize("clause_sql, body, objective", [
        pytest.param("x + sum(from previous to previous else 0.5: x) over (t.period within t.product) <= 9",
                     le(X + _range("sum", previous(), previous(), 1, {"fallback": Const(0.5)}, X), 9),
                     MAXIMIZE_X, id="fractional-else"),
        pytest.param("max(from previous to next else 0.5: x) over (t.period within t.product) >= 0.5",
                     ge(_range("max", previous(), next_(), 1, {"fallback": Const(0.5)}, X), 0.5),
                     Objective(ObjSense.MINIMIZE, reduce("sum", X)), id="fractional-else-bound"),
        pytest.param("sum(from previous to next: x + 0.5) over (t.period within t.product) <= 4",
                     le(_range("sum", previous(), next_(), 1, {}, X + 0.5), 4), MAXIMIZE_X,
                     id="fractional-addend"),
        pytest.param("x + sum(from previous to previous else t.f: x) over (t.period within t.product) <= 9",
                     le(X + _range("sum", previous(), previous(), 1, {"fallback": Col("f")}, X), 9), MAXIMIZE_X,
                     id="fractional-data-else"),
    ])
    def test_a_fractional_range_is_no_whole_helper(self, decidb_cli, scope_solver, clause_sql, body, objective):
        """A range's helper is whole-numbered only when the type its payload and `else` share
        is: an INTEGER helper for `else 0.5` called a feasible query infeasible and stepped
        `>= 0.5` to `>= 1`."""
        rows = tuple(row + (0.5,) for row in EDGE_ROWS)
        verb = "MINIMIZE" if objective.sense == ObjSense.MINIMIZE else "MAXIMIZE"
        sql = f"""SELECT id, x {table('t', EDGE + ('f',), rows)} DECIDE x(INT)
            SUCH THAT x <= 9 AND {clause_sql} {verb} sum(x) ORDER BY id"""
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE + ("f",), rows), [Decision("x")], [CAP, clause(body)],
                objective)

    @pytest.mark.parametrize("sense", ["<", ">", "<>"])
    @pytest.mark.parametrize("frame", ["sum(from previous to next: x + 0.5) over (t.period)",
                                       "sum(from previous to next else 0.5: x) over (t.period)"])
    def test_a_strict_bound_on_a_fractional_range_is_refused(self, decidb_cli, sense, frame):
        """The binder reads a range's whole-numberedness the same way, so `<`, `>` and `<>`
        over a fractional range get its fractional message, not an internal error."""
        result = decidb_cli.execute_raw(_edge(f"{frame} {sense} 3"))
        assert "'sum(from previous to next" in result.stderr and "has type DECIMAL" in result.stderr, result.stderr
        assert "can take fractional values" in result.stderr, result.stderr
        assert_no_internal_leak(result, "a strict bound on a fractional range")

    def test_an_else_of_another_type(self, decidb_cli, scope_solver):
        """The `else` takes the type the bound reducer reads, which its overload may widen
        the payload to: a FLOAT `else` beside an INTEGER payload sums as a DOUBLE."""
        sql = _edge("x <= sum(from previous to next else CAST(0.5 AS FLOAT): t.v) over (t.period within t.product)")
        body = le(X, _range("sum", previous(), next_(), 1, {"fallback": Const(0.5)}, Col("v")))
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, EDGE_ROWS), [Decision("x")], [CAP, clause(body)],
                MAXIMIZE_X)

    def test_count_all_that_cannot_miss_inside_a_reducer_over_decisions(self, decidb_cli, scope_solver):
        """`from first to previous` never requests a position off its timeline, so `count ...
        all` reads 0 at worst and is no absent member: accepted beside a decision."""
        sql = _edge("sum(x + count(from first to previous all: t.v) over (t.period within t.product)) <= 20")
        frame = _range("count", FIRST, previous(), 1, {"all": True}, Col("v"))
        _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, EDGE_ROWS), [Decision("x")],
                [CAP, clause(le(reduce("sum", X + frame), 20))], MAXIMIZE_X)

    def test_a_window_all_makes_absent_reads_nothing(self, decidb_cli):
        """Under `all` a window with a missing position reads no row: the payload is never
        evaluated there, so a cast that fails on a row only such windows select raises
        nothing."""
        sql = """SELECT t.id, x FROM (VALUES (1, 1, '4'), (2, 2, 'none'), (3, 3, '1')) t(id, period, s)
            DECIDE x(INT) SUCH THAT x <= 9 AND
                x <= sum(from previous to next every 2 all: CAST(t.s AS INTEGER)) over (t.period)
            MAXIMIZE sum(x) ORDER BY t.id"""
        assert [int(row["x"]) for row in answer(decidb_cli, sql)] == [9, 5, 9]

    def test_count_skips_a_null_else(self, decidb_cli):
        """`count` counts the values that are not NULL, its `else` among them, as SQL's
        `count(e)` does."""
        sql = """SELECT t.id, x FROM (VALUES (1, 1, 4, NULL), (2, 2, 6, 2), (3, 3, 1, 3)) t(id, period, v, n)
            DECIDE x(INT) SUCH THAT x <= 9 AND x <= count(from previous to next else t.n: t.v) over (t.period)
            MAXIMIZE sum(x) ORDER BY t.id"""
        assert [int(row["x"]) for row in answer(decidb_cli, sql)] == [2, 3, 3]

    def test_split_windows_count_against_the_limit(self, decidb_cli):
        """An `else` that reads the anchor's row parts a position's window per anchor, and
        each part reads the position's values again: each counts against the limit."""
        sql = ("SET decide_frame_limit = 30; SELECT id, x FROM (SELECT i AS id, 1 AS p, i AS w FROM range(5) r(i)) t "
               "DECIDE x(INT) SUCH THAT x <= 9 AND sum(from previous to next else t.w: x) over (t.p) <= 50 "
               "MAXIMIZE sum(x)")
        result = decidb_cli.execute_raw(sql)
        assert "reads more than 30 values over its ranges, above decide_frame_limit (30)" in result.stderr, \
            result.stderr

    @pytest.mark.parametrize("bound, optimum", [(4, None), (8, 20)])
    def test_a_reducer_beside_a_range_under_abs_keeps_its_own_helper(self, decidb_cli, scope_solver, bound,
                                                                     optimum):
        """The value rewrite matched a reducer's second occurrence -- the copy an ABS envelope
        holds -- to the link at its own index, which the range rewrite, running first, had
        taken: the copy read the range's helper, and with the bound 4 an infeasible query
        answered, breaking a generated row."""
        sql = _edge(f"x <= t.v AND abs(x - 0.5 * sum(x) + sum(from first to last: x) over (t.period within t.product) "
                    f"- 10) <= {bound}")
        body = le(Abs(X - 0.5 * reduce("sum", X) + _range("sum", FIRST, LAST, 1, {}, X) - 10), bound)
        clauses = [CAP, clause(le(X, Col("v"))), clause(body)]
        if optimum is None:
            model = AnrModel(table_rows(EDGE, EDGE_ROWS), [Decision("x")])
            assert model.solve(scope_solver, clauses, MAXIMIZE_X).status == SolverStatus.INFEASIBLE
            assert "is infeasible" in decidb_cli.execute_raw(sql).stderr
            return
        assert _judged(decidb_cli, scope_solver, sql, table_rows(EDGE, EDGE_ROWS), [Decision("x")], clauses,
                       MAXIMIZE_X) == optimum
