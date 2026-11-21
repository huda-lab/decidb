"""What the data left of a clause, said where the user can read it (owner, 2026-11-04).

An instance with no value -- a NULL it reads, a position a frame does not find, a reducer over
no row -- is skipped (deck pp. 50, 56). EXPLAIN ANALYZE counts the instances a clause skipped
under the clause, and a clause left with none, which constrains nothing, is a warning on stderr;
so is an objective that is 0 whatever the decisions. EXPLAIN ANALYZE also says which helpers
stage 06 wrote where they are read (D89). A plain EXPLAIN runs nothing, so it says none of it.
The solver path reports these; a query direct solve answers prints nothing, so every query here
runs on the solver path.
"""

import re

import pytest

from ._output_helpers import assert_no_internal_leak
from ._scope_cases import table

EDGE = ("id", "product", "period", "v", "w")
#: Products A (three periods), B (two) and C (one): `previous` misses at each timeline's start.
EDGE_ROWS = (
    (1, "A", 1, 4, 2),
    (2, "A", 2, 6, 1),
    (3, "A", 3, 1, 3),
    (4, "B", 1, 7, 2),
    (5, "B", 2, 3, 1),
    (6, "C", 1, 5, 2),
)
TL = "over (t.period within t.product)"
SOLVER_PATH = "SET decide_direct_solve='off'; "


def _query(clause_sql, objective="MAXIMIZE sum(x)"):
    return f"SELECT id, x {table('t', EDGE, EDGE_ROWS)} DECIDE x(INT) SUCH THAT x <= 9 AND {clause_sql} {objective}"


def _run(cli, sql):
    result = cli.execute_raw(SOLVER_PATH + sql)
    assert_no_internal_leak(result, sql)
    assert result.returncode == 0, result.stderr
    return result


def _profile(cli, sql):
    """EXPLAIN ANALYZE's DECIDE box, its wrapped lines joined and its spaces dropped."""
    out = _run(cli, "EXPLAIN ANALYZE " + sql).stdout
    return "".join(line.strip().strip("│").strip() for line in out.splitlines()).replace(" ", "")


def _plain(cli, sql):
    return _run(cli, "EXPLAIN " + sql).stdout.replace(" ", "")


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause_sql, warning",
    [
        # Every timeline is shorter than five periods: no row finds its position.
        pytest.param(
            f"x <= at(5 previous: x) {TL}",
            f"'x <= at(5 previous: x) {TL}' constrains nothing: each of its 6 instances has no value, "
            "so each is skipped.",
            id="per-row",
        ),
        pytest.param(
            f"per t.product: sum(at(5 previous: x) {TL}) by (t.product) <= 3",
            f"'per t.product: sum(at(5 previous: x) {TL}) by (t.product) <= 3' constrains nothing: each of its 3 "
            "instances has no value, so each is skipped.",
            id="per-group",
        ),
        # A composed MIN/MAX clause is one instance; its MAX reads no row.
        pytest.param(
            "sum(x) + max(when t.v > 100: x) <= 30",
            "'sum(x) + max(when t.v > 100: x) <= 30' constrains nothing: its one instance has no value, so it is "
            "skipped.",
            id="composed",
        ),
    ],
)
def test_a_clause_left_with_no_instance_warns(decidb_cli, clause_sql, warning):
    """The query is answered as written -- the clause skips every instance -- and stderr
    says the clause constrains nothing, once."""
    result = _run(decidb_cli, _query(clause_sql))
    warnings, rest = decidb_cli.warnings(result.stderr)
    assert warnings == [warning] and not rest.strip(), result.stderr


