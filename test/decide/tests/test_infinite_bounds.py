"""An infinite bound means "no bound", whichever shape the query wrote it in.

``x <= 1e1000::DOUBLE`` has always solved. Move any term across the comparison and
the same bound used to be refused, because canonicalization rebuilds it as
``1e1000 - l_linenumber``, which evaluates per row and reached a finiteness guard
that rejected every non-finite value wholesale.

The two spellings describe one constraint, so they now behave as one. The rule is
the solver's, not a new DeciDB convention: a right-hand side may be infinite, and
the model builder has always accepted that and rejected only NaN. What direction
the infinity points still decides the outcome — ``<= +inf`` constrains nothing,
``>= +inf`` cannot be satisfied — and that is the solver's answer to give, not a
guard's. A bare bound is not absorbed into the column box when it is infinite, for
that reason: the box has one sentinel per direction and would have kept the
infinity in one and lost it in the other.

**The verdict is shared; the outcome is the backend's.** Both backends identify the
unreachable clause and name it in the same words — ``clause `X` sets a bound no value
can reach``. Gurobi then reports it as an infeasible query; HiGHS cannot load an
inverted row-bound pair at all and refuses in SQL terms instead, pointing at Gurobi for
the infeasibility reading. Which of the two a host gives is not a DeciDB decision, so
the assertions below stop at the shared sentence rather than pinning one host's ending.

That leaves one thing a rewrite genuinely cannot do: no finite Big-M dominates an
infinite range. But a bound is only linearized when it constrains something, so an
infinity is classified first — vacuous, unsatisfiable, or finite — and only the
finite case asks for an M. ``inf - inf`` is NaN, which is neither and still an
error rather than a bound.

Covers:
  - test_infinite_bound_survives_the_rebuilt_rhs: the bug, both spellings agree
  - test_infinite_bound_wrong_direction_is_infeasible: ``>= +inf`` has no solution
  - test_infinite_bound_is_a_tautology_for_ne: ``<> +inf`` drops, never linearizes
  - test_vacuous_minmax_bound_drops: ``MIN(x) <= +inf`` constrains nothing
  - test_unreachable_minmax_bound_is_infeasible: ``MAX(x) >= +inf`` has no solution
  - test_nan_bound_is_still_rejected: inf - inf is not a bound
  - test_infinite_value_inside_abs_is_named: the Big-M refusal names ABS, not M

A bound can also be reduced from a column instead of typed, and that path kept the
strict guard for longer: one infinite row in ``cap`` refused ``MIN(x) <= MAX(cap)``
outright, while the same bound written as a literal was accepted and classified.
The reducer's input is on the way to a model row's ``rhs`` like any other bound, so
it follows the same rule; the direction still decides the outcome, now per group.

  - test_data_reducer_bound_agrees_with_the_literal_spelling: the bug
  - test_data_reducer_bound_is_classified_per_group: mixed +inf/finite groups
  - test_data_reducer_ignores_an_infinity_it_never_reads: masked rows are not judged
  - test_data_reducer_unreachable_bound_is_infeasible: the solver gives the verdict
  - test_nan_from_reducer_arithmetic_is_still_rejected: inf + -inf is still not a bound

The two backends do not agree on an unreachable bound, and cannot. Gurobi loads the row
and reports the infeasibility, which is the answer this module is built around. HiGHS
spells a one-sided row bound by pairing the user's bound with its own ±1e30 infinity
sentinel, so `Ax >= +inf` becomes `lower = +inf, upper = 1e30` — an inverted pair it
rejects at model load. DeciDB refuses it in SQL terms rather than letting that surface as
an internal error that invalidates the connection. Only the unreachable direction differs;
a vacuous infinity pairs cleanly with the sentinel and solves on both.

  - test_highs_refuses_an_unreachable_bound_in_sql_terms: named clause, live connection
  - test_highs_still_solves_a_vacuous_infinity: the vacuous direction is unaffected
"""

import csv
import io
import re

import pytest

from solver.types import VarType, ObjSense, SolverStatus


