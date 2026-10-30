"""Frame selectors (phases 3-5 of
``context/descriptions/09_anr_language/anr_algorithm_and_frames_plan.md``, §6.6;
``syntax_reference.md`` §5.6).

A frame reads a payload at a position of the anchor row's timeline: ``at(F [else v]: e)
over (S [desc] [cyclic] [within K])`` for one position, ``agg(from F1 to F2 [every d]
[else v | all]: e) over (...)`` for several. Phase 3 parses, binds and plans every
spelling; phase 4 formulates the point frames, and a range still stops at one place with
"not supported yet" until phase 5. ``TestSyntax`` holds the parse, bind and echo contract
and every refusal of D74; ``TestPoint`` runs every phase-4 fixture of ``_anr_cases``, the
deck's point examples and the DeQL comparison through DeciDB, each judged by
``_anr_oracle``, which reads no SQL. The exact parse errors are
``test/decidb/test_frame_parser.test``'s, the round trips ``test_decidb_plan_serialization.cpp``'s.
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

RANGE_NOT_YET = "DECIDE: a range frame selector is not supported yet: "

RANGE_CASES = [pytest.param(case, id=case.name) for case in CASES if " over (" in case.sql and case.phase >= 5]

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
    @pytest.mark.parametrize("case", RANGE_CASES)
    def test_every_range_fixture_binds_and_stops_at_one_place(self, decidb_cli, case):
        """Every range fixture of phases 5-6, as written, binds: no parse or bind error comes
        first, not even the ones its data raises once it runs."""
        result = decidb_cli.execute_raw(case.sql)
        assert RANGE_NOT_YET in result.stderr, result.stderr
        assert_no_internal_leak(result, case.name)

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
    def test_a_range_stops_at_the_gate_as_written(self, decidb_cli, clause, frame):
        """The range's refusal quotes it as written, the defaults left out."""
        result = decidb_cli.execute_raw(_query(clause))
        assert RANGE_NOT_YET + frame in result.stderr, result.stderr
        assert_no_internal_leak(result, "a range")

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

    def test_no_later_layer_refuses_a_range_first(self, decidb_cli):
        """The gate stands before canonicalization, the first layer that would read a frame,
        so none refuses a range for a reason of its own first: `<>` beside an AVG reads as a
        reducer's refusal there."""
        result = decidb_cli.execute_raw(
            f"SELECT id, x {T} DECIDE x(INT) SUCH THAT x <= 9 AND "
            "x + avg(from first to last: x) over (t.period) <> 3 MAXIMIZE sum(x)")
        assert RANGE_NOT_YET + "avg(from first to last: x) over (t.period)" in result.stderr, result.stderr

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
