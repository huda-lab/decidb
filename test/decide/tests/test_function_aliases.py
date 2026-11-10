"""One spelling per function: a name DuckDB registers as an alias answers as its primary does.

DuckDB registers `pow`, `power` and `^` for `**`, `abs` for `@`, `add`, `subtract` and `multiply` for `+`, `-` and
`*`, and `mean` for `avg`. At the start of binding every call of a clause's algebra whose name resolves to such an alias
is renamed to its primary (`RespellDecideAliases`), so every check from then on matches one name per function, and it
keeps the name it was written under as a display tag, so a plan and a message read as the user wrote them.

Each test states the property, and two spellings of one problem are compared with each other, with the model they build,
and (for the direct-solve rule that reads the names in data) with the oracle.
"""

import pytest

from decidb_cli import DeciDBCliError
from solver.types import ObjSense

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)
from ._direct_oracle_common import agree_with_oracle
from .test_direct_o1_monge_assignment import _abs, _case, _check_assignment, _oracle_model, _sql, _squared

_T = "(VALUES (1, 1.5, 3), (2, 2.5, 5), (3, 3.5, 4), (4, 4.5, 6)) t(id, v, w)"

_SOLVER_PATH = "SET decide_direct_solve='off'; "


def _rows(cli, sql):
    rows, columns = cli.execute(sql)
    return [dict(zip(columns, row)) for row in rows]


def _model(cli, sql, tmp_path):
    return cli.dump_model(sql, tmp_path / "model.txt")


def _plan(cli, sql):
    out = cli.execute_raw(f"{_SOLVER_PATH}EXPLAIN {sql}").stdout
    box = "│┌┐└┘├┤┬┴┼─"
    return "".join(ch for ch in out if ch not in box and not ch.isspace())


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "objective,cost",
    [
        ("MINIMIZE SUM((l.lpos - r.rpos) ** 2 * x)", _squared),
        ("MINIMIZE SUM((l.lpos - r.rpos) ^ 2 * x)", _squared),
        ("MINIMIZE SUM(pow(l.lpos - r.rpos, 2) * x)", _squared),
        ("MINIMIZE SUM((@(l.lpos - r.rpos)) * x)", _abs),
    ],
    ids=["double_star", "caret", "pow", "at_sign"],
)
def test_alias_o1_monge_reads_a_square_or_an_absolute_value_under_any_spelling(
    decidb_cli, oracle_solver, objective, cost
):
    """O1 answers a cost written with any name of its function, on `require` as on the solver path."""
    case = _case(objective, cost)
    oracle = _oracle_model(oracle_solver, case)
    agree_with_oracle(
        decidb_cli,
        _sql(case),
        oracle,
        lambda rows, columns, mode: _check_assignment(case, oracle, rows, columns, mode),
        modes=("off", "require"),
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("alias", ["@", "abs", "ABS"])
def test_alias_abs_over_a_decision_builds_the_model_of_abs(decidb_cli, tmp_path, alias):
    """`@(x - w)` and `abs(x - w)` are accepted over a decision and build what `ABS(x - w)` builds."""
    sql = f"SELECT id, x FROM {_T} DECIDE x(REAL) SUCH THAT x <= 4 MINIMIZE SUM({alias}(x - w))"
    reference = f"SELECT id, x FROM {_T} DECIDE x(REAL) SUCH THAT x <= 4 MINIMIZE SUM(ABS(x - w))"
    assert _rows(decidb_cli, f"{sql} ORDER BY id") == _rows(decidb_cli, f"{reference} ORDER BY id")
    assert _model(decidb_cli, sql, tmp_path) == _model(decidb_cli, reference, tmp_path)


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "written,primary",
    [
        ("multiply(x, w)", "x * w"),
        ("add(x * w, 1)", "x * w + 1"),
        ("subtract(x * w, 1)", "x * w - 1"),
    ],
    ids=["multiply", "add", "subtract"],
)
def test_alias_arithmetic_over_a_decision_builds_the_model_of_its_operator(decidb_cli, tmp_path, written, primary):
    sql = "SELECT id, x FROM {t} DECIDE x(BOOL) SUCH THAT SUM({e}) <= 7 MAXIMIZE SUM(x * v)"
    assert _rows(decidb_cli, sql.format(t=_T, e=written) + " ORDER BY id") == _rows(
        decidb_cli, sql.format(t=_T, e=primary) + " ORDER BY id"
    )
    assert _model(decidb_cli, sql.format(t=_T, e=written), tmp_path) == _model(
        decidb_cli, sql.format(t=_T, e=primary), tmp_path
    )


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_alias_pow_and_power_in_one_product_are_a_square(decidb_cli_highs, tmp_path):
    """`pow(v, 2)` and `power(v, 2)` are one function, so their product of two equal factors is a square."""
    mixed = f"SELECT id, x FROM {_T} DECIDE x(REAL) SUCH THAT x <= 30 MINIMIZE SUM((x - pow(v, 2)) * (x - power(v, 2)))"
    twice = f"SELECT id, x FROM {_T} DECIDE x(REAL) SUCH THAT x <= 30 MINIMIZE SUM((x - pow(v, 2)) * (x - pow(v, 2)))"
    assert _rows(decidb_cli_highs, f"{mixed} ORDER BY id") == _rows(decidb_cli_highs, f"{twice} ORDER BY id")
    assert _model(decidb_cli_highs, mixed, tmp_path) == _model(decidb_cli_highs, twice, tmp_path)


