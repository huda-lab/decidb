"""Databases for the DIAGNOSE documentation page (docs/diagnose.html)."""

BAG = """CREATE TABLE Items (item VARCHAR, value INT, weight INT, category VARCHAR);
INSERT INTO Items VALUES
    ('Laptop', 100, 20, 'tech'), ('Camera', 60, 10, 'tech'), ('Drone', 120, 30, 'tech'),
    ('Tripod', 40, 15, 'gear'), ('Lens', 75, 12, 'gear');"""

LINES = """CREATE TABLE Lines (line VARCHAR, ordered INT, available INT);
INSERT INTO Lines VALUES ('L1', 20, 30), ('L2', 25, 18), ('L3', 10, 40), ('L4', 15, 12);"""

ROUTES = """CREATE TABLE Routes (route VARCHAR, region VARCHAR, margin INT);
INSERT INTO Routes VALUES
    ('R1', 'East', 3), ('R2', 'East', 5), ('R3', 'West', 3),
    ('R4', 'West', 5), ('R5', 'North', 3), ('R6', 'North', 5);"""

DEPOTS = """CREATE TABLE Depots (depot VARCHAR, capacity INT, demand INT);
INSERT INTO Depots VALUES ('North', 10, 30), ('South', 20, 5);"""

EXPANDED = "SET diagnose_decide_infeasible_slack_scope = 'expanded';"

FIXTURES = {
    # the Guide's five-item bag, with its categories
    "diagnose-bag": {"setup": BAG},
    # order lines whose stock falls short of the order on two lines (a conflict in the data)
    "diagnose-lines": {"setup": LINES},
    "diagnose-lines-expanded": {"setup": LINES + "\n" + EXPANDED},
    # shipping routes with one region left uncapped (an unbounded query)
    "diagnose-routes": {"setup": ROUTES},
    # a capacity that only an open depot provides: both sides of one clause hold decisions
    "diagnose-depots": {"setup": DEPOTS},
}
