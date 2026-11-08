"""Which error a DECIDE query reports when it raises two.

Evaluation runs in phases, and a query that breaks two of them reports the first phase's error. Nothing else pins that
order, and the rewrite of the evaluator changes the order evaluation runs in (literal one-pass plan, P0, and its risk
table). So this file states the order as it stands, one solver-path query per pair of phases:

    sink_state     DecideGlobalSinkState's constructor: a written bound the decision's domain contradicts
    side_plan      a check under the operator, before it reads a row: a NaN a reducer on a bound reads
    (the one pass) DecideRowStore::Build raises nothing: it holds a row's error, and the reader of that row raises it
    frames         BuildFrames: a `per` class that spans two timelines (D75)
    keyed_mapping  BuildKeyedMappings: a row error the definition of an auxiliary reads
    scope_check    RunScopeChecks: an objective produced more than once (D72)
    frame_peers    CheckFramePeers: a position that holds two rows (D73)
    constraints    EvaluateConstraints: a coefficient that is not finite
    objective      EvaluateObjective: the same in the objective
    composed       EvaluateComposedClauses: the same in a composed MIN/MAX
    value_links    EvaluateReducerValueLinks: the same in a helper's members
    assembly       BuildSolverInput: like terms merged past a DOUBLE
    formulation    FormulateModel: an infinite bound a Big-M cannot dominate
    model_build    SolverModel::Build: objective coefficients merged past a DOUBLE, or a `>=` row's (MODEL_ROW)
    solver         the solver's own answer: infeasible

Fault injection by data: each phase has one fault, a clause or objective term that raises only when a cell of its own
column holds a bad value. A pair's three runs share one query and differ in the table alone, but for a fault a literal
raises (`Fault.refused`), which is written in place of its clause: both faults on pins the winner, and each fault on
alone is the control that shows the other one raises in this very query. The later phase's fault is written first, so
the phase decides, not the written order. No term can be written before SUCH THAT, though, so where a clause is to raise
before an objective term the two orders agree, and the case pins the phase order alone. So model_build, whose grid fault
is an objective term, has a row fault too, MODEL_ROW, written before each earlier phase's clause; scope_check and
objective raise in no clause. Every message is compared whole. Each fault's phase was read off a backtrace at its
throw; the file itself can only see the order.

Within a phase the written order decides (`test_within_a_phase_the_first_written_raises`), except where a phase runs in
sub-steps (`test_a_sub_step_decides_whatever_the_written_order`) and in sink_state, which follows the declared decisions
(`test_the_sink_state_reads_decisions_as_declared`). A rule reads the same order as the solver does
(`test_a_rule_raises_in_the_solver_order`), under `require`, which fails unless the rule takes the query; `auto` takes
the same rule. The grid declares a query-wide decision, which no rule takes, so it runs under `off` alone.
"""

from dataclasses import dataclass
from itertools import combinations

import pytest

from ._scope_cases import literal

NAN = float("nan")
INFINITY = float("inf")
BIG = 1e308

SQRT = "Out of Range Error: cannot take square root of a negative number"
LN = "Out of Range Error: cannot take logarithm of a negative number"
INFEASIBLE = (
    "Invalid Input Error: DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see which clause to "
    "change."
)


def _coefficient(value):
    return (
        f"Invalid Input Error: DECIDE: {value} is NaN or infinite on some rows. "
        f"Skip those rows with 'when isfinite({value}):'."
    )


def _bound(value):
    return (
        f"Invalid Input Error: DECIDE: the bound {value} is NaN on some rows. "
        f"Skip those rows with 'when NOT isnan({value}):'."
    )


def _finite_bound(value):
    return (
        f"Invalid Input Error: DECIDE: the bound {value} is infinite on some rows, and this clause needs a finite one. "
        f"Skip those rows with 'when isfinite({value}):'."
    )


def _too_large(written):
    return f"Invalid Input Error: DECIDE: {written} computes a number too large for a DOUBLE. Scale its data down."