@pytest.mark.var_integer
@pytest.mark.cons_comparison
@pytest.mark.correctness
@pytest.mark.parametrize(
    "written,primary",
    [("add(x, 0.5) < 3", "x + 0.5 < 3"), ("subtract(x, 0.5) > 1", "x - 0.5 > 1"), ("pow(x, 2) < 10", "x ** 2 < 10")],
    ids=["add", "subtract", "pow"],
)
def test_alias_whole_number_check_of_a_strict_comparison_reads_the_primary(decidb_cli, written, primary):
    """The binder's whole-number check for `<` and `>` reads `add(x, 0.5)` as `x + 0.5`."""
    sql = "SELECT id, x FROM {t} DECIDE x(INT) SUCH THAT {c} AND x <= 9 MAXIMIZE SUM(x)"
    assert _rows(decidb_cli, sql.format(t=_T, c=written)) == _rows(decidb_cli, sql.format(t=_T, c=primary))


@pytest.mark.var_integer
@pytest.mark.cons_comparison
@pytest.mark.error
@pytest.mark.correctness
def test_alias_whole_number_check_refuses_a_fractional_factor_under_an_alias(decidb_cli):
    """`multiply(x, v)` is refused as `x * v` is, with the same message."""
    refused = "SELECT id, x FROM {t} DECIDE x(INT) SUCH THAT {c} < 3 MAXIMIZE SUM(x)"
    messages = []
    for written in ("x * v", "multiply(x, v)"):
        with pytest.raises(DeciDBCliError) as error:
            decidb_cli.execute(refused.format(t=_T, c=written))
        messages.append(str(error.value).split("\n\n")[0])
    assert "allows fractional values" in messages[0]
    assert messages[0] == messages[1]


@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
@pytest.mark.parametrize("written,primary", [("multiply(SUM(x), 2)", "SUM(x) * 2"), ("add(SUM(x), 3)", "SUM(x) + 3")])
def test_alias_objective_composition_of_a_reducer_reads_the_primary(decidb_cli, written, primary):
    """The objective binder lets a reducer sit under `+`, `-` and `*`, and `multiply`, `add` and `subtract` are those."""
    sql = "SELECT id, x FROM {t} DECIDE x(INT) SUCH THAT x >= 1 AND x <= 4 AND SUM(x) >= 6 MINIMIZE {o}"
    assert _rows(decidb_cli, sql.format(t=_T, o=written) + " ORDER BY id") == _rows(
        decidb_cli, sql.format(t=_T, o=primary) + " ORDER BY id"
    )