@pytest.mark.correctness
def test_a_clause_that_skips_some_instances_counts_them(decidb_cli):
    """A, B and C each start with a period that has no previous one: 3 of 6 instances are
    skipped, which EXPLAIN ANALYZE says under the clause. The clause still constrains,
    so there is no warning."""
    sql = _query(f"x <= at(previous: t.v) {TL}")
    assert not _run(decidb_cli, sql).stderr.strip()
    assert "↳3of6instanceshavenovalueandareskipped" in _profile(decidb_cli, sql)
    assert "skipped" not in _plain(decidb_cli, sql)


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause_sql",
    [
        pytest.param("when t.v > 100: x <= 3", id="per-row"),
        pytest.param("when t.v > 100: sum(x) by (t.product) <= 3", id="per-group"),
        pytest.param("when t.v > 100: sum(x) + max(x) <= 30", id="composed"),
    ],
)
def test_a_when_that_keeps_no_row_is_no_warning(decidb_cli, clause_sql):
    """A clause whose `when` keeps no row holds no instance to skip: the user's own filter,
    not a loss of data."""
    sql = _query(clause_sql)
    assert not _run(decidb_cli, sql).stderr.strip()
    assert "skipped" not in _profile(decidb_cli, sql)


@pytest.mark.correctness
def test_a_per_key_null_on_every_row_is_no_warning(decidb_cli):
    """A row whose `per` key is NULL is in no class (D5): with every key NULL the clause holds
    no instance to skip."""
    rows = ((1, None, 4), (2, None, 6))
    sql = f"""SELECT id, x {table('t', ('id', 'k', 'v'), rows)} DECIDE x(INT)
        SUCH THAT x <= t.v AND per t.k: sum(x) <= 3 MAXIMIZE sum(x)"""
    assert not _run(decidb_cli, sql).stderr.strip()


# `per t.k: sum(x) by (t.g) >= t.cap`: one class (k = 1), whose rows' `by` key g is NULL where it reaches no group. The
# class holds one instance, which has no value (guide §7.4), as one whose bound is NULL does.
@pytest.mark.correctness
@pytest.mark.parametrize(
    "rows",
    [
        # The class's only row has a NULL g: no group, and a NULL k row beside it reads group 'a' for no class.
        pytest.param(((1, None, 1, 5), (2, "a", None, None)), id="class-reaches-no-group"),
        # The class reads group 'a' too, so its instance is that group's and the NULL-g row adds none.
        pytest.param(((1, "a", 1, None), (2, None, 1, 5)), id="class-reaches-a-group"),
    ],
)
def test_a_class_whose_by_key_is_null_holds_one_instance_with_no_value(decidb_cli, rows):
    sql = f"""SELECT id, x {table('t', ('id', 'g', 'k', 'cap'), rows)} DECIDE x(INT)
        SUCH THAT x <= 100 AND per t.k: sum(x) by (t.g) >= t.cap MINIMIZE sum(x) ORDER BY id"""
    result = _run(decidb_cli, sql)
    warnings, rest = decidb_cli.warnings(result.stderr)
    assert (
        warnings
        == ["'per t.k: sum(x) by (t.g) >= t.cap' constrains nothing: its one instance has no value, so it is skipped."]
        and not rest.strip()
    ), result.stderr
    assert "↳itsoneinstancehasnovalueandisskipped" in _profile(decidb_cli, sql)


@pytest.mark.correctness
def test_a_folded_bound_counts_its_skipped_instances_as_any_clause_does(decidb_cli):
    """`c <= 5` is folded into the box and never reaches the row evaluation, `c + d <= 5` is not: where a row has no
    class of the `per` key, both read no value there, so both say the same under the clause and neither warns, a class
    holding the other rows. A decision with a class at every row has nothing to say."""
    rows = ((1, "a", 1.0), (2, "b", 2.0), (3, None, 3.0))

    def sql(decisions, clause, rows):
        return (
            f"SELECT id {table('t', ('id', 'k', 'v'), rows)} DECIDE per t.k: {decisions} "
            f"SUCH THAT {clause} MINIMIZE sum(power(c - t.v, 2))"
        )

    skipped = re.compile(r"↳\d+of\d+instanceshavenovalueandareskipped")

    def line(query):
        assert not _run(decidb_cli, query).stderr.strip()
        return skipped.findall(_profile(decidb_cli, query))

    folded = line(sql("c(REAL)", "c <= 5", rows))
    assert folded and folded == line(sql("c(REAL), d(REAL)", "c + d <= 5", rows))
    assert not line(sql("c(REAL)", "c <= 5", rows[:2]))