def _domain(bound, name):
    return (
        f"Invalid Input Error: DECIDE optimization is infeasible: {bound} cannot hold because {name} is non-negative "
        "(>= 0). Add an explicit lower bound if it may be negative."
    )


def _spans(column, frame):
    return (
        f"Invalid Input Error: DECIDE: per t.{column} does not determine the timeline t.tl of {frame}. "
        f"t.{column} = '{TEXT[column][0]}' spans t.tl = 'A', 'B'."
    )


def _peers(frame, column, position, timeline="A"):
    return (
        f"Invalid Input Error: DECIDE: {frame} finds 2 rows at t.tl = '{timeline}', t.{column} = {position}; "
        "a position must hold one row."
    )


def _limited(frame, limit):
    return (
        f"Invalid Input Error: DECIDE: {frame} reads more than {limit} values over its ranges, above "
        f"decide_frame_limit ({limit}). Narrow the range, or raise the setting."
    )


def _twice(column):
    return (
        f"Invalid Input Error: DECIDE objective: 't.{column}' takes more than one value over the kept rows (1.0, 2.0), "
        "so the objective would be produced more than once; move it inside a reducer or fix it with when."
    )


#: Four rows in two timelines of two positions. The clean table raises nothing.
TEXT = {
    "tl": ["A", "A", "B", "B"],
    "cls": ["c1", "c1", "c2", "c2"],
    "cls2": ["e1", "e1", "e2", "e2"],
}
WHOLE = {
    "id": [1, 2, 3, 4],
    "q": [1, 2, 1, 2],
    "pos": [1, 2, 1, 2],
    "pos2": [1, 2, 1, 2],
}
NUMBER = {
    "pay": [1.0, 2.0, 3.0, 4.0],
    "fval": [9.0] * 4,
    "side": [1.0] * 4,
    "side2": [1.0] * 4,
    "root": [1.0] * 4,
    "root2": [2.0] * 4,
    "base": [2.0] * 4,
    "level": [1.0] * 4,
    "level2": [1.0] * 4,
    "weight": [1.0] * 4,
    "cap": [100.0] * 4,
    "cap2": [100.0] * 4,
    "floor_at": [200.0] * 4,
    "gain": [1.0] * 4,
    "gain2": [1.0] * 4,
    "sq": [1.0] * 4,
    "peak": [1.0] * 4,
    "peak2": [1.0] * 4,
    "crest": [1.0] * 4,
    "share": [1.0] * 4,
    "share2": [1.0] * 4,
    "huge": [1.0] * 4,
    "huge2": [1.0] * 4,
    "mass": [1.0] * 4,
    "mass2": [1.0] * 4,
    "vast": [1.0] * 4,
    "far": [0.0] * 4,
    "gap": [0.0] * 4,
    "gap2": [0.0] * 4,
    "cost": [1.0] * 4,
    "need": [0.0] * 4,
}


@dataclass(frozen=True)
class Fault:
    """What raises `message`: a clause or an objective term, turned on by `cells`, each `(column, row, value)`, or by
    `refused`, written in place of `clause` for a fault a literal raises rather than a row. With neither `clause` nor
    `term` it is one part of a clause written elsewhere."""

    name: str
    message: str
    clause: str = ""
    term: str = ""
    cells: tuple = ()
    refused: str = ""


def _both_rows(column):
    """A sum of two rows is past a DOUBLE where one row is not."""
    return ((column, 0, BIG), (column, 1, BIG))


AT_PREVIOUS = "at(previous else 9: t.pay) over (t.q within t.tl)"
AT_NEXT = "at(next else 9: t.pay) over (t.q within t.tl)"
AT_FIRST = "at(first else 9: t.pay) over (t.pos within t.tl)"
AT_LAST = "at(last else 9: t.pay) over (t.pos2 within t.tl)"