@pytest.mark.cons_perrow
@pytest.mark.cons_comparison
@pytest.mark.edge_case
@pytest.mark.correctness
def test_infinite_bound_survives_the_rebuilt_rhs(decidb_cli, duckdb_conn, oracle_solver):
    """``x + l_linenumber <= 1e1000`` is the same non-constraint as ``x <= 1e1000``.

    Both spellings are run because a fix that merely stopped erroring could still
    have built a different model — dropping the row, or clamping the bound to a
    finite stand-in that silently caps ``x``. The oracle then pins the shared
    answer: with the infinite bound carrying no information, the only thing
    holding ``x`` down is ``x <= 6``.
    """
    common = """
        SELECT l_orderkey, l_linenumber, x
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE x(INT)
        SUCH THAT x <= 6 AND {bound}
        MAXIMIZE SUM(x)
    """
    bare, bare_cols = decidb_cli.execute(
        common.format(bound="x <= 1e1000::DOUBLE"))
    rebuilt, rebuilt_cols = decidb_cli.execute(
        common.format(bound="x + l_linenumber <= 1e1000::DOUBLE"))

    assert bare_cols == rebuilt_cols
    xi = bare_cols.index("x")
    bare_obj = sum(int(r[xi]) for r in bare)
    rebuilt_obj = sum(int(r[xi]) for r in rebuilt)
    assert bare_obj == rebuilt_obj, \
        f"rebuilt bound disagrees with the bare one: {rebuilt_obj} vs {bare_obj}"

    n = duckdb_conn.execute(
        "SELECT COUNT(*) FROM lineitem WHERE l_orderkey <= 3").fetchone()[0]
    assert n > 1 and len(rebuilt) == n

    oracle_solver.create_model("infinite_bound_is_no_bound")
    obj = {}
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=6.0)
        obj[f"x_{i}"] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert abs(rebuilt_obj - result.objective_value) < 1e-6, \
        f"Objective mismatch: DecidB={rebuilt_obj}, Oracle={result.objective_value}"


@pytest.mark.cons_perrow
@pytest.mark.error_infeasible
@pytest.mark.error
def test_infinite_bound_wrong_direction_is_infeasible(decidb_cli):
    """``>= +inf`` is not an invalid query, it is one with no solution.

    Nothing finite reaches an infinite floor, so this is a genuine infeasibility
    and is reported as one — the same diagnosis the aggregate spelling
    (``SUM(x) >= 1e1000``) has always produced.

    The bare spelling is checked with the rebuilt one because it takes a different
    route: a bare ``x >= K`` is absorbed into the column box instead of becoming a
    model row. The box clamps an upper bound to a large sentinel but had no such
    floor for a lower one, so ``+inf`` was written straight into it and the model
    validator raised an internal error — which also invalidated the connection, so
    every later query in the session failed too.
    """
    for bound in ("x + l_linenumber >= 1e1000::DOUBLE", "x >= 1e1000::DOUBLE"):
        decidb_cli.assert_error(f"""
            SELECT l_orderkey, x
            FROM lineitem WHERE l_orderkey <= 3
            DECIDE x(INT)
            SUCH THAT x <= 6 AND {bound}
            MAXIMIZE SUM(x)
        """, match=r"sets a bound no value can reach")


@pytest.mark.cons_perrow
@pytest.mark.cons_comparison
@pytest.mark.edge_case
def test_infinite_bound_is_a_tautology_for_ne(decidb_cli):
    """``<> +inf`` is satisfied by every value, so no Big-M is ever needed.

    The ``<>`` rewrite drops rows whose bound no integer can equal, and an
    infinite bound is one of those. The drop has to happen before the Big-M
    constant is computed, or a constraint that constrains nothing would be
    refused for lacking a finite ``M``. Both spellings are checked: the aggregate
    one wrote the same test inline as ``abs(rhs - round(rhs)) >= 1e-9``, which is
    false for the NaN that infinity produces, so it fell through the drop.
    """
    common = """
        SELECT l_orderkey, x
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE x(INT)
        SUCH THAT x <= 6 AND {constraint}
        MAXIMIZE SUM(x)
    """
    for constraint in ("x + l_linenumber <> 1e1000::DOUBLE",
                       "SUM(x) <> 1e1000::DOUBLE"):
        rows, cols = decidb_cli.execute(common.format(constraint=constraint))
        xi = cols.index("x")
        assert all(int(r[xi]) == 6 for r in rows), \
            f"a vacuous <> bound restricted the model: {constraint}"


