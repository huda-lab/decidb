"""Shared data and helpers for the scope tests: `when` and `per` in front of a clause or
inside a reducer, and `by` after one (syntax_reference.md §5-§7).

The deck's relations (§6.5), small enough to check by hand: five shipments joined to
three depots in two regions and two countries, with a priority flag, a status and one
network capacity. ``from_clause`` renders them as VALUES -- with any rows swapped in --
and ``joined`` as the oracle's rows, in shipmentID order, the order every query sorts by.
"""

from __future__ import annotations

import re

import pytest

SHIPMENT_COLUMNS = ("shipmentID", "depotID", "demand", "priority", "dispatchDay", "transitDays", "status")
DEPOT_COLUMNS = ("depotID", "region", "country", "capacity", "priorityCapacity", "networkReserveShare",
                 "maxShipmentShare", "networkCapacity")
SHIPMENTS = (
    (1, "D1", 10, True, 1, 2, "active"),
    (2, "D1", 20, False, 1, 3, "active"),
    (3, "D2", 30, True, 2, 1, "active"),
    (4, "D2", 15, False, 2, 2, "hold"),
    (5, "D3", 25, True, 3, 1, "active"),
)
DEPOTS = (
    ("D1", "East", "US", 25, 8, 0.1, 0.5, 60),
    ("D2", "East", "CA", 40, 12, 0.2, 0.6, 60),
    ("D3", "West", "US", 20, 30, 0.3, 0.7, 60),
)


def literal(value) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, str):
        return "'" + value.replace("'", "''") + "'"
    return str(value)


def values(rows, alias: str, columns) -> str:
    tuples = ", ".join("(" + ", ".join(literal(v) for v in row) + ")" for row in rows)
    return f"(VALUES {tuples}) {alias}({', '.join(columns)})"


def from_clause(shipments=SHIPMENTS, depots=DEPOTS) -> str:
    return (f"FROM {values(shipments, 'S', SHIPMENT_COLUMNS)} "
            f"JOIN {values(depots, 'D', DEPOT_COLUMNS)} USING (depotID)")


def joined(shipments=SHIPMENTS, depots=DEPOTS) -> list[dict]:
    """The result rows of ``from_clause(shipments, depots)``, in shipmentID order."""
    by_id = {depot[0]: dict(zip(DEPOT_COLUMNS, depot)) for depot in depots}
    rows = []
    for shipment in sorted(shipments):
        row = dict(zip(SHIPMENT_COLUMNS, shipment))
        if row["depotID"] in by_id:
            row.update(by_id[row["depotID"]])
            rows.append(row)
    return rows


def table(name: str, columns, rows) -> str:
    """A one-relation FROM clause over VALUES."""
    return f"FROM {values(rows, name, columns)}"


def table_rows(columns, rows) -> list[dict]:
    return [dict(zip(columns, row)) for row in rows]


def answer(cli, sql: str) -> list[dict]:
    rows, columns = cli.execute(sql)
    return [dict(zip(columns, row)) for row in rows]


def aligned(result: list[dict], model, id_column: str = "shipmentID") -> list[dict]:
    assert [row[id_column] for row in result] == [row[id_column] for row in model.rows], (
        "DeciDB's rows are not in the oracle's order")
    return result


_HEADER = re.compile(r"^(num_vars|num_rows): (\d+)$", re.M)


def dump_counts(dump: str) -> dict[str, int]:
    """``num_vars`` (one solver column per decision) and ``num_rows`` (linear rows)."""
    counts = {name: int(count) for name, count in _HEADER.findall(dump)}
    assert set(counts) == {"num_vars", "num_rows"}, f"no model header in the dump:\n{dump}"
    return counts


def error_of(cli, sql: str) -> str:
    """The error ``sql`` raises (its stderr); fails if it runs."""
    result = cli.execute_raw(sql)
    assert result.returncode != 0 or result.stderr.strip(), f"expected an error, got:\n{result.stdout}"
    return result.stderr


@pytest.fixture
def scope_solver(_raw_oracle_solver):
    """Gurobi, solving afresh each time: the oracle models are tiny, and a cached optimum
    would hide a change to `_scope_oracle`."""
    if _raw_oracle_solver is None:
        pytest.skip("Gurobi not available — the scope oracle requires it")
    return _raw_oracle_solver