SINK_STATE = Fault("sink_state", _domain("x <= -1", "x"), clause="x <= 8", refused="x <= -1")
SIDE_PLAN = Fault("side_plan", _bound("t.side"), clause="sum(x) <= sum(t.side) + 100", cells=(("side", 0, NAN),))
FRAMES = Fault("frames", _spans("cls", AT_PREVIOUS), clause=f"per t.cls: x <= {AT_PREVIOUS}", cells=(("cls", 2, "c1"),))
KEYED_MAPPING = Fault("keyed_mapping", SQRT, clause="abs(sqrt(t.root) * x - 1) <= 30", cells=(("root", 0, -1.0),))
SCOPE_CHECK = Fault("scope_check", _twice("level"), term="t.level * s", cells=(("level", 0, 2.0),))
FRAME_PEERS = Fault("frame_peers", _peers(AT_FIRST, "pos", 1), clause=f"x <= {AT_FIRST} + 20", cells=(("pos", 1, 1),))
CONSTRAINTS = Fault(
    "constraints", _coefficient("t.weight"), clause="sum(t.weight * x) <= 100", cells=(("weight", 0, NAN),)
)
OBJECTIVE = Fault("objective", _coefficient("t.gain"), term="sum(t.gain * x)", cells=(("gain", 0, NAN),))
COMPOSED = Fault(
    "composed", _coefficient("t.peak"), clause="sum(x) + max(t.peak * x) <= 100", cells=(("peak", 0, NAN),)
)
VALUE_LINKS = Fault(
    "value_links", _coefficient("t.share"), clause="x <= 0.5 * sum(t.share * x) + 9", cells=(("share", 0, NAN),)
)
ASSEMBLY = Fault(
    "assembly", _too_large("sum(t.huge * s) <= 1000"), clause="sum(t.huge * s) <= 1000", cells=_both_rows("huge")
)
FORMULATION = Fault(
    "formulation", _finite_bound("t.gap"), clause="norm(x + t.gap, 0) <= 2", cells=(("gap", 0, INFINITY),)
)
# The message prints the objective as written, so it is filled in for each query (_expected).
MODEL_BUILD = Fault("model_build", _too_large("MINIMIZE {objective}"), term="sum(t.cost * s)", cells=_both_rows("cost"))
SOLVER = Fault("solver", INFEASIBLE, clause="x + s >= t.need", cells=(("need", 0, 100.0),))

PHASES = [
    SINK_STATE,
    SIDE_PLAN,
    FRAMES,
    KEYED_MAPPING,
    SCOPE_CHECK,
    FRAME_PEERS,
    CONSTRAINTS,
    OBJECTIVE,
    COMPOSED,
    VALUE_LINKS,
    ASSEMBLY,
    FORMULATION,
    MODEL_BUILD,
    SOLVER,
]

# The one pass raises nothing of its own, so it has no fault. These pin what it does instead: a row's error, or a
# constant's, waits for its reader. A varying value read in a helper's members is read by the NULL test of its reducer
# as early as keyed_mapping, so the last reader of a row error is a composed term.
HELD_ROW_ERROR = Fault("held_row_error", LN, clause="sum(x) + min(ln(t.base) * x) <= 100", cells=(("base", 0, -1.0),))
HELD_CONSTANT = Fault(
    "held_constant",
    SQRT,
    clause="sum(sqrt(1.0::DOUBLE) * x) <= 100",
    refused="sum(sqrt(-1.0::DOUBLE) * x) <= 100",
)

BEFORE_THE_LAST_READ = PHASES[: PHASES.index(COMPOSED)]
AFTER_THE_LAST_READ = PHASES[PHASES.index(COMPOSED) + 1 :]

# model_build's other fault, a clause, so it can be written before an earlier phase's clause, as MODEL_BUILD's objective
# term cannot. Implied-bound propagation (assembly) reads `<=` and `=` rows alone, so this `>=` row overflows at
# model_build.
MODEL_ROW = Fault(
    "model_row", _too_large("sum(t.mass * s) >= -1000"), clause="sum(t.mass * s) >= -1000", cells=_both_rows("mass")
)