@pytest.mark.var_integer
@pytest.mark.error
@pytest.mark.correctness
def test_alias_data_aggregate_factor_is_refused_as_its_operator_is(decidb_cli):
    """`multiply(avg(v), x)` is refused as `avg(v) * x` is: the data aggregate would be spread over the rows."""
    sql = "SELECT id, x FROM {t} DECIDE x(INT) SUCH THAT x <= 4 MAXIMIZE SUM({e})"
    with pytest.raises(DeciDBCliError) as primary:
        decidb_cli.execute(sql.format(t=_T, e="avg(v) * x"))
    with pytest.raises(DeciDBCliError) as alias:
        decidb_cli.execute(sql.format(t=_T, e="multiply(avg(v), x)"))
    assert "cannot multiply a decision variable" in str(primary.value)
    assert str(alias.value) == str(primary.value)


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_alias_mean_answers_as_avg(decidb_cli, tmp_path):
    constraint = "SELECT id, x FROM {t} DECIDE x(INT) SUCH THAT x <= 6 AND {r}(x) >= 3 MINIMIZE SUM(x)"
    objective = "SELECT id, x FROM {t} DECIDE x(INT) SUCH THAT x <= 6 AND SUM(x) <= 9 MAXIMIZE {r}(x)"
    for sql in (constraint, objective):
        assert _rows(decidb_cli, sql.format(t=_T, r="mean") + " ORDER BY id") == _rows(
            decidb_cli, sql.format(t=_T, r="avg") + " ORDER BY id"
        )
        assert _model(decidb_cli, sql.format(t=_T, r="mean"), tmp_path) == _model(
            decidb_cli, sql.format(t=_T, r="avg"), tmp_path
        )


@pytest.mark.explain
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize(
    "written,reprinted",
    [("abs(x - v)", "↳abs(x-v)becomes"), ("ABS(x - v)", "↳abs(x-v)becomes"), ("@(x - v)", "↳@(x-v)becomes")],
)
def test_alias_explain_shows_the_name_as_written(decidb_cli, written, reprinted):
    """The plan reprints the decision structure of `abs(x - v)` from the bound tree, under the name written."""
    plan = _plan(decidb_cli, f"SELECT id, x FROM {_T} DECIDE x(REAL) SUCH THAT x <= 10 MINIMIZE SUM({written})")
    assert reprinted in plan, plan
    assert ("@" in plan) == (written.startswith("@")), plan


@pytest.mark.explain
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("written", ["abs", "ABS"])
def test_alias_explain_canonical_form_reprints_a_call_as_written(decidb_cli, written):
    """The canonical objective moves the coefficient left and reprints the call from the bound tree."""
    plan = _plan(
        decidb_cli, f"SELECT id, x FROM {_T} DECIDE x(REAL) SUCH THAT x <= 10 MINIMIZE SUM({written}(x - v) * w)"
    )
    assert "≡sum(w*abs(x-v))" in plan and "@" not in plan, plan


@pytest.mark.explain
@pytest.mark.cons_aggregate
@pytest.mark.correctness
@pytest.mark.parametrize("written", ["pow(x - v, 2)", "power(x - v, 2)", "(x - v) ^ 2", "(x - v) ** 2"])
def test_alias_explain_shows_a_power_as_written(decidb_cli, written):
    plan = _plan(decidb_cli, f"SELECT id, x FROM {_T} DECIDE x(REAL) SUCH THAT x <= 10 MINIMIZE SUM({written} * w)")
    assert "MINIMIZESUM(" + "".join(written.split()) + "*w)" in plan, plan
    # The canonical product spelling is `power`, as it is for a square the user wrote as a product.
    assert "sum(w*power(x-v,2))" in plan, plan


@pytest.mark.explain
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_alias_explain_shows_a_name_in_data_and_a_reducer_as_written(decidb_cli):
    """DuckDB prints a call in data, under a cast, and a reducer, by their names: those read as written too."""
    plan = _plan(
        decidb_cli,
        f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= 6 AND mean(x) >= 2 MINIMIZE SUM(x * abs(v - 3))",
    )
    assert "abs(v-3)" in plan and "mean(x)>=2" in plan, plan
    assert "@" not in plan and "avg(" not in plan and "__written_name_" not in plan, plan


