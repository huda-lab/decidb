"""Named databases the site's SQL runs against.

A fixture has `setup` SQL and, for fragments (`frag`), a `template` whose {CLAUSES} the fragment replaces.
Every example's setup is also a fixture under its slug.
"""

import glob
import os

import examples as EX

DECK_SETUP = """
CREATE TABLE Depot (depotID VARCHAR, region VARCHAR, country VARCHAR, capacity INT, priorityCapacity INT,
                    networkReserveShare DOUBLE, maxShipmentShare DOUBLE, networkCapacity INT, opening_cost INT);
INSERT INTO Depot VALUES
    ('D1', 'North', 'US', 100, 40, 0.10, 0.6, 300, 500),
    ('D2', 'North', 'US', 80, 30, 0.12, 0.7, 300, 300),
    ('D3', 'South', 'US', 90, 35, 0.08, 0.6, 300, 450),
    ('D4', 'South', 'MX', 60, 20, 0.15, 0.8, 300, 250);
CREATE TABLE Shipment (shipmentID VARCHAR, depotID VARCHAR, customerID VARCHAR, demand INT, priority BOOLEAN,
                       dispatchDay INT, transitDays INT, unit_cost DOUBLE);
INSERT INTO Shipment VALUES
    ('S1', 'D1', 'C1', 30, true, 1, 2, 2.0), ('S2', 'D1', 'C2', 25, false, 1, 1, 1.5),
    ('S3', 'D2', 'C1', 40, true, 2, 1, 2.5), ('S4', 'D3', 'C3', 35, false, 1, 3, 1.0),
    ('S5', 'D3', 'C2', 20, true, 2, 2, 3.0), ('S6', 'D4', 'C4', 45, false, 3, 1, 1.2),
    ('S7', 'D2', 'C3', 15, false, 2, 1, 2.2);
"""

ITEMS_SETUP = """
CREATE TABLE Items (item VARCHAR, value INT, weight INT, category VARCHAR);
INSERT INTO Items VALUES
    ('Laptop', 100, 20, 'tech'), ('Camera', 60, 10, 'tech'), ('Drone', 120, 30, 'tech'),
    ('Tripod', 40, 15, 'gear'), ('Lens', 75, 12, 'gear');
"""

FIXTURES = {
    "empty": {"setup": ""},
    "deck": {
        "setup": DECK_SETUP,
        "template": """SELECT S.shipmentID, ship
FROM Shipment S JOIN Depot D USING (depotID)
DECIDE ship(INT), per D.depotID: reserve(REAL)
SUCH THAT ship <= S.demand AND {CLAUSES}
MAXIMIZE SUM(ship)""",
    },
    "deck-obj": {
        "setup": DECK_SETUP,
        "template": """SELECT S.shipmentID, ship
FROM Shipment S JOIN Depot D USING (depotID)
DECIDE ship(INT), per D: open(BOOL), per (): cap(INT)
SUCH THAT ship <= S.demand * open AND SUM(ship) >= 100 AND cap <= 500
{CLAUSES}""",
    },
    "items": {
        "setup": ITEMS_SETUP,
        "template": """SELECT item, x
FROM Items
DECIDE x(BOOL)
SUCH THAT {CLAUSES}
MAXIMIZE SUM(x * value)""",
    },
    "items-int": {
        "setup": ITEMS_SETUP,
        "template": """SELECT item, x
FROM Items
DECIDE x(INT)
SUCH THAT x <= 3 AND {CLAUSES}
MAXIMIZE SUM(x * value)""",
    },
    "items-real": {
        "setup": ITEMS_SETUP,
        "template": """SELECT item, x
FROM Items
DECIDE x(REAL)
SUCH THAT x <= 3 AND {CLAUSES}
MAXIMIZE SUM(x * value)""",
    },
    "category-rules": {
        "template": """SELECT name, pick
FROM Products
WHERE in_stock
DECIDE pick(BOOL)
SUCH THAT SUM(pick * cost) <= (SELECT budget FROM Settings) AND {CLAUSES}
MAXIMIZE SUM(pick * value)""",
    },
    "shift-coverage": {
        "template": """SELECT nurse, shift, on_duty
FROM Availability
DECIDE on_duty(BOOL)
SUCH THAT per shift: SUM(on_duty) by (shift) = 1 AND {CLAUSES}""",
    },
}

for _ex in EX.EXAMPLES:
    FIXTURES.setdefault(_ex["slug"], {})
    FIXTURES[_ex["slug"]]["setup"] = _ex["setup"]

FIXTURES["items-two"] = {
    "setup": ITEMS_SETUP,
    "template": """SELECT item, x, b
FROM Items
DECIDE x(INT), b(BOOL)
SUCH THAT x <= 3 AND {CLAUSES}
MAXIMIZE SUM(x * value)""",
}
FIXTURES["r1"] = {
    "setup": """CREATE TABLE Cargo (good VARCHAR, value_per_kg DOUBLE, available_kg DOUBLE);
INSERT INTO Cargo VALUES
    ('Saffron', 90, 2), ('Coffee', 12, 300), ('Tea', 9, 200), ('Rice', 1.5, 900), ('Sugar', 1.1, 800);"""
}
FIXTURES["a1"] = {
    "setup": """CREATE TABLE Orders (sku VARCHAR, margin DOUBLE, min_qty INT, max_qty INT);
INSERT INTO Orders VALUES
    ('A-100', 4.0, 10, 50), ('B-200', -1.5, 5, 20), ('C-300', 2.5, 0, 30), ('D-400', -0.5, 3, 8);"""
}
FIXTURES["n1"] = {
    "setup": """CREATE TABLE Signals (name VARCHAR, score DOUBLE);
INSERT INTO Signals VALUES ('alpha', 3), ('beta', 4), ('gamma', 0);"""
}

# the documentation pages' own databases: each docs_fixtures/<name>.py defines FIXTURES = {...}; a name may not repeat
for _path in sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), "docs_fixtures", "*.py"))):
    _scope = {}
    exec(compile(open(_path).read(), _path, "exec"), _scope)
    for _name, _fx in _scope.get("FIXTURES", {}).items():
        if _name in FIXTURES:
            raise SystemExit(f"{_path}: fixture {_name!r} is already defined")
        FIXTURES[_name] = _fx