# Left out, as a reported defect: this pair's winner is a race. The sink's state is built in its pipeline's initialize
# task, which nothing orders against the side plan's pipeline, so at two threads or more either error can be pushed
# first. threads=1 would pin the scheduler's order, not the engine's, so the pair is not pinned there either.
RACED = (SINK_STATE, SIDE_PLAN)

PAIRS = [
    *(pair for pair in combinations(PHASES, 2) if pair != RACED),
    *((phase, HELD_ROW_ERROR) for phase in BEFORE_THE_LAST_READ),
    *((HELD_ROW_ERROR, phase) for phase in AFTER_THE_LAST_READ),
    *((fault, MODEL_ROW) for fault in [*PHASES[: PHASES.index(MODEL_BUILD)], HELD_ROW_ERROR] if fault.clause),
    (FRAME_PEERS, HELD_CONSTANT),
    (HELD_CONSTANT, OBJECTIVE),
    # The composed clauses are read after the objective, so a plain clause's constant raises before their row error.
    (HELD_CONSTANT, HELD_ROW_ERROR),
]

BOX = ("x <= 9", "s <= 9")
DECLARATION = "x(REAL), per (): s(REAL)"


def _table(on):
    columns = {name: list(values) for name, values in {**TEXT, **WHOLE, **NUMBER}.items()}
    for fault in on:
        for column, row, value in fault.cells:
            columns[column][row] = value

    def cell(value):
        return f"'{value}'::DOUBLE" if isinstance(value, float) else literal(value)

    rows = zip(*columns.values())
    return f"(VALUES {', '.join('(' + ', '.join(cell(v) for v in row) + ')' for row in rows)}) t({', '.join(columns)})"


def _objective(written):
    return " + ".join(["sum(x)", *(fault.term for fault in written if fault.term)])


def _decide(written, on):
    clauses = [fault.refused if fault in on and fault.refused else fault.clause for fault in written if fault.clause]
    return f"{DECLARATION} SUCH THAT {' AND '.join([*BOX, *clauses])} MINIMIZE {_objective(written)}"


def _error(cli, decide, on, mode="off", setup=""):
    sql = f"SET decide_direct_solve='{mode}'; {setup}SELECT id, x FROM {_table(on)} DECIDE {decide}"
    return cli.execute_raw(sql).stderr.strip()


def _expected(fault, written):
    return fault.message.format(objective=_objective(written))


def _assert_wins(cli, winner, loser, written, setup=""):
    """Both on, `winner` raises; each on alone, each raises, so the query does hold both errors."""
    on = [winner, loser]
    assert _error(cli, _decide(written, on), on, setup=setup) == _expected(winner, written)
    assert _error(cli, _decide(written, [winner]), [winner], setup=setup) == _expected(winner, written)
    assert _error(cli, _decide(written, [loser]), [loser], setup=setup) == _expected(loser, written)


@pytest.mark.error
@pytest.mark.parametrize("earlier, later", PAIRS, ids=[f"{pair[0].name}-{pair[1].name}" for pair in PAIRS])
def test_the_earlier_phase_raises(decidb_cli, earlier, later):
    _assert_wins(decidb_cli, earlier, later, [later, earlier])


def test_every_fault_is_off_on_the_clean_table(decidb_cli):
    # Each coefficient of the objective is positive, and x = 0, s = 0 satisfies every clause: abs(0 - 1) <= 30, a norm
    # of no non-zero value, 0 + 0 >= 0. So nothing raises, and the minimum is 0 on every row.
    decide = _decide([*PHASES, HELD_ROW_ERROR, HELD_CONSTANT], [])
    rows, _ = decidb_cli.execute(f"SET decide_direct_solve='off'; SELECT id, x FROM {_table([])} DECIDE {decide}")
    assert rows == [(1, 0.0), (2, 0.0), (3, 0.0), (4, 0.0)]


