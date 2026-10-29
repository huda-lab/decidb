"""Frame selectors (phases 3-5 of
``context/descriptions/09_anr_language/anr_algorithm_and_frames_plan.md``, §6.6;
``syntax_reference.md`` §5.6).

A frame reads a payload at a position of the anchor row's timeline: ``at(F [else v]: e)
over (S [desc] [cyclic] [within K])`` for one position, ``agg(from F1 to F2 [every d]
[else v | all]: e) over (...)`` for several. Phase 3 parses, binds and plans every
spelling; formulating one is phase 4's, so a frame stops at one place with "not supported
yet". ``TestSyntax`` holds that line: every frame fixture of ``_anr_cases`` binds and stops
there, and every refusal of D74 names its construct. The exact parse errors and the echo
are ``test/decidb/test_frame_parser.test``'s and ``test_decidb_plan_serialization.cpp``'s.
"""

import pytest

from ._anr_cases import CASES
from ._output_helpers import assert_no_internal_leak
from ._scope_cases import table

NOT_YET = "DECIDE: frame selectors are not supported yet: "

FRAME_CASES = [pytest.param(case, id=case.name) for case in CASES if " over (" in case.sql]

T = table("t", ("id", "product", "period", "demand", "price"),
          ((1, "A", 1, 5, 2.5), (2, "A", 2, 6, 1.5), (3, "B", 1, 7, 0.5)))


def _query(clause: str, objective: str = "MAXIMIZE sum(x)") -> str:
    return f"SELECT id, x {T} DECIDE x(REAL), per (): s(REAL) SUCH THAT x <= 9 AND {clause} {objective}"


@pytest.mark.error
class TestSyntax:
    @pytest.mark.parametrize("case", FRAME_CASES)
    def test_every_frame_fixture_binds_and_stops_at_one_place(self, decidb_cli, case):
        """Every frame fixture of phases 4-6, as written, binds: no parse or bind error comes
        first, not even the ones its data raises once it runs (an ambiguous peer, a spanned
        timeline, a reached fallback that fails)."""
        result = decidb_cli.execute_raw(case.sql)
        assert NOT_YET in result.stderr, result.stderr
        assert_no_internal_leak(result, case.name)

    @pytest.mark.parametrize("clause, frame", [
        pytest.param("x >= at(previous: x) over (t.period within t.product)",
                     "at(previous: x) over (t.period within t.product)", id="point"),
        pytest.param("x <= at(first: t.demand) over (t.period desc cyclic)",
                     "at(first: t.demand) over (t.period desc cyclic)", id="data-payload"),
        pytest.param("x >= at(2 next else s: x) over (t.period within t)",
                     "at(2 next else s: x) over (t.period within t)", id="query-wide-fallback"),
        pytest.param("sum(from 2 previous to 1 previous every 1: x) over (t.period asc within ()) <= 4",
                     "sum(from 2 previous to previous: x) over (t.period)", id="range-defaults-dropped"),
        pytest.param("avg(from first to last all: x) over (t.period) >= 1",
                     "avg(from first to last all: x) over (t.period)", id="range-all"),
        pytest.param("x <= count(from first to last: t.demand) over (t.period)",
                     "count(from first to last: t.demand) over (t.period)", id="range-count"),
        pytest.param("sum(x) by (t.product) <= sum(at(last: t.demand) over (t.period within t.product))",
                     "at(last: t.demand) over (t.period within t.product)", id="inside-a-reducer"),
        pytest.param("t.price * at(previous: x) over (t.period) <= 3",
                     "at(previous: x) over (t.period)", id="data-coefficient"),
    ])
    def test_a_frame_binds_and_echoes_as_written(self, decidb_cli, clause, frame):
        """The refusal quotes the frame as written, the defaults left out."""
        result = decidb_cli.execute_raw(_query(clause))
        assert NOT_YET + frame in result.stderr, result.stderr
        assert_no_internal_leak(result, "a frame")

    def test_a_frame_in_the_objective_binds(self, decidb_cli):
        result = decidb_cli.execute_raw(_query(
            "x >= 0", "MINIMIZE sum(abs(x - at(previous else 0: x) over (t.period within t.product)))"))
        assert NOT_YET + "at(previous else 0: x) over (t.period within t.product)" in result.stderr, result.stderr

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
        reducer or a frame in a payload, and a frame term times a decision."""
        result = decidb_cli.execute_raw(_query(clause))
        assert message in result.stderr, result.stderr
        assert NOT_YET not in result.stderr
        assert_no_internal_leak(result, "a frame refusal")

    @pytest.mark.parametrize("clause, frame", [
        pytest.param("x >= AT(PREVIOUS ELSE 0: x) OVER (t.period CYCLIC)",
                     "at(previous else 0: x) over (t.period cyclic)", id="upper-case-words"),
        pytest.param("x <= at(previous else 0.5: x) over (t.period)",
                     "at(previous else 0.5: x) over (t.period)", id="fractional-fallback"),
        pytest.param("x >= at(previous else s: t.demand) over (t.period)",
                     "at(previous else s: t.demand) over (t.period)", id="decision-fallback-over-data"),
    ])
    def test_every_frame_stops_at_the_gate_first(self, decidb_cli, clause, frame):
        result = decidb_cli.execute_raw(_query(clause))
        assert NOT_YET + frame in result.stderr, result.stderr

    def test_no_later_layer_refuses_a_frame_first(self, decidb_cli):
        """The gate stands before canonicalization, the first layer that would read a frame,
        so none refuses one for a reason of its own first: `<>` beside an AVG reads as a
        reducer's refusal there."""
        result = decidb_cli.execute_raw(
            f"SELECT id, x {T} DECIDE x(INT) SUCH THAT x <= 9 AND "
            "x + avg(from first to last: x) over (t.period) <> 3 MAXIMIZE sum(x)")
        assert NOT_YET + "avg(from first to last: x) over (t.period)" in result.stderr, result.stderr

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