@pytest.mark.min_max
@pytest.mark.edge_case
@pytest.mark.correctness
def test_vacuous_minmax_bound_drops(decidb_cli, oracle_solver):
    """``MIN(x) <= +inf`` and ``MAX(x) >= -inf`` hold for every assignment.

    The easy direction of a MIN/MAX constraint becomes a per-row bound and has
    always accepted an infinity. The hard direction is linearized with an indicator
    and a Big-M, and was refused for lacking a finite M — even here, where the bound
    rules out nothing and there is nothing to linearize. Classifying the bound first
    drops it instead, exactly as the ``<>`` rewrite drops a bound no integer can
    equal.

    Dropping has to leave the rest of the model alone, so the objective is compared
    against the same query without the constraint and then pinned by the oracle. The
    PER spelling is included because the verdict is reached per group.
    """
    common = """
        SELECT l_orderkey, x
        FROM lineitem WHERE l_orderkey <= 3
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 6{extra}
        MAXIMIZE SUM(x)
    """

    def solve(extra):
        rows, cols = decidb_cli.execute(common.format(extra=extra))
        xi = cols.index("x")
        return sum(int(r[xi]) for r in rows), len(rows)

    unconstrained, n = solve("")
    assert n > 1
    for extra in (" AND MIN(x) <= 1e1000::DOUBLE",
                  " AND MAX(x) >= -1e1000",
                  " AND MIN(x) <= 1e1000::DOUBLE PER l_orderkey"):
        assert solve(extra) == (unconstrained, n), \
            f"a vacuous bound restricted the model: {extra}"

    oracle_solver.create_model("vacuous_minmax_bound")
    obj = {}
    for i in range(n):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=6.0)
        obj[f"x_{i}"] = 1.0
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    assert abs(unconstrained - result.objective_value) < 1e-6, \
        f"Objective mismatch: DecidB={unconstrained}, Oracle={result.objective_value}"


@pytest.mark.min_max
@pytest.mark.error_infeasible
@pytest.mark.error
def test_unreachable_minmax_bound_is_infeasible(decidb_cli):
    """``MAX(x) >= +inf`` is the mirror image: no assignment reaches it.

    Nothing is linearized here either — with the bound out of every row's reach, the
    disjunction has no case left that could hold — so the constraint is reported as
    an infeasibility, the same answer the aggregate spelling ``SUM(x) >= 1e1000``
    gives. ``MAX(x) = +inf`` is included because equality splits into a per-row easy
    half and this hard half, and the easy half is satisfiable on its own.
    """
    for constraint in ("MAX(x) >= 1e1000::DOUBLE",
                       "MIN(x) <= -1e1000",
                       "MAX(x) = 1e1000::DOUBLE"):
        decidb_cli.assert_error(f"""
            SELECT l_orderkey, x
            FROM lineitem WHERE l_orderkey <= 3
            DECIDE x(INT)
            SUCH THAT x >= 0 AND x <= 6 AND {constraint}
            MAXIMIZE SUM(x)
        """, match=r"sets a bound no value can reach")


@pytest.mark.cons_perrow
@pytest.mark.edge_case
@pytest.mark.error
def test_nan_bound_is_still_rejected(decidb_cli):
    """``inf - inf`` is NaN, which is not a bound in either direction.

    Relaxing the guard to admit infinities must not admit NaN with them: the
    solver has no reading for it, and it usually means the arithmetic went wrong.
    """
    decidb_cli.assert_error("""
        WITH data AS (
            SELECT 1 AS id, 1e1000::DOUBLE AS big UNION ALL
            SELECT 2, 1e1000::DOUBLE
        )
        SELECT id, x FROM data
        DECIDE x(REAL)
        SUCH THAT x <= 5 AND x + big <= big
        MAXIMIZE SUM(x)
    """, match=r"NaN")