# A second fault of each phase the written order decides, each read in its phase's loop over clauses, terms or frames.
SECOND_FRAME = Fault(
    "second_frame", _spans("cls2", AT_NEXT), clause=f"per t.cls2: x <= {AT_NEXT}", cells=(("cls2", 2, "e1"),)
)
SECOND_ROW_ERROR = Fault("second_row_error", LN, clause="abs(ln(t.root2) * x - 1) <= 30", cells=(("root2", 0, -1.0),))
SECOND_FIELD = Fault("second_field", _twice("level2"), term="t.level2 * s", cells=(("level2", 0, 2.0),))
SECOND_PEER = Fault("second_peer", _peers(AT_LAST, "pos2", 2), clause=f"x <= {AT_LAST} + 20", cells=(("pos2", 0, 2),))
# A bound, against CONSTRAINTS' coefficient: a clause is read whole before the next one, whichever kind each is.
ROW_BOUND = Fault("row_bound", _bound("t.cap"), clause="x + s <= t.cap", cells=(("cap", 0, NAN),))
SECOND_TERM = Fault("second_term", _coefficient("t.gain2"), term="sum(t.gain2 * x)", cells=(("gain2", 0, NAN),))
SECOND_COMPOSED = Fault(
    "second_composed", _coefficient("t.peak2"), clause="sum(x) + min(t.peak2 * x) <= 100", cells=(("peak2", 0, NAN),)
)
SECOND_LINK = Fault(
    "second_link", _coefficient("t.share2"), clause="x <= 0.25 * sum(t.share2 * x) + 9", cells=(("share2", 0, NAN),)
)
SECOND_MERGE = Fault(
    "second_merge", _too_large("sum(t.huge2 * s) <= 1000"), clause="sum(t.huge2 * s) <= 1000", cells=_both_rows("huge2")
)
SECOND_NORM = Fault(
    "second_norm", _finite_bound("t.gap2"), clause="norm(x + t.gap2, 0) <= 3", cells=(("gap2", 0, INFINITY),)
)
MODEL_ROW2 = Fault(
    "model_row2", _too_large("sum(t.mass2 * s) >= -1000"), clause="sum(t.mass2 * s) >= -1000", cells=_both_rows("mass2")
)
# Its check is inserted where the bound was recorded (RecordGroupBounds), not after the other checks.
GROUP_SIDE_PLAN = Fault(
    "group_side_plan",
    _bound("t.side2"),
    clause="sum(x) by (t.tl) <= sum(t.side2) by (t.tl) + 100",
    cells=(("side2", 0, NAN),),
)

WRITTEN_ORDER = [
    (SIDE_PLAN, GROUP_SIDE_PLAN),
    (FRAMES, SECOND_FRAME),
    (KEYED_MAPPING, SECOND_ROW_ERROR),
    (SCOPE_CHECK, SECOND_FIELD),
    (FRAME_PEERS, SECOND_PEER),
    (CONSTRAINTS, ROW_BOUND),
    (OBJECTIVE, SECOND_TERM),
    (COMPOSED, SECOND_COMPOSED),
    (VALUE_LINKS, SECOND_LINK),
    (ASSEMBLY, SECOND_MERGE),
    (FORMULATION, SECOND_NORM),
    (MODEL_ROW, MODEL_ROW2),
]


@pytest.mark.error
@pytest.mark.parametrize("one, other", WRITTEN_ORDER, ids=[pair[0].name for pair in WRITTEN_ORDER])
def test_within_a_phase_the_first_written_raises(decidb_cli, one, other):
    _assert_wins(decidb_cli, one, other, [one, other])
    _assert_wins(decidb_cli, other, one, [other, one])