@pytest.mark.correctness
def test_an_objective_that_is_zero_warns(decidb_cli):
    """Every term of the objective has no value, so it is 0 whatever the decisions: any
    feasible answer is optimal, which stderr and EXPLAIN ANALYZE say."""
    sql = _query("x >= 1", objective=f"MAXIMIZE sum(at(5 previous: x) {TL})")
    result = _run(decidb_cli, sql)
    warnings, rest = decidb_cli.warnings(result.stderr)
    assert (
        warnings
        == [
            "the objective is 0 whatever the decisions: each of its terms has no value or a zero factor on every row, so "
            "any feasible answer is optimal."
        ]
        and not rest.strip()
    ), result.stderr
    assert "↳is0whateverthedecisions" in _profile(decidb_cli, sql)


@pytest.mark.correctness
@pytest.mark.parametrize(
    "cli_fixture, decide, objective_sql",
    [
        # Each reads its decisions through one part of the model alone.
        pytest.param("decidb_cli", "x(REAL)", "MINIMIZE sum(power(x, 2))", id="a-square"),
        pytest.param("decidb_cli", "x(INT)", "MINIMIZE max(x)", id="an-extremum-column"),
        pytest.param("decidb_cli_gurobi", "x(INT), y(INT)", "MAXIMIZE sum(x * y)", id="a-product"),
    ],
)
def test_an_objective_that_reads_a_column_is_no_warning(request, cli_fixture, decide, objective_sql):
    """A square, an extremum's column or a product of decisions is no zero objective."""
    cli = request.getfixturevalue(cli_fixture)
    sql = f"""SELECT id, x {table('t', EDGE, EDGE_ROWS)} DECIDE {decide}
        SUCH THAT x <= 9 AND x >= 1 {"AND y <= 3" if "y" in decide else ""} {objective_sql}"""
    assert not _run(cli, sql).stderr.strip()


@pytest.mark.correctness
def test_a_query_with_no_objective_is_no_warning(decidb_cli):
    """With no objective there is none to be 0."""
    sql = f"SELECT id, x {table('t', EDGE, EDGE_ROWS)} DECIDE x(INT) SUCH THAT x <= 9 AND x >= 1"
    assert not _run(decidb_cli, sql).stderr.strip()


@pytest.mark.correctness
@pytest.mark.parametrize(
    "clause_sql, line",
    [
        # Each window read by its anchor alone: written into its row.
        pytest.param(
            f"x >= sum(from 2 previous to previous else 0: x) {TL} - t.w",
            f"↳sum(from2previoustopreviouselse0:x){TL}iswrittenwhereitisread,withnohelperdecision",
            id="whole",
        ),
        # A reducer's helper per group, each read by its group's rows once.
        pytest.param(
            "x <= sum(t.w * x) by (t.product) - 1",
            "↳sum(t.w*x)by(t.product)iswrittenwhereitisread,withnohelperdecision",
            id="a-reducer",
        ),
        # C's window is C alone: that group of one member is its member; the others keep their columns.
        pytest.param(
            f"x >= max(from previous to next: x) {TL} - 1",
            f"↳max(frompreviousto next:x){TL}:eachgroupofonememberiswrittenwhereitisread,withnohelperdecision",
            id="groups-of-one-member",
        ),
    ],
)
def test_a_helper_written_where_it_is_read_says_so(decidb_cli, clause_sql, line):
    """The plan says a range becomes a helper; whether stage 06 writes it where it is read
    is the data's (D89), so EXPLAIN ANALYZE says that under the clause."""
    sql = _query(clause_sql)
    assert line.replace(" ", "") in _profile(decidb_cli, sql)
    assert "writtenwhere" not in _plain(decidb_cli, sql)