@pytest.mark.cons_perrow
@pytest.mark.edge_case
@pytest.mark.error
def test_infinite_value_inside_abs_is_named(decidb_cli):
    """ABS needs a finite range, and the refusal points at the data, not at M.

    ``ABS`` is linearized with a sign indicator whose constant must dominate the
    expression's range; infinity has no such constant. The user never wrote an
    ``M``, so the message names ``ABS()`` and the row instead.
    """
    decidb_cli.assert_error("""
        WITH data AS (
            SELECT 1 AS id, 1e1000::DOUBLE AS big UNION ALL
            SELECT 2, 1e1000::DOUBLE
        )
        SELECT id, x FROM data
        DECIDE x(REAL)
        SUCH THAT x <= 5
        MAXIMIZE SUM(ABS(x - big))
    """, match=r"ABS\(\).*Infinity")


# ---------------------------------------------------------------------------
# The bound built from data, rather than typed as a literal
# ---------------------------------------------------------------------------
#
# Everything above writes the infinity as a literal the user typed. A bound can
# also be *reduced from a column* — ``MIN(x) <= MAX(cap) PER g`` — and that path
# read its input through the strict finiteness guard, so one infinite row in
# ``cap`` refused the whole query. The two spellings describe the same bound, so
# they get the same answer: the reducer's input is on the way to a model row's
# ``rhs``, and ±inf is a value there. What the infinity *means* is still decided
# downstream, per group, by the direction it points.

_MIXED = """
    WITH data AS (
        SELECT 0 AS g, 1e1000::DOUBLE AS cap UNION ALL
        SELECT 0, 2.0 UNION ALL
        SELECT 1, 3.0 UNION ALL
        SELECT 1, 1.0
    )
    SELECT g, x FROM data
    DECIDE x(INT)
    SUCH THAT x >= 0 AND x <= 6{extra}
    MAXIMIZE SUM(x)
"""


def _by_group(rows, cols):
    """``{g: sorted([x, ...])}``. Row order out of a CTE is not stable."""
    gi, xi = cols.index("g"), cols.index("x")
    out: dict[int, list[int]] = {}
    for r in rows:
        out.setdefault(int(r[gi]), []).append(int(r[xi]))
    return {g: sorted(v) for g, v in out.items()}


@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.edge_case
def test_data_reducer_bound_agrees_with_the_literal_spelling(decidb_cli):
    """``MAX(cap)`` folding to +inf is the same bound as a typed ``1e1000``.

    This is the bug at its smallest: the reducer's input was extracted under the
    strict guard, so the query died at the first infinite row — before grouping,
    before folding, before anything knew which group that row belonged to. The
    literal spelling was accepted and classified. Both are run over the same data
    so a divergence can only come from the spelling.
    """
    data = """
        WITH data AS (
            SELECT 0 AS g, 1e1000::DOUBLE AS cap UNION ALL
            SELECT 0, 1e1000::DOUBLE UNION ALL
            SELECT 1, 1e1000::DOUBLE UNION ALL
            SELECT 1, 1e1000::DOUBLE
        )
        SELECT g, x FROM data
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 6 AND MIN(x) <= {bound} PER g
        MAXIMIZE SUM(x)
    """
    reduced = _by_group(*decidb_cli.execute(data.format(bound="MAX(cap)")))
    literal = _by_group(*decidb_cli.execute(data.format(bound="1e1000::DOUBLE")))
    assert reduced == literal, \
        f"data reducer and literal disagree: {reduced} vs {literal}"
    # Both vacuous, so nothing but `x <= 6` is left holding x down.
    assert reduced == {0: [6, 6], 1: [6, 6]}