FRAME_VALUE = Fault(
    "frame_value",
    "Invalid Input Error: DECIDE: min(from first to last: t.fval) over (t.q within t.tl) reads a NaN on some rows. "
    "Skip those rows with a 'when' on the column it reads.",
    clause="x <= min(from first to last: t.fval) over (t.q within t.tl)",
    cells=(("fval", 0, NAN),),
)
SQUARED_TERM = Fault("squared_term", _coefficient("t.sq"), term="sum(power(t.sq * x, 2))", cells=(("sq", 0, NAN),))
COMPOSED_TERM = Fault("composed_term", _coefficient("t.crest"), term="max(t.crest * x)", cells=(("crest", 0, NAN),))
# A row error the objective's composed term holds, which waits for the composed phase's read of the objective.
HELD_TERM_ERROR = Fault("held_term_error", SQRT, term="max(sqrt(t.crest) * x)", cells=(("crest", 0, -1.0),))
EXTREMUM_ROW = Fault(
    "extremum_row",
    _too_large("max(x + t.far + t.far) >= 3"),
    clause="max(x + t.far + t.far) >= 3",
    cells=(("far", 0, BIG),),
)
COMPOSED_ROW = Fault(
    "composed_row",
    _too_large("sum(t.vast * s) + max(x) <= 1000"),
    clause="sum(t.vast * s) + max(x) <= 1000",
    cells=_both_rows("vast"),
)

# (runs first, runs after): BuildFrames checks each frame's partition before any frame's column is read, the objective
# reads its linear terms before its squares, the composed phase reads clauses before the objective, their coefficients
# and the row errors they hold alike, FormulateModel works by construct, and SolverModel::Build sets up the objective
# before the rows.
SUB_STEPS = [
    (FRAMES, FRAME_VALUE),
    (OBJECTIVE, SQUARED_TERM),
    (COMPOSED, COMPOSED_TERM),
    (HELD_ROW_ERROR, HELD_TERM_ERROR),
    (FORMULATION, EXTREMUM_ROW),
    (EXTREMUM_ROW, COMPOSED_ROW),
    (MODEL_BUILD, MODEL_ROW),
]


@pytest.mark.error
@pytest.mark.parametrize("first, after", SUB_STEPS, ids=[f"{pair[0].name}-{pair[1].name}" for pair in SUB_STEPS])
def test_a_sub_step_decides_whatever_the_written_order(decidb_cli, first, after):
    # Keyed by the query: a clause and a term are written in one order only.
    for written in {_decide(order, []): order for order in ([first, after], [after, first])}.values():
        _assert_wins(decidb_cli, first, after, written)


@pytest.mark.error
def test_each_frame_is_checked_before_the_next_is_built(decidb_cli):
    # A frame's limit is raised building its map, and its partition checked right after: the frames go one at a time,
    # so here the written order decides, where FRAME_VALUE, read once they are all built, loses either way.
    text = "sum(from first to last: t.pay) over (t.id)"
    limited = Fault(
        "limited",
        _limited(text, 3),
        clause="x <= sum(from first to first: t.pay) over (t.id) + 9",
        refused=f"x <= {text} + 9",
    )
    setup = "SET decide_frame_limit = 3; "
    _assert_wins(decidb_cli, limited, FRAMES, [limited, FRAMES], setup)
    _assert_wins(decidb_cli, FRAMES, limited, [FRAMES, limited], setup)


# One frame is built as BuildDecideFrameMap builds it: the order of each row it routes is read, then its windows are
# listed, raising decide_frame_limit, and CheckDecideFramePartition checks its partition after.
NARROW = "sum(from first to first: t.pay) over (sqrt(t.q) within t.tl)"
WIDE = "sum(from first to last: t.pay) over (sqrt(t.q) within t.tl)"
LISTED = Fault(
    "listed", _limited(WIDE, 2), clause=f"per t.cls: x <= {NARROW} + 9", refused=f"per t.cls: x <= {WIDE} + 9"
)
ROUTED = Fault("routed", SQRT, cells=(("q", 0, -1),))
CHECKED = Fault("checked", _spans("cls", NARROW), cells=(("cls", 2, "c1"),))
ONE_FRAME = [(ROUTED, LISTED), (LISTED, CHECKED)]


@pytest.mark.error
@pytest.mark.parametrize("first, after", ONE_FRAME, ids=[f"{pair[0].name}-{pair[1].name}" for pair in ONE_FRAME])
def test_one_frame_is_routed_listed_and_checked_in_that_order(decidb_cli, first, after):
    _assert_wins(decidb_cli, first, after, [LISTED], "SET decide_frame_limit = 2; ")


