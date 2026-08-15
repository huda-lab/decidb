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
"""

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
        """, match=r"infeasible")


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
        """, match=r"infeasible")


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