@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.edge_case
@pytest.mark.correctness
def test_data_reducer_bound_is_classified_per_group(decidb_cli, oracle_solver):
    """One group's bound folds to +inf, the other's to a finite 3.0.

    The per-group verdict was previously unreachable from SQL: a uniform literal
    gave every group the same kind, and the mixed case needed a data reducer,
    which was refused. So the group that reduces to +inf must drop while the
    group beside it still linearizes — the halves are checked independently and
    then the whole model is pinned against the oracle, which encodes the
    surviving group's "some row is at or below the bound" with native indicator
    constraints rather than a Big-M of our own.
    """
    mixed = _by_group(*decidb_cli.execute(
        _MIXED.format(extra=" AND MIN(x) <= MAX(cap) PER g")))
    unconstrained = _by_group(*decidb_cli.execute(_MIXED.format(extra="")))

    # g=0 reduces to MAX(inf, 2.0) = +inf: vacuous, so it matches the model with
    # the constraint absent. g=1 reduces to MAX(3.0, 1.0) = 3.0 and still binds.
    assert mixed[0] == unconstrained[0] == [6, 6], \
        f"the vacuous group was constrained anyway: {mixed[0]}"
    assert mixed[1] == [3, 6], \
        f"the finite group's disjunction did not bind: {mixed[1]}"

    oracle_solver.create_model("data_reducer_mixed_groups")
    obj = {}
    for i in range(4):
        oracle_solver.add_variable(f"x_{i}", VarType.INTEGER, lb=0.0, ub=6.0)
        obj[f"x_{i}"] = 1.0
    # Only g=1 (rows 2 and 3) carries a bound: at least one of them is <= 3.
    ys = []
    for i in (2, 3):
        y = f"y_{i}"
        oracle_solver.add_variable(y, VarType.BINARY)
        oracle_solver.add_indicator_constraint(
            y, 1, {f"x_{i}": 1.0}, "<=", 3.0, name=f"binds_{i}")
        ys.append(y)
    oracle_solver.add_constraint({y: 1.0 for y in ys}, ">=", 1.0, name="some_row")
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL
    total = sum(sum(v) for v in mixed.values())
    assert abs(total - result.objective_value) < 1e-6, \
        f"Objective mismatch: DecidB={total}, Oracle={result.objective_value}"


@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.when_constraint
@pytest.mark.edge_case
def test_data_reducer_ignores_an_infinity_it_never_reads(decidb_cli):
    """A row the reducer's own ``WHEN`` excludes cannot poison the bound.

    The guard ran during extraction, ahead of the ``WHEN`` mask and the
    relation-qualified dedup, so it judged rows that go on to contribute to
    nothing. Here the only infinite ``cap`` sits in a ``WHEN``-false row: the
    bound for g=0 is ``MAX(2.0) = 2.0``, and the excluded row is not part of the
    ``MIN(x)`` disjunction either, so it floats to 6.
    """
    rows, cols = decidb_cli.execute("""
        WITH data AS (
            SELECT 0 AS g, 1e1000::DOUBLE AS cap, false AS ok UNION ALL
            SELECT 0, 2.0, true UNION ALL
            SELECT 1, 3.0, true UNION ALL
            SELECT 1, 1.0, true
        )
        SELECT g, x FROM data
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 6 AND MIN(x) <= MAX(cap) WHEN ok PER g
        MAXIMIZE SUM(x)
    """)
    assert _by_group(rows, cols) == {0: [2, 6], 1: [3, 6]}


@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.error_infeasible
@pytest.mark.error
def test_data_reducer_unreachable_bound_is_infeasible(decidb_cli):
    """A reduced bound pointing out of reach is the solver's verdict to give.

    ``MIN(x) <= -inf`` holds for no assignment, and the answer is an
    infeasibility naming the query rather than an extraction guard refusing the
    data. Admitting the infinity is what lets it get that far: the constraint is
    emitted per row carrying the unreachable bound, exactly as the literal
    spelling already was.
    """
    decidb_cli.assert_error("""
        WITH data AS (
            SELECT 0 AS g, -1e1000::DOUBLE AS cap UNION ALL
            SELECT 0, -1e1000::DOUBLE UNION ALL
            SELECT 1, 3.0 UNION ALL
            SELECT 1, 1.0
        )
        SELECT g, x FROM data
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 6 AND MIN(x) <= MAX(cap) PER g
        MAXIMIZE SUM(x)
    """, match=r"`MIN\(x\) <= -inf PER g` sets a bound no value can reach")