# RunScopeChecks reads an objective's kept rows, its `when`, before it counts the objective's instances (D72). An
# objective with no signature has no such check, and its `when` is read with the objective.
KEPT_ROWS = Fault("kept_rows", SQRT, cells=(("root", 1, -1.0),))
GATED = [(KEPT_ROWS, SCOPE_CHECK, [SCOPE_CHECK]), (FRAME_PEERS, KEPT_ROWS, [FRAME_PEERS])]


@pytest.mark.error
@pytest.mark.parametrize("winner, loser, written", GATED, ids=["with_a_signature", "without_one"])
def test_the_scope_check_reads_the_objective_when(decidb_cli, winner, loser, written):
    clauses = " AND ".join([*BOX, *(fault.clause for fault in written if fault.clause)])
    decide = f"{DECLARATION} SUCH THAT {clauses} MINIMIZE when sqrt(t.root) > 0: {_objective(written)}"
    for on in ([winner, loser], [winner], [loser]):
        assert _error(decidb_cli, decide, on) == on[0].message


@pytest.mark.error
@pytest.mark.parametrize(
    "declaration, clauses, message",
    [
        ("x(REAL), per (): s(REAL)", "s <= -2 AND x <= -1", _domain("x <= -1", "x")),
        ("per (): s(REAL), x(REAL)", "x <= -1 AND s <= -2", _domain("s <= -2", "s")),
    ],
    ids=["x_declared_first", "s_declared_first"],
)
def test_the_sink_state_reads_decisions_as_declared(decidb_cli, declaration, clauses, message):
    # The one phase whose order is not the clauses': it loops over the decisions. So the decision declared first has
    # its bound written second.
    assert _error(decidb_cli, f"{declaration} SUCH THAT {clauses} MINIMIZE sum(x)", []) == message


def _cell(name, column, row, value, message):
    return Fault(name, message, cells=((column, row, value),))


NAN_WEIGHT = _cell("nan_weight", "weight", 0, NAN, _coefficient("t.weight"))
NAN_FIXING = _cell("nan_fixing", "weight", 0, NAN, _bound("t.weight"))
NAN_GAIN = _cell("nan_gain", "gain", 0, NAN, _coefficient("t.gain"))
NAN_CAP = _cell("nan_cap", "cap", 0, NAN, _bound("t.cap"))
NAN_CAP2 = _cell("nan_cap2", "cap2", 0, NAN, _bound("t.cap2"))
FIRST_ROW_ERROR = _cell("first_row_error", "root", 0, -1.0, SQRT)
LATER_ROW_ERROR = _cell("later_row_error", "root2", 1, -1.0, LN)

ONE_CLAUSE = [
    ("when_before_coefficient", "when sqrt(t.root) > 0: t.weight * x + s <= 100", FIRST_ROW_ERROR, NAN_WEIGHT),
    ("coefficient_before_bound", "t.weight * x + s <= t.cap", NAN_WEIGHT, NAN_CAP),
    ("first_written_term", "t.weight * x + t.gain * s <= 100", NAN_WEIGHT, NAN_GAIN),
    ("first_written_term_swapped", "t.gain * s + t.weight * x <= 100", NAN_GAIN, NAN_WEIGHT),
    # One expression fails row by row: the first bad row raises, whichever operator is written first.
    (
        "first_bad_row_of_an_expression",
        "sum((ln(t.root2) + sqrt(t.root)) * x) <= 100",
        FIRST_ROW_ERROR,
        LATER_ROW_ERROR,
    ),
    # Two terms are two expressions: the first written raises, though its bad row comes later.
    (
        "first_term_before_the_first_bad_row",
        "ln(t.root2) * x + sqrt(t.root) * s <= 100",
        LATER_ROW_ERROR,
        FIRST_ROW_ERROR,
    ),
    # Two positions of two rows: a frame reads its timelines in the order of their values, not of their rows, so 'B'
    # raises though 'C' holds the first rows.
    (
        "positions_in_the_order_of_timelines",
        f"x <= {AT_FIRST} + 20",
        Fault("peer_in_b", _peers(AT_FIRST, "pos", 1, "B"), cells=(("pos", 3, 1),)),
        Fault("peer_in_c", _peers(AT_FIRST, "pos", 1, "C"), cells=(("tl", 0, "C"), ("tl", 1, "C"), ("pos", 1, 1))),
    ),
]


