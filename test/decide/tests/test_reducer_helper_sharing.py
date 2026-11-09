"""One helper decision for one reducer (MSG-1): `Helpers::Find` keys on the reducer, not on the text it was written with.

A reducer that a row-varying decision multiplies is held in a helper decision (the value rewrite). Two clauses that read
the same reducer, in the same scope, read the same helper, however each spells the reducer. The alias used to carry that
spelling, so `x <= 0.5 * sum(x) AND x >= 0.1 * sum(x)` built two helpers where `Helpers::Find` promised one.
"""

import pytest

#: Three rows, so the model has three columns of x before it has any helper.
_DATA = "(VALUES (1, 'F', 2.5, 1, 'a'), (2, 'G', 3.5, 2, 'a'), (3, 'F', 4.5, 3, 'b')) t(id, s, w, b, g)"


def _columns(cli, sql, tmp_path):
    """The number of columns in the model the clauses build."""
    dump = cli.dump_model(f"SET decide_direct_solve='off'; {sql}", tmp_path / "model.dump")
    return int(next(line for line in dump.splitlines() if line.startswith("num_vars:")).split(":")[1])


_COLUMNS = 3  # x on each of the three rows of _DATA


@pytest.mark.parametrize(
    "clauses, helpers",
    [
        pytest.param("x <= 0.5 * sum(x) AND x >= 0.1 * sum(x)", 1, id="the-same-reducer-in-two-clauses"),
        pytest.param("x <= 0.5 * sum(x) AND x >= 0.1 * SUM( x )", 1, id="spelled-apart"),
        pytest.param("x <= 0.5 * sum(x) AND x >= 0.1 * sum(when s = 'F': x)", 2, id="reducers-own-when-differs"),
        pytest.param("x <= 0.5 * sum(x) AND when s = 'F': x >= 0.1 * sum(x)", 2, id="clauses-when-differs"),
        pytest.param("x <= 0.5 * sum(x) AND x >= 0.1 * sum(per id: x)", 2, id="reducers-own-per-differs"),
        # `per id` is a class of one row: the clause is the same rows, under another key.
        pytest.param("x <= 0.5 * sum(x) AND per id: x >= 0.1 * sum(x)", 2, id="clauses-per-key-differs"),
        pytest.param("per id: x <= 0.5 * sum(x) AND per id: x >= 0.1 * sum(x)", 1, id="the-same-per-key"),
        # `by (g)` holds one helper per group (two), beside the helper of the plain `sum(x)`.
        pytest.param("x <= 0.5 * sum(x) by (g) AND x >= 0.1 * sum(x)", 3, id="by-key-differs"),
    ],
)
def test_a_reducer_gets_one_helper_per_scope(decidb_cli, tmp_path, clauses, helpers):
    """Helpers::Find keys on the reducer (Equals), its `by`, its own `per`, the clause's `when` and its `per` key.
    The alias, which carried the text the reducer was written with, is not part of it."""
    sql = f"SELECT id, x FROM {_DATA} DECIDE x(REAL) SUCH THAT {clauses} AND SUM(x) <= 10 MAXIMIZE SUM(x)"
    assert _columns(decidb_cli, sql, tmp_path) == _COLUMNS + helpers