@pytest.mark.min_max
@pytest.mark.per_clause
@pytest.mark.edge_case
@pytest.mark.error
def test_nan_from_reducer_arithmetic_is_still_rejected(decidb_cli):
    """Relaxing the reducer's guard must not let NaN through with the infinities.

    ``MAX(cap) + MIN(cap)`` over a group holding both signs is ``inf + -inf``.
    The reducer now hands both values on, and the per-row extraction that reads
    the combined bound back is where the NaN is caught — so the relaxation costs
    nothing here.
    """
    decidb_cli.assert_error("""
        WITH data AS (
            SELECT 0 AS g, 1e1000::DOUBLE AS cap UNION ALL
            SELECT 0, -1e1000::DOUBLE UNION ALL
            SELECT 1, 3.0 UNION ALL
            SELECT 1, 1.0
        )
        SELECT g, x FROM data
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 6 AND MIN(x) <= MAX(cap) + MIN(cap) PER g
        MAXIMIZE SUM(x)
    """, match=r"NaN")


@pytest.mark.cons_perrow
@pytest.mark.min_max
@pytest.mark.edge_case
@pytest.mark.error
@pytest.mark.parametrize("constraint,clause", [
    ("x >= 1e1000::DOUBLE", "x >= inf"),
    ("SUM(x) >= 1e1000::DOUBLE", "SUM(x) >= inf"),
    ("SUM(x) <= -1e1000::DOUBLE", "SUM(x) <= -inf"),
    ("SUM(x) = 1e1000::DOUBLE", "SUM(x) = inf"),
    ("MIN(x) <= -1e1000::DOUBLE", "MIN(x) <= -inf"),
    ("MAX(x) >= 1e1000::DOUBLE", "MAX(x) >= inf"),
])
def test_highs_refuses_an_unreachable_bound_in_sql_terms(
    decidb_cli_highs, constraint, clause
):
    """HiGHS cannot load the row, and says so as a SQL error naming the clause.

    The row it cannot load is exactly the unreachable one: HiGHS pairs a one-sided
    bound with its own 1e30 infinity sentinel, so `Ax >= +inf` arrives as
    `lower = +inf, upper = 1e30` and is rejected as an inverted pair. Left to the
    backend that surfaced as `INTERNAL Error: Failed to pass model to HiGHS`, which
    invalidated the connection and forced a restart — a solver-internals error for a
    question the user asked in SQL. It is now caught before the model is passed.
    """
    decidb_cli_highs.assert_error(f"""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 6 AND {constraint}
        MAXIMIZE SUM(x)
    """, match=rf"`{re.escape(clause)}` sets a bound no value can reach")


@pytest.mark.cons_perrow
@pytest.mark.edge_case
@pytest.mark.error
def test_highs_refusal_leaves_the_connection_usable(decidb_cli_highs):
    """The refusal is an ordinary SQL error, not a fatal one.

    This is the half that mattered: the old internal error left the database needing a
    restart, so one infinite bound cost the user every later statement in the session.
    """
    result = decidb_cli_highs.execute_script(
        ".mode csv\n"
        "SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT) "
        "SUCH THAT x >= 0 AND x <= 6 AND SUM(x) >= 1e1000::DOUBLE MAXIMIZE SUM(x);\n"
        "SELECT id, x FROM (VALUES (1), (2)) t(id) DECIDE x(INT) "
        "SUCH THAT x >= 0 AND x <= 6 MAXIMIZE SUM(x);\n"
    )
    assert "INTERNAL Error" not in result.stderr, result.stderr
    assert "restarted" not in result.stderr, result.stderr
    # The second DECIDE ran on the same connection and solved.
    rows = list(csv.DictReader(io.StringIO(result.stdout)))
    assert [r["x"] for r in rows] == ["6", "6"], result.stdout


@pytest.mark.cons_perrow
@pytest.mark.edge_case
def test_highs_still_solves_a_vacuous_infinity(decidb_cli_highs):
    """`x <= +inf` points the other way and must keep solving under HiGHS.

    The guard keys on the direction the infinity points, not on the infinity, so the
    vacuous spelling never reaches it — the same rule the rewrite applies upstream. A
    guard that refused every infinite bound would break a query that constrains nothing.
    """
    rows, cols = decidb_cli_highs.execute("""
        SELECT id, x FROM (VALUES (1), (2)) t(id)
        DECIDE x(INT)
        SUCH THAT x >= 0 AND x <= 1e1000::DOUBLE AND SUM(x) <= 7
        MAXIMIZE SUM(x)
    """)
    xi = cols.index("x")
    assert sum(int(r[xi]) for r in rows) == 7