@pytest.mark.error
@pytest.mark.parametrize("name, clause, winner, loser", ONE_CLAUSE, ids=[case[0] for case in ONE_CLAUSE])
def test_within_one_clause(decidb_cli, name, clause, winner, loser):
    _assert_wins(decidb_cli, winner, loser, [Fault(name, "", clause=clause)])


# No cell turns this on: the clauses leave nothing feasible, so it is what remains with the other fault off.
NO_ANSWER = Fault("no_answer", INFEASIBLE)

R1 = "x(REAL) SUCH THAT x <= 4 AND sum(t.weight * x) <= sum(t.side) MAXIMIZE sum(t.gain * x)"
R1_NO_ANSWER = "x(REAL) SUCH THAT x <= 4 AND sum(t.weight * x) <= -1 MAXIMIZE sum(t.gain * x)"
A1 = "x(REAL) SUCH THAT t.weight * x <= 5 MAXIMIZE sum(t.gain * x)"
A1_NO_ANSWER = "x(REAL) SUCH THAT x <= t.cap AND x >= t.floor_at MAXIMIZE sum(t.gain * x)"
S1 = "x(BOOL) SUCH THAT sum(x) <= t.cap AND x >= t.weight MAXIMIZE sum(t.gain * x)"
S1_NO_ANSWER = "x(BOOL) SUCH THAT sum(x) <= 1 AND x >= 1 MAXIMIZE sum(t.gain * x)"

RULE_CASES = [
    ("r1_side_plan_before_constraint", R1, SIDE_PLAN, NAN_WEIGHT),
    ("r1_side_plan_before_objective", R1, SIDE_PLAN, NAN_GAIN),
    ("r1_constraint_before_objective", R1, NAN_WEIGHT, NAN_GAIN),
    ("r1_objective_before_solver", R1_NO_ANSWER, NAN_GAIN, NO_ANSWER),
    ("a1_constraint_before_objective", A1, NAN_WEIGHT, NAN_GAIN),
    ("a1_objective_before_solver", A1_NO_ANSWER, NAN_GAIN, NO_ANSWER),
    (
        "a1_first_written_bound",
        "x(REAL) SUCH THAT x <= t.cap AND x <= t.cap2 MAXIMIZE sum(t.gain * x)",
        NAN_CAP,
        NAN_CAP2,
    ),
    (
        "a1_first_written_bound_swapped",
        "x(REAL) SUCH THAT x <= t.cap2 AND x <= t.cap MAXIMIZE sum(t.gain * x)",
        NAN_CAP2,
        NAN_CAP,
    ),
    ("s1_side_plan_before_constraint", S1, NAN_CAP, NAN_FIXING),
    ("s1_side_plan_before_objective", S1, NAN_CAP, NAN_GAIN),
    ("s1_constraint_before_objective", S1, NAN_FIXING, NAN_GAIN),
    ("s1_objective_before_solver", S1_NO_ANSWER, NAN_GAIN, NO_ANSWER),
]


@pytest.mark.error
@pytest.mark.parametrize("mode", ["off", "require"])
@pytest.mark.parametrize("name, decide, winner, loser", RULE_CASES, ids=[case[0] for case in RULE_CASES])
def test_a_rule_raises_in_the_solver_order(decidb_cli, monkeypatch, name, decide, winner, loser, mode):
    # `require` refuses a forced backend, and says so if the rule does not take the query.
    monkeypatch.delenv("DECIDB_FORCE_SOLVER", raising=False)
    assert _error(decidb_cli, decide, [winner, loser], mode) == winner.message
    assert _error(decidb_cli, decide, [winner], mode) == winner.message
    assert _error(decidb_cli, decide, [loser], mode) == loser.message
