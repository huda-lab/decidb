"""`DeciDBCli.execute` reads a result's NULL, NaN, infinity and quoted text back as the values SQL holds."""

import math

import pytest

from decidb_cli import _REUSE_ENV

QUERY = (
    "SELECT 'NaN'::DOUBLE AS d_nan, NULL::DOUBLE AS d_null, 'inf'::DOUBLE AS d_inf, '-inf'::DOUBLE AS d_ninf, "
    "'NaN'::FLOAT AS f_nan, 'nan' AS s_nan, 'NULL' AS s_null, 'a,b''c\nd|e' AS s_punct, '' AS s_empty, "
    "NULL::VARCHAR AS s_null_value, 7 AS i, 1.5 AS f, true AS b"
)


@pytest.fixture(params=["session", "one_process"])
def cli(request, decidb_cli, monkeypatch):
    """The wrapper reading a result from a long-lived session, and from a process of its own."""
    if request.param == "one_process":
        monkeypatch.setenv(_REUSE_ENV, "0")
    return decidb_cli


def test_execute_reads_a_nan_and_a_null_as_different_values(cli):
    rows, columns = cli.execute(QUERY)
    (row,) = rows
    value = dict(zip(columns, row))
    assert math.isnan(value["d_nan"]) and math.isnan(value["f_nan"])
    assert value["d_null"] is None and value["s_null_value"] is None
    assert value["d_inf"] == math.inf and value["d_ninf"] == -math.inf
    # Text that spells a bare token stays text, and quoting keeps separators and newlines inside the value.
    assert value["s_nan"] == "nan" and value["s_null"] == "NULL"
    assert value["s_punct"] == "a,b'c\nd|e" and value["s_empty"] == ""
    assert value["i"] == 7 and type(value["i"]) is int
    assert value["f"] == 1.5 and value["b"] is True


def test_execute_reads_several_rows_and_no_rows(cli):
    rows, columns = cli.execute("SELECT x, CASE WHEN x = 2 THEN NULL ELSE x END AS y FROM range(1, 4) t(x) ORDER BY x")
    assert (rows, columns) == ([(1, 1), (2, None), (3, 3)], ["x", "y"])
    assert cli.execute("SELECT 1 AS x WHERE false") == ([], [])