@pytest.mark.error
@pytest.mark.correctness
@pytest.mark.parametrize(
    "written,fragment",
    [
        ("mod(x, 2) <= 1", "function 'mod' cannot take a DECIDE variable"),
        ("x % 2 <= 1", "operator '%' cannot take a DECIDE variable"),
        ("ceiling(x) <= 3", "function 'ceiling' cannot take a DECIDE variable"),
        ("SUM(divide(x * w, 2)) <= 3", "function 'divide' cannot take a DECIDE variable"),
    ],
    ids=["mod", "percent", "ceiling", "divide_in_a_reducer"],
)
def test_alias_a_refusal_names_the_function_as_written(decidb_cli, written, fragment):
    with pytest.raises(DeciDBCliError) as error:
        decidb_cli.execute(f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT {written} MAXIMIZE SUM(x)")
    assert fragment in str(error.value)


@pytest.mark.error
@pytest.mark.correctness
def test_alias_a_reducer_refusal_names_the_reducer_as_written(decidb_cli):
    with pytest.raises(DeciDBCliError) as error:
        decidb_cli.execute(f"SELECT id, x FROM {_T} DECIDE x(INT) SUCH THAT x <= 4 MAXIMIZE mean(x, 2)")
    assert "MEAN needs one argument" in str(error.value)


@pytest.mark.var_real
@pytest.mark.correctness
def test_alias_a_name_that_resolves_to_a_user_function_is_not_respelled(decidb_cli):
    """A user's macro of the name of an alias is the function the binder calls, and is left as written."""
    query = (
        f"SELECT id, x FROM {_T} DECIDE x(REAL) SUCH THAT x <= 10 AND SUM(x * {{e}}) <= 30 MAXIMIZE SUM(x) ORDER BY id"
    )
    # The serializer's round trip replays the CREATE against the read-only database, so this script runs without it.
    with_macro = _rows(
        decidb_cli.without_verify_serializer(),
        "CREATE OR REPLACE TEMP MACRO pow(a, b) AS a + b; " + query.format(e="pow(w, 2)"),
    )
    assert with_macro == _rows(decidb_cli, query.format(e="(w + 2)"))
    assert with_macro != _rows(decidb_cli, query.format(e="power(w, 2)"))


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_alias_mean_and_avg_of_one_body_share_a_helper(decidb_cli, tmp_path):
    """Two clauses that read `AVG(POWER(x, 2))`, one written `mean`, hold the body in one helper decision.

    The name a reducer was written under is display only, so it is not part of the reducer's identity."""
    sql = (
        f"SELECT id, x FROM {_T} DECIDE x(REAL) SUCH THAT x <= 5 AND x >= 0 AND "
        "MAX(POWER(x, 2)) + AVG(POWER(x, 2)) <= 30 AND MAX(POWER(x, 2)) + {r}(POWER(x, 2)) <= 40 MAXIMIZE SUM(x)"
    )
    columns = []
    for reducer in ("avg", "mean"):
        dump = decidb_cli.dump_model(sql.format(r=reducer), tmp_path / f"{reducer}.txt")
        columns.append(next(line for line in dump.splitlines() if line.startswith("num_vars:")))
    assert columns[0] == columns[1]


@pytest.mark.explain
@pytest.mark.var_real
@pytest.mark.correctness
@pytest.mark.parametrize("second", ["@(x - v)", "ABS(x - v)", "abs(x - v)"])
def test_alias_two_names_of_one_call_over_a_decision_are_one_square(decidb_cli, second):
    """`abs(x - v) * @(x - v)` is the square of one factor: the names the two calls were written under are display."""
    plan = _plan(
        decidb_cli, f"SELECT id, x FROM {_T} DECIDE x(REAL) SUCH THAT x <= 10 MINIMIZE SUM(abs(x - v) * {second})"
    )
    assert "≡sum(power(abs(x-v),2))" in plan, plan


@pytest.mark.var_real
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_alias_a_when_condition_is_respelled_and_shares_its_helper(decidb_cli, tmp_path):
    """A `when` condition is respelled like the rest of the clause.

    Two reducers whose conditions are `pow(b, 2) > 1` and `power(b, 2) > 1` are one reducer to the helper sharing, so
    the pair builds the model of two `pow`s and answers as it does."""
    data = "(VALUES (1, 2.5, 1), (2, 3.5, 2), (3, 4.5, 3)) t(id, w, b)"
    sql = (
        f"SELECT id, x FROM {data} DECIDE x(REAL) SUCH THAT x <= 5 AND x <= 0.5 * SUM(when {{a}}(b, 2) > 1: x) "
        "AND x >= 0.1 * SUM(when {b}(b, 2) > 1: x) MAXIMIZE SUM(x)"
    )
    columns = {}
    answers = {}
    for second in ("pow", "power"):
        query = sql.format(a="pow", b=second)
        answers[second] = _rows(decidb_cli, f"{query} ORDER BY id")
        dump = decidb_cli.dump_model(query, tmp_path / f"{second}.txt")
        columns[second] = int(next(line for line in dump.splitlines() if line.startswith("num_vars:")).split(":")[1])
    assert answers["pow"] == answers["power"]
    assert columns["power"] == columns["pow"]
