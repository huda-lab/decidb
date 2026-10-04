"""Conformance to the NYUAD 'DeciQL proposals' deck, on the deck's own data.

Every example is run as the deck spells it (or as close as the shipped surface
allows, with the deviation named in the docstring) and checked against an
independent gurobipy model of the same problem. Page numbers refer to the deck;
section numbers to `syntax_reference.md`.

The ANR examples (p17-25) share one Shipment/Depot pair of PRIMARY KEY temp
tables; the frame examples (p44-50, p73-75) share a demand_forecast/product_policy
pair. Each docstring says which wrong answer a plausible bug would return; where
a construct is equivalent to its omission on this data, it says so.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType

# --- Deck p14: Shipment S join Depot D ------------------------------------
_DECK = """
CREATE TEMP TABLE Depot(depotID VARCHAR PRIMARY KEY, region VARCHAR, country VARCHAR,
    capacity INT, priorityCapacity INT, networkReserveShare DOUBLE, maxShipmentShare DOUBLE, openCost INT);
INSERT INTO Depot VALUES ('D1','North','UAE',10,6,0.10,0.5,20), ('D2','North','KSA',8,4,0.20,0.6,10),
    ('D3','South','KSA',12,5,0.15,0.5,50);
CREATE TEMP TABLE Shipment(shipmentID VARCHAR PRIMARY KEY, depotID VARCHAR, customerID VARCHAR,
    demand INT, priority BOOLEAN, dispatchDay INT, transitDays INT, w INT);
INSERT INTO Shipment VALUES ('S1','D1','C1',5,true,1,2,1), ('S2','D1','C1',3,false,1,3,2),
    ('S3','D1','C2',4,true,2,1,3), ('S4','D2','C2',6,true,1,1,4), ('S5','D2','C3',2,false,2,2,5),
    ('S6','D3','C3',7,true,3,1,6);
"""
_FROM = "FROM Shipment S JOIN Depot D ON S.depotID = D.depotID"
# depotID: (region, country, capacity, priorityCapacity, networkReserveShare, maxShipmentShare, openCost)
_DEPOT = {"D1": ("North", "UAE", 10, 6, 0.10, 0.5, 20), "D2": ("North", "KSA", 8, 4, 0.20, 0.6, 10),
          "D3": ("South", "KSA", 12, 5, 0.15, 0.5, 50)}
# shipmentID: (depotID, demand, priority, w)   -- w is a distinct weight that makes optima unique
_SHIP = {"S1": ("D1", 5, True, 1), "S2": ("D1", 3, False, 2), "S3": ("D1", 4, True, 3),
         "S4": ("D2", 6, True, 4), "S5": ("D2", 2, False, 5), "S6": ("D3", 7, True, 6)}
_TOTAL_DEMAND = 27  # per depot 12 / 8 / 7; region North 20, South 7; country UAE 12, KSA 15

# --- Deck p73: demand_forecast D join product_policy P ---------------------
_PLAN = """
CREATE TEMP TABLE demand_forecast(product VARCHAR, period INT, demand INT, PRIMARY KEY (product, period));
INSERT INTO demand_forecast VALUES ('A',1,3),('A',2,5),('A',3,4),('B',1,2),('B',2,6),('B',3,1);
CREATE TEMP TABLE product_policy(product VARCHAR PRIMARY KEY, opening_stock INT, safety_stock INT,
    max_rate INT, warehouse_cap INT, max_ramp INT, hold_cost INT, run_cost INT);
INSERT INTO product_policy VALUES ('A',2,1,6,20,3,1,2), ('B',3,1,4,20,1,2,1);
"""
_PFROM = "FROM demand_forecast D JOIN product_policy P USING (product)"
_DEMAND = {("A", 1): 3, ("A", 2): 5, ("A", 3): 4, ("B", 1): 2, ("B", 2): 6, ("B", 3): 1}
# product: (opening_stock, safety_stock, max_rate, warehouse_cap, max_ramp, hold_cost, run_cost)
_POLICY = {"A": (2, 1, 6, 20, 3, 1, 2), "B": (3, 1, 4, 20, 1, 2, 1)}


def _rows(decidb_cli, sql, *cols):
    rows, names = decidb_cli.execute(sql)
    idx = [names.index(c) for c in cols]
    return sorted(tuple(r[i] for i in idx) for r in rows)


def _ships_at(depot):
    return [s for s, v in _SHIP.items() if v[0] == depot]


def _ship_model(oracle, name):
    """ship_S1..S6 as INTEGER in [0, demand]; returns the weighted objective {var: w}."""
    oracle.create_model(name)
    for s, (_, demand, _, _) in _SHIP.items():
        oracle.add_variable(f"ship_{s}", VarType.INTEGER, lb=0.0, ub=float(demand))
    return {f"ship_{s}": float(v[3]) for s, v in _SHIP.items()}


def _weighted(got):
    return sum(int(ship) * _SHIP[s][3] for s, ship in got)


def _oracle_network_share(oracle):
    """ANR example 1 (p17): every shipment at most 20% of the network total."""
    _ship_model(oracle, "anr1")
    everything = {f"ship_{s}": 1.0 for s in _SHIP}
    for s in _SHIP:
        oracle.add_constraint({**{k: -0.2 for k in everything}, f"ship_{s}": 0.8}, "<=", 0.0)
    oracle.set_objective(everything, ObjSense.MAXIMIZE)
    return oracle.solve()


def _oracle_depot_capacity(oracle):
    """ANR example 4 (p20): one row per depot over that depot's shipments."""
    obj = _ship_model(oracle, "anr4")
    for d, (_, _, cap, *_) in _DEPOT.items():
        oracle.add_constraint({f"ship_{s}": 1.0 for s in _ships_at(d)}, "<=", float(cap))
    oracle.set_objective(obj, ObjSense.MAXIMIZE)
    return oracle.solve()


def _oracle_priority_capacity(oracle):
    """ANR example 7 (p23): only the priority shipments of a depot count."""
    obj = _ship_model(oracle, "anr7")
    for d, (_, _, _, prio_cap, *_) in _DEPOT.items():
        rows = {f"ship_{s}": 1.0 for s in _ships_at(d) if _SHIP[s][2]}
        oracle.add_constraint(rows, "<=", float(prio_cap))
    oracle.set_objective(obj, ObjSense.MAXIMIZE)
    return oracle.solve()


# ===========================================================================
# ANR examples 1-9 (deck p17-25)
# ===========================================================================

@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_anr1_tuple_per_global_aggregation(decidb_cli, oracle_solver):
    """p17: `ship <= 0.20 * SUM(ship)` -- one row per shipment against the network
    total. Ignoring the constraint ships every demand (27); reading the sum per
    depot forces every shipment to 0 (no depot has the five shipments a 20% share
    needs), per region gives 2 each but S6 0 (10). The unique fixed point is 21."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT ship <= 0.20 * SUM(ship) MAXIMIZE SUM(ship)""", "shipmentID", "ship")
    result = _oracle_network_share(oracle_solver)
    assert result.status == SolverStatus.OPTIMAL

    assert [ship for _, ship in got] == [4, 3, 4, 4, 2, 4]
    assert sum(ship for _, ship in got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.correctness
def test_anr1_fully_explicit_spelling_is_the_default(decidb_cli, oracle_solver):
    """p17's 'fully explicit equivalent': `PER ROW: ... SUM(PER ROW: ship) BY ()`, on
    the declarator and the objective too, is the same problem as the bare form
    (§2.1, §3, §4). The explicit `by ()` is read: `by (D.depotID)` in its place
    ships nothing."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE PER ROW: ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT per row: ship <= 0.20 * sum(per row: ship) by ()
        MAXIMIZE PER (): SUM(ship)""", "shipmentID", "ship")
    result = _oracle_network_share(oracle_solver)
    assert result.status == SolverStatus.OPTIMAL

    assert [ship for _, ship in got] == [4, 3, 4, 4, 2, 4]
    assert sum(ship for _, ship in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_anr2_global_per_global_aggregation(decidb_cli, oracle_solver):
    """p18: `PER (): SUM(ship) <= 15` -- one network row. The heaviest weights fill
    the 15 exactly (S6, S5, S4). Dropping `PER ()` is equivalent here (one copy of
    the same network row per shipment); what discriminates is that SUM reads every
    row: a per-row `ship <= 15` or a per-depot `SUM(ship) BY (D.depotID) <= 15`
    ships all 27."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT PER (): SUM(ship) <= 15 MAXIMIZE SUM(w * ship)""", "shipmentID", "ship")

    obj = _ship_model(oracle_solver, "anr2")
    oracle_solver.add_constraint({f"ship_{s}": 1.0 for s in _SHIP}, "<=", 15.0)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [ship for _, ship in got] == [0, 0, 0, 6, 2, 7]
    assert _weighted(got) == pytest.approx(result.objective_value)


@pytest.mark.var_real
@pytest.mark.per_clause
@pytest.mark.correctness
def test_anr3_local_per_global_aggregation(decidb_cli, oracle_solver):
    """p19: `PER D.depotID: reserve >= D.networkReserveShare * SUM(S.demand)` -- the
    depot's share (read through Depot's PRIMARY KEY, §3.1) of the network demand 27.
    A per-depot sum would give D1 0.1 * 12 = 1.2 instead of 2.7."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT DISTINCT D.depotID, reserve {_FROM} DECIDE PER D.depotID: reserve(REAL)
        SUCH THAT PER D.depotID: reserve >= D.networkReserveShare * SUM(S.demand)
        MINIMIZE SUM(PER D.depotID: reserve)""", "depotID", "reserve")

    oracle_solver.create_model("anr3")
    for d, (_, _, _, _, share, _, _) in _DEPOT.items():
        oracle_solver.add_variable(f"reserve_{d}", VarType.CONTINUOUS, lb=0.0, ub=100.0)
        oracle_solver.add_constraint({f"reserve_{d}": 1.0}, ">=", share * _TOTAL_DEMAND)
    oracle_solver.set_objective({f"reserve_{d}": 1.0 for d in _DEPOT}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [r for _, r in got] == pytest.approx([2.7, 5.4, 4.05])
    assert sum(r for _, r in got) == pytest.approx(result.objective_value)


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_anr3_depot_column_needs_a_key_the_generation_key_covers(decidb_cli):
    """§3.1: without a PRIMARY KEY on Depot, `PER D.depotID` does not determine
    `networkReserveShare` and the column is refused by name."""
    decidb_cli.assert_error(_DECK.replace("PRIMARY KEY", "") + f"""
        SELECT DISTINCT D.depotID, reserve {_FROM} DECIDE PER D.depotID: reserve(REAL)
        SUCH THAT PER D.depotID: reserve >= D.networkReserveShare * SUM(S.demand)
        MINIMIZE SUM(PER D.depotID: reserve)""", match=r"not determined by the generation key")


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_anr4_local_per_local_aggregation(decidb_cli, oracle_solver):
    """p20: `PER D.depotID: SUM(ship) BY (D.depotID) <= D.capacity` -- each depot's
    own shipments against its own capacity. D1 (cap 10 over demands 5, 3, 4) drops
    two units of its lightest shipment. A global BY () would cap the whole network
    at the tightest capacity, 8 (S5 1, S6 7); BY (D.region) would empty D1. Dropping
    the PER is equivalent (each shipment's row is its depot's row)."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT PER D.depotID: SUM(ship) BY (D.depotID) <= D.capacity
        MAXIMIZE SUM(w * ship)""", "shipmentID", "ship")
    result = _oracle_depot_capacity(oracle_solver)
    assert result.status == SolverStatus.OPTIMAL
    assert [ship for _, ship in got] == [3, 3, 4, 6, 2, 7]
    assert _weighted(got) == pytest.approx(result.objective_value)


@pytest.mark.per_clause
@pytest.mark.correctness
def test_anr4_relation_key_expands_to_its_columns(decidb_cli, oracle_solver):
    """p7: `PER D: SUM(ship) BY (D) <= D.capacity` names the relation, which expands
    to all of Depot's columns -- the same key as D.depotID, the same rows. Reading
    `BY (D)` as the whole input would cap the network at 8."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT PER D: SUM(ship) BY (D) <= D.capacity MAXIMIZE SUM(w * ship)""",
        "shipmentID", "ship")
    result = _oracle_depot_capacity(oracle_solver)
    assert result.status == SolverStatus.OPTIMAL
    assert [ship for _, ship in got] == [3, 3, 4, 6, 2, 7]
    assert _weighted(got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.correctness
def test_anr5_tuple_per_local_aggregation_with_the_depots_share(decidb_cli, oracle_solver):
    """p21: `ship <= D.maxShipmentShare * SUM(ship) BY (D.depotID)` -- the factor is
    one value per reduced group (§4). D2's share 0.6 lets S4 reach 3 (1.5 * S5);
    a constant 0.5 share everywhere would give 16, not 17; D3's lone shipment is 0.
    A global 0.5 * SUM(ship) would ship all 27."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT ship <= D.maxShipmentShare * SUM(ship) BY (D.depotID)
        MAXIMIZE SUM(ship)""", "shipmentID", "ship")

    _ship_model(oracle_solver, "anr5")
    for s, (d, *_) in _SHIP.items():
        share = _DEPOT[d][5]
        row = {f"ship_{t}": -share for t in _ships_at(d)}
        row[f"ship_{s}"] += 1.0
        oracle_solver.add_constraint(row, "<=", 0.0)
    oracle_solver.set_objective({f"ship_{s}": 1.0 for s in _SHIP}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [ship for _, ship in got] == [5, 3, 4, 3, 2, 0]
    assert sum(ship for _, ship in got) == pytest.approx(result.objective_value)


@pytest.mark.var_real
@pytest.mark.per_clause
@pytest.mark.correctness
def test_anr6_two_data_reducers_with_different_keys(decidb_cli, oracle_solver):
    """p22: 10% of the region's demand plus 2% of the country's, two BY keys in one
    body. D1: 0.1 * 20 + 0.02 * 12 = 2.24; both reducers on the depot's own rows
    would give 0.12 * 12 = 1.44, swapped keys 0.1 * 12 + 0.02 * 20 = 1.6."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT DISTINCT D.depotID, reserve {_FROM} DECIDE PER D.depotID: reserve(REAL)
        SUCH THAT PER D.depotID: reserve >= 0.10 * SUM(S.demand) BY (D.region)
                                        + 0.02 * SUM(S.demand) BY (D.country)
        MINIMIZE SUM(PER D.depotID: reserve)""", "depotID", "reserve")

    oracle_solver.create_model("anr6_data")
    region = {"North": 20, "South": 7}
    country = {"UAE": 12, "KSA": 15}
    for d, (reg, cty, *_) in _DEPOT.items():
        oracle_solver.add_variable(f"reserve_{d}", VarType.CONTINUOUS, lb=0.0, ub=100.0)
        oracle_solver.add_constraint({f"reserve_{d}": 1.0}, ">=", 0.10 * region[reg] + 0.02 * country[cty])
    oracle_solver.set_objective({f"reserve_{d}": 1.0 for d in _DEPOT}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [r for _, r in got] == pytest.approx([2.24, 2.30, 1.00])
    assert sum(r for _, r in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_anr6_two_decision_reducers_with_different_keys(decidb_cli, oracle_solver):
    """p22 with decisions: `SUM(ship) BY (D.depotID) <= 0.6 * SUM(ship) BY (D.region)`
    keeps the two reducers distinct terms (§4). D3 is alone in the South, so it must
    ship 0; merging the terms into one keyed sum would force every depot to 0, and
    reading the right side BY () would let S6 ship 7."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT PER D.depotID: SUM(ship) BY (D.depotID) <= 0.6 * SUM(ship) BY (D.region)
        MAXIMIZE SUM(w * ship)""", "shipmentID", "ship")

    obj = _ship_model(oracle_solver, "anr6_region")
    for d, (reg, *_) in _DEPOT.items():
        row = {f"ship_{s}": -0.6 for s, v in _SHIP.items() if _DEPOT[v[0]][0] == reg}
        for s in _ships_at(d):
            row[f"ship_{s}"] += 1.0
        oracle_solver.add_constraint(row, "<=", 0.0)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [ship for _, ship in got] == [5, 3, 4, 6, 2, 0]
    assert _weighted(got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.cons_aggregate
@pytest.mark.per_clause
@pytest.mark.correctness
def test_anr6_local_and_global_reducer_of_one_decision(decidb_cli, oracle_solver):
    """p16/p22: `SUM(ship) BY (D.depotID) <= 0.35 * SUM(ship) BY ()` -- no depot may
    carry more than 35% of the network. D1 is squeezed to 8 (its lightest shipment
    drops to 1); folding BY () into the depot sum, or reading it BY (D.region),
    gives all zeros."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT PER D.depotID: SUM(ship) BY (D.depotID) <= 0.35 * SUM(ship) BY ()
        MAXIMIZE SUM(w * ship)""", "shipmentID", "ship")

    obj = _ship_model(oracle_solver, "anr6_global")
    for d in _DEPOT:
        row = {f"ship_{s}": -0.35 for s in _SHIP}
        for s in _ships_at(d):
            row[f"ship_{s}"] += 1.0
        oracle_solver.add_constraint(row, "<=", 0.0)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [ship for _, ship in got] == [1, 3, 4, 6, 2, 7]
    assert _weighted(got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.when
@pytest.mark.per_clause
@pytest.mark.correctness
def test_anr7_filtered_local_aggregation(decidb_cli, oracle_solver):
    """p23: `SUM(WHEN S.priority: ship) BY (D.depotID) <= D.priorityCapacity` counts
    only priority shipments. The non-priority S2 ships its full 3 beside D1's cap
    of 6; an unfiltered sum would hold D1's three shipments to 6 together (S1 0,
    S2 2) and D2's to 4 (S4 2)."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT PER D.depotID: SUM(WHEN S.priority: ship) BY (D.depotID) <= D.priorityCapacity
        MAXIMIZE SUM(w * ship)""", "shipmentID", "ship")
    result = _oracle_priority_capacity(oracle_solver)
    assert result.status == SolverStatus.OPTIMAL
    assert [ship for _, ship in got] == [2, 3, 4, 4, 2, 5]
    assert _weighted(got) == pytest.approx(result.objective_value)


@pytest.mark.when_constraint
@pytest.mark.per_clause
@pytest.mark.correctness
def test_anr7_constraint_when_filters_before_generation(decidb_cli, oracle_solver):
    """§3 filter -> generate: `WHEN S.priority PER D.depotID: SUM(ship) BY (D.depotID)`
    generates from the priority rows only, so its groups are p23's filtered groups
    and the rows agree with the reducer-level WHEN. Dropping the WHEN gives the
    unfiltered answer (S1 0, S2 2, S4 2); dropping the PER is equivalent."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT WHEN S.priority PER D.depotID: SUM(ship) BY (D.depotID) <= D.priorityCapacity
        MAXIMIZE SUM(w * ship)""", "shipmentID", "ship")
    result = _oracle_priority_capacity(oracle_solver)
    assert result.status == SolverStatus.OPTIMAL
    assert [ship for _, ship in got] == [2, 3, 4, 4, 2, 5]
    assert _weighted(got) == pytest.approx(result.objective_value)


@pytest.mark.error
@pytest.mark.error_binder
def test_anr8_derived_by_key_is_refused_by_name(decidb_cli):
    """p24 `BY (D.depotID, S.dispatchDay + S.transitDays)`: a key names columns or
    relations, never an expression (§4) -- not implemented, refused by name."""
    decidb_cli.assert_error(_DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT PER S.shipmentID: ship <= 0.25 * SUM(ship) BY (D.depotID, S.dispatchDay + S.transitDays)
        MAXIMIZE SUM(ship)""", match=r"columns or relations")


@pytest.mark.error
@pytest.mark.error_binder
def test_anr9_nested_reducer_in_a_constraint_is_refused_by_name(decidb_cli):
    """p25 `SUM(WHEN S.priority PER S.customerID: MAX(ship) BY (S.customerID)) BY (D.depotID)`:
    reducers nest in an objective only (§4) -- refused by name."""
    decidb_cli.assert_error(_DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT PER D.depotID: SUM(WHEN S.priority PER S.customerID: MAX(ship) BY (S.customerID))
                                 BY (D.depotID) <= D.priorityCapacity
        MAXIMIZE SUM(ship)""", match=r"Nested reducers")


@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_anr9_nesting_is_available_in_the_objective(decidb_cli, oracle_solver):
    """p25 / §4 matrix #9, §6: reducers nest in an objective. `MINIMIZE MAX(PER
    D.depotID: SUM(ship) BY (D.depotID))` is the busiest depot's load; carrying 12
    in total it is 4 at every depot, and the THEN stage (heaviest weights) makes
    the vector unique. An outer SUM would carry S4 3 / S5 2 / S6 7; a flat MAX(ship)
    spreads 2 per shipment; keying on D.region balances 6 / 6 (S4 4, S6 6)."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT PER (): SUM(ship) >= 12
        MINIMIZE MAX(PER D.depotID: SUM(ship) BY (D.depotID)) THEN MAXIMIZE SUM(w * ship)""",
        "shipmentID", "ship")

    def model(stage):
        obj = _ship_model(oracle_solver, f"anr9_stage{stage}")
        oracle_solver.add_variable("busiest", VarType.CONTINUOUS, lb=0.0, ub=100.0)
        for d in _DEPOT:
            oracle_solver.add_constraint({**{f"ship_{s}": 1.0 for s in _ships_at(d)}, "busiest": -1.0},
                                         "<=", 0.0)
        oracle_solver.add_constraint({f"ship_{s}": 1.0 for s in _SHIP}, ">=", 12.0)
        return obj

    model(1)
    oracle_solver.set_objective({"busiest": 1.0}, ObjSense.MINIMIZE)
    stage1 = oracle_solver.solve()
    assert stage1.status == SolverStatus.OPTIMAL
    obj = model(2)
    oracle_solver.add_constraint({"busiest": 1.0}, "<=", stage1.objective_value)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    stage2 = oracle_solver.solve()
    assert stage2.status == SolverStatus.OPTIMAL

    loads = [sum(ship for s, ship in got if _SHIP[s][0] == d) for d in _DEPOT]
    assert max(loads) == pytest.approx(stage1.objective_value)
    assert _weighted(got) == pytest.approx(stage2.objective_value)
    assert [ship for _, ship in got] == [0, 0, 4, 2, 2, 4]


@pytest.mark.edge_case
@pytest.mark.when
@pytest.mark.correctness
@pytest.mark.parametrize("constraint, threshold", [
    ("PER D.depotID: SUM(WHEN S.demand > 6: ship) BY (D.depotID) >= 5", 6),
    ("WHEN S.demand > 100 PER D.depotID: SUM(ship) BY (D.depotID) >= 5", 100),
], ids=["reducer_reads_no_row_at_two_depots", "when_admits_no_row"])
def test_reducer_over_no_rows_imposes_nothing(decidb_cli, oracle_solver, constraint, threshold):
    """p50 NULL policy, §4: a reducer over no rows has no value, so an instance whose
    only reducer reads nothing is not imposed, and a WHEN that admits no row
    generates no instance. Minimizing, only D3 (S6 has demand 7) must carry 5 in the
    first query and nothing is imposed in the second. Reading the empty sum as 0
    makes the first infeasible (0 >= 5 at D1 and D2); dropping the WHEN makes every
    depot carry 5 (total 15)."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT {constraint} MINIMIZE SUM(ship)""", "shipmentID", "ship")

    _ship_model(oracle_solver, "empty")
    for d in _DEPOT:
        admitted = [s for s in _ships_at(d) if _SHIP[s][1] > threshold]
        if admitted:  # an instance over no row is not imposed
            oracle_solver.add_constraint({f"ship_{s}": 1.0 for s in admitted}, ">=", 5.0)
    oracle_solver.set_objective({f"ship_{s}": 1.0 for s in _SHIP}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [ship for _, ship in got] == ([0, 0, 0, 0, 0, 5] if threshold == 6 else [0] * 6)
    assert sum(ship for _, ship in got) == pytest.approx(result.objective_value)


# ===========================================================================
# Conditional generation (deck p58-59): WHEN -> PER -> IF
# ===========================================================================

@pytest.mark.var_boolean
@pytest.mark.var_integer
@pytest.mark.when
@pytest.mark.per_clause
@pytest.mark.correctness
def test_when_per_if_filter_generate_guard(decidb_cli, oracle_solver):
    """§3, p59: `WHEN S.priority PER D.depotID IF NOT open: SUM(ship) BY (D.depotID) <= 0`
    -- a closed depot ships no priority shipment, while non-priority rows escape the
    filter. `open` is declared PER D and read under PER D.depotID through the PRIMARY
    KEY (§3.1). D2's S4 (24) pays its cost 10; D1's 17 and D3's 42 do not. Ignoring
    WHEN opens D1 (23 > 20); ignoring IF ships only the two non-priority rows;
    `IF open` ships everything with every depot closed. Dropping the PER is
    equivalent (a priority row's instance is its depot's)."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship, open {_FROM}
        DECIDE ship(INT) BETWEEN 0 AND S.demand, PER D: open(BOOL)
        SUCH THAT WHEN S.priority PER D.depotID IF NOT open: SUM(ship) BY (D.depotID) <= 0
        MAXIMIZE SUM(w * ship) - SUM(PER D.depotID: D.openCost * open)""", "shipmentID", "ship", "open")

    obj = _ship_model(oracle_solver, "when_per_if")
    for d, (*_, open_cost) in _DEPOT.items():
        oracle_solver.add_variable(f"open_{d}", VarType.BINARY)
        obj[f"open_{d}"] = -float(open_cost)
        priority = {f"ship_{s}": 1.0 for s in _ships_at(d) if _SHIP[s][2]}
        oracle_solver.add_indicator_constraint(f"open_{d}", 0, priority, "<=", 0.0)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [(ship, bool(o)) for _, ship, o in got] == [
        (0, False), (3, False), (0, False), (6, True), (2, True), (0, False)]
    value = _weighted([(s, ship) for s, ship, _ in got]) - sum(
        _DEPOT[d][6] for d in _DEPOT if any(o for s, _, o in got if _SHIP[s][0] == d))
    assert value == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_p58_text_status_guards_each_depot(decidb_cli, oracle_solver):
    """p58: `PER D.depotID: status(TEXT IN [...])` and `PER D.depotID IF status = 'open':
    SUM(ship) BY (D.depotID) <= D.capacity`, plus: an open depot is fully used, a
    closed one ships nothing, one in repair at most its priority capacity, and the
    network carries 15. D2 opens (exactly 8), D1 and D3 go to repair; every other
    status is infeasible or worse for those loads. Imposing every guarded row
    unconditionally is infeasible (a depot cannot be both full and empty); dropping
    the guarded rows ships the 15 heaviest units (S4 6, S5 2, S6 7); swapping the
    'open' and 'repair' guards opens D1 and D3 instead."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship, status {_FROM}
        DECIDE ship(INT) BETWEEN 0 AND S.demand, PER D.depotID: status(TEXT IN ['closed', 'open', 'repair'])
        SUCH THAT PER D.depotID IF status = 'open': SUM(ship) BY (D.depotID) <= D.capacity
              AND PER D.depotID IF status = 'open': SUM(ship) BY (D.depotID) >= D.capacity
              AND PER D.depotID IF status = 'closed': SUM(ship) BY (D.depotID) <= 0
              AND PER D.depotID IF status = 'repair': SUM(ship) BY (D.depotID) <= D.priorityCapacity
              AND PER (): SUM(ship) <= 15
        MAXIMIZE SUM(w * ship)""", "shipmentID", "ship", "status")

    obj = _ship_model(oracle_solver, "p58")
    for d, (_, _, cap, prio_cap, *_) in _DEPOT.items():
        load = {f"ship_{s}": 1.0 for s in _ships_at(d)}
        for value in ("open", "closed", "repair"):
            oracle_solver.add_variable(f"{value}_{d}", VarType.BINARY)
        oracle_solver.add_constraint({f"{v}_{d}": 1.0 for v in ("open", "closed", "repair")}, "=", 1.0)
        oracle_solver.add_indicator_constraint(f"open_{d}", 1, load, "<=", float(cap))
        oracle_solver.add_indicator_constraint(f"open_{d}", 1, load, ">=", float(cap))
        oracle_solver.add_indicator_constraint(f"closed_{d}", 1, load, "<=", 0.0)
        oracle_solver.add_indicator_constraint(f"repair_{d}", 1, load, "<=", float(prio_cap))
    oracle_solver.add_constraint({f"ship_{s}": 1.0 for s in _SHIP}, "<=", 15.0)
    oracle_solver.set_objective(obj, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [ship for _, ship, _ in got] == [0, 0, 2, 6, 2, 5]
    assert {_SHIP[s][0]: st for s, _, st in got} == {"D1": "repair", "D2": "open", "D3": "repair"}
    assert _weighted([(s, ship) for s, ship, _ in got]) == pytest.approx(result.objective_value)


# ===========================================================================
# Frame examples (deck p44-56)
# ===========================================================================

@pytest.mark.var_integer
@pytest.mark.correctness
def test_frame1_balance_reads_the_previous_periods_inventory(decidb_cli, oracle_solver):
    """p44: `inventory = AT(PREVIOUS ELSE 2: inventory) OVER (period WITHIN product) +
    produce - demand`, one balance per tuple, opening stock 2 (the deck's `ELSE
    P.openingStock` reads a column, which ELSE does not take yet). B must carry 2
    units into period 2 (demand 6 > rate 4). Opening from 0 (ELSE 0, or CYCLIC
    reading the empty period 3) gives A1 (3, 0) and B1 (4, 2); DESC walks the
    timeline backwards; without WITHIN a period holds two rows and AT is refused."""
    got = _rows(decidb_cli, _PLAN + f"""
        SELECT product, period, produce, inventory {_PFROM}
        DECIDE produce(INT) BETWEEN 0 AND max_rate, inventory(INT) BETWEEN 0 AND warehouse_cap
        SUCH THAT inventory = AT(PREVIOUS ELSE 2: inventory) OVER (period WITHIN product) + produce - demand
        MINIMIZE SUM(hold_cost * inventory + run_cost * produce)""", "product", "period", "produce", "inventory")

    oracle_solver.create_model("frame1")
    obj = {}
    for (p, t), demand in _DEMAND.items():
        _, _, rate, cap, _, hold, run = _POLICY[p]
        oracle_solver.add_variable(f"produce_{p}{t}", VarType.INTEGER, lb=0.0, ub=float(rate))
        oracle_solver.add_variable(f"inv_{p}{t}", VarType.INTEGER, lb=0.0, ub=float(cap))
        obj[f"produce_{p}{t}"], obj[f"inv_{p}{t}"] = float(run), float(hold)
        row = {f"inv_{p}{t}": 1.0, f"produce_{p}{t}": -1.0}
        if t > 1:
            row[f"inv_{p}{t - 1}"] = -1.0
        oracle_solver.add_constraint(row, "=", (2 if t == 1 else 0) - demand)
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    plan = {(p, t): (pr, inv) for p, t, pr, inv in got}
    assert [plan[k] for k in sorted(_DEMAND)] == [(1, 0), (5, 0), (4, 0), (2, 2), (4, 0), (1, 0)]
    cost = sum(pr * _POLICY[p][6] + inv * _POLICY[p][5] for (p, _), (pr, inv) in plan.items())
    assert cost == pytest.approx(result.objective_value)


@pytest.mark.error
@pytest.mark.error_binder
def test_frame1_else_over_a_column_is_refused_by_name(decidb_cli):
    """p44 `AT(PREVIOUS ELSE opening_stock: inventory)`: a non-constant ELSE is not
    implemented (§5) and is refused by name."""
    decidb_cli.assert_error(_PLAN + f"""
        SELECT product, period, produce {_PFROM}
        DECIDE produce(INT) BETWEEN 0 AND max_rate, inventory(INT) BETWEEN 0 AND warehouse_cap
        SUCH THAT inventory = AT(PREVIOUS ELSE opening_stock: inventory) OVER (period WITHIN product) + produce - demand
        MINIMIZE SUM(produce)""", match=r"ELSE value is a constant")


@pytest.mark.var_integer
@pytest.mark.correctness
def test_frame2_rolling_demand_range_truncates_at_the_edges(decidb_cli, oracle_solver):
    """p45: `SUM(FROM PREVIOUS TO NEXT: demand) OVER (period WITHIN product)` is the
    three-period demand around each row, summed over the positions that exist. Here
    it bounds production: `produce <= 12 - <rolling demand>`. A1 reads 3 + 5 = 8, so
    4; skipping edge instances (ALL) would give A1 its rate 6; reading the frame as
    the product's whole demand (12, as CYCLIC does) would give A1 0; without WITHIN
    each position holds both products and the bound goes negative (infeasible)."""
    got = _rows(decidb_cli, _PLAN + f"""
        SELECT product, period, produce {_PFROM} DECIDE produce(INT) BETWEEN 0 AND max_rate
        SUCH THAT produce <= 12 - SUM(FROM PREVIOUS TO NEXT: demand) OVER (period WITHIN product)
        MAXIMIZE SUM(produce)""", "product", "period", "produce")

    oracle_solver.create_model("frame2")
    for (p, t) in _DEMAND:
        oracle_solver.add_variable(f"produce_{p}{t}", VarType.INTEGER, lb=0.0, ub=float(_POLICY[p][2]))
        around = sum(_DEMAND.get((p, u), 0) for u in (t - 1, t, t + 1))
        oracle_solver.add_constraint({f"produce_{p}{t}": 1.0}, "<=", 12.0 - around)
    oracle_solver.set_objective({f"produce_{p}{t}": 1.0 for (p, t) in _DEMAND}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [pr for _, _, pr in got] == [4, 0, 3, 4, 3, 4]
    assert sum(pr for _, _, pr in got) == pytest.approx(result.objective_value)


# (product, period): cost -- A has four periods so an edge instance differs from a middle one
_ROLL = {("A", 1): 2, ("A", 2): 1, ("A", 3): 3, ("A", 4): 1, ("B", 1): 2, ("B", 2): 1}
_ROLL_SQL = """
CREATE TEMP TABLE sched(product VARCHAR, period INT, cost INT, PRIMARY KEY (product, period));
INSERT INTO sched VALUES ('A',1,2),('A',2,1),('A',3,3),('A',4,1),('B',1,2),('B',2,1);
"""
_AROUND, _BEFORE = "FROM PREVIOUS TO NEXT", "FROM 2 PREVIOUS TO PREVIOUS"
_OFFSETS = {_AROUND: (-1, 0, 1), _BEFORE: (-2, -1)}


@pytest.mark.var_integer
@pytest.mark.edge_case
@pytest.mark.correctness
@pytest.mark.parametrize("window, policy, order, expected", [
    (_AROUND, "", "period WITHIN product", [0, 5, 0, 5, 0, 5]),
    (_AROUND, " ELSE NULL", "period WITHIN product", [0, 5, 0, 5, 0, 5]),
    (_AROUND, " EVERY 1 ELSE NULL", "period ASC WITHIN product", [0, 5, 0, 5, 0, 5]),
    (_AROUND, " ELSE 1", "period WITHIN product", [0, 5, 0, 4, 0, 4]),
    (_AROUND, " ALL", "period WITHIN product", [0, 5, 0, 0, 0, 0]),
    (_BEFORE, "", "period WITHIN product", [5, 5, 0, 0, 5, 0]),
    (_BEFORE, " ELSE NULL", "period WITHIN product", [5, 5, 0, 0, 5, 0]),
    (_BEFORE, " ALL", "period WITHIN product", [0, 5, 0, 0, 0, 0]),
], ids=["truncate", "else_null", "fully_explicit", "else_1", "all",
        "no_position_truncate", "no_position_else_null", "no_position_all"])
def test_frame6_boundary_policies(decidb_cli, oracle_solver, window, policy, order, expected):
    """p49/p50: `SUM(<window> [ELSE v | ALL]: produce) >= 5`. The default (spelled
    `ELSE NULL`, or fully explicit with `EVERY 1` and `ASC`) sums the positions that
    exist, so A's two edge instances force two disjoint pairs (cost 10); `ELSE 1`
    fills each missing position with 1 (cost 9); `ALL` imposes only complete frames,
    so A's middle pair needs one unit and B nothing (cost 5). On the around window,
    which always holds the current position, ELSE NULL and ELSE 0 coincide (and ASC
    and DESC, by symmetry); the before window selects no position at a timeline's
    first period, where the default skips the instance (p50) while ELSE 0 would read
    0 >= 5 and be infeasible."""
    got = _rows(decidb_cli, _ROLL_SQL + f"""
        SELECT product, period, produce FROM sched DECIDE produce(INT) BETWEEN 0 AND 9
        SUCH THAT SUM({window}{policy}: produce) OVER ({order}) >= 5
        MINIMIZE SUM(cost * produce)""", "product", "period", "produce")

    oracle_solver.create_model("frame6")
    for (p, t) in _ROLL:
        oracle_solver.add_variable(f"produce_{p}{t}", VarType.INTEGER, lb=0.0, ub=9.0)
    for (p, t) in _ROLL:
        offsets = _OFFSETS[window]
        present = [t + d for d in offsets if (p, t + d) in _ROLL]
        missing = len(offsets) - len(present)
        if policy == " ALL" and missing:
            continue  # an incomplete frame is NULL: the instance is skipped
        fill = missing if policy == " ELSE 1" else 0
        if not present and not fill:
            continue  # no position left: NULL, skipped
        oracle_solver.add_constraint({f"produce_{p}{u}": 1.0 for u in present}, ">=", 5.0 - fill)
    oracle_solver.set_objective({f"produce_{p}{t}": float(c) for (p, t), c in _ROLL.items()},
                                ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [pr for _, _, pr in got] == expected
    assert sum(pr * _ROLL[(p, t)] for p, t, pr in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.edge_case
@pytest.mark.correctness
def test_p50_non_reduced_null_skips_the_instance(decidb_cli, oracle_solver):
    """p50: `AT(PREVIOUS: inventory) OVER (period WITHIN product) >= 3` has no previous
    row at the first period, so that instance is skipped; periods 1 and 2 are read
    by the later instances and held at 3, period 3 is free. Reading the missing
    position as 0 would make the query infeasible; CYCLIC would hold period 3 too."""
    got = _rows(decidb_cli, _PLAN + f"""
        SELECT product, period, inventory {_PFROM} DECIDE inventory(INT) BETWEEN 0 AND warehouse_cap
        SUCH THAT AT(PREVIOUS: inventory) OVER (period WITHIN product) >= 3
        MINIMIZE SUM(inventory)""", "product", "period", "inventory")

    oracle_solver.create_model("p50")
    for (p, t) in _DEMAND:
        oracle_solver.add_variable(f"inv_{p}{t}", VarType.INTEGER, lb=0.0, ub=20.0)
        if (p, t + 1) in _DEMAND:  # this row is the PREVIOUS of a later instance
            oracle_solver.add_constraint({f"inv_{p}{t}": 1.0}, ">=", 3.0)
    oracle_solver.set_objective({f"inv_{p}{t}": 1.0 for (p, t) in _DEMAND}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [inv for _, _, inv in got] == [3, 3, 0, 3, 3, 0]
    assert sum(inv for _, _, inv in got) == pytest.approx(result.objective_value)


# (product, period): dueDate -- the due-date order differs from the period order
_SCHED = {("A", 1): 30, ("A", 2): 10, ("A", 3): 20, ("B", 1): 20, ("B", 2): 10}
_SCHED_SQL = """
CREATE TEMP TABLE sched(product VARCHAR, period INT, dueDate INT, PRIMARY KEY (product, period));
INSERT INTO sched VALUES ('A',1,30),('A',2,10),('A',3,20),('B',1,20),('B',2,10);
"""


@pytest.mark.var_integer
@pytest.mark.correctness
def test_frame4_two_orders_in_one_body(decidb_cli, oracle_solver):
    """p47 without ABS (not available over a frame): the previous production by period
    minus the previous by due date, each frame with its own OVER (p56). Only A1 and
    B1 are bounded (<= 2): the rows after them by period, A2 and B2, come first by
    due date and read ELSE 0 there. Collapsing the two frames onto one order cancels
    the terms, and dropping ELSE skips those instances; both let every row reach 9."""
    got = _rows(decidb_cli, _SCHED_SQL + """
        SELECT product, period, produce FROM sched DECIDE produce(INT) BETWEEN 0 AND 9
        SUCH THAT AT(PREVIOUS ELSE 0: produce) OVER (period WITHIN product)
                - AT(PREVIOUS ELSE 0: produce) OVER (dueDate WITHIN product) <= 2
        MAXIMIZE SUM(produce)""", "product", "period", "produce")

    oracle_solver.create_model("frame4")
    for (p, t) in _SCHED:
        oracle_solver.add_variable(f"produce_{p}{t}", VarType.INTEGER, lb=0.0, ub=9.0)
    for (p, t), due in _SCHED.items():
        row = {}
        if (p, t - 1) in _SCHED:
            row[f"produce_{p}{t - 1}"] = 1.0
        earlier = [(d, u) for (q, u), d in _SCHED.items() if q == p and d < due]
        if earlier:
            _, u = max(earlier)
            row[f"produce_{p}{u}"] = row.get(f"produce_{p}{u}", 0.0) - 1.0
        row = {k: v for k, v in row.items() if v}
        if row:
            oracle_solver.add_constraint(row, "<=", 2.0)
    oracle_solver.set_objective({f"produce_{p}{t}": 1.0 for (p, t) in _SCHED}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [pr for _, _, pr in got] == [2, 9, 9, 2, 9]
    assert sum(pr for _, _, pr in got) == pytest.approx(result.objective_value)


@pytest.mark.error
@pytest.mark.error_binder
def test_frame4_abs_over_a_frame_is_refused_by_name(decidb_cli):
    """p47 verbatim, `ABS(AT(...) OVER (period) - AT(...) OVER (dueDate))`: ABS over a
    frame is not implemented (§5) and is refused by name."""
    decidb_cli.assert_error(_SCHED_SQL + """
        SELECT product, period, produce FROM sched DECIDE produce(INT) BETWEEN 0 AND 9
        SUCH THAT ABS(AT(PREVIOUS ELSE 0: produce) OVER (period WITHIN product)
                    - AT(PREVIOUS ELSE 0: produce) OVER (dueDate WITHIN product)) <= 2
        MAXIMIZE SUM(produce)""", match=r"ABS over a frame")


@pytest.mark.var_integer
@pytest.mark.when
@pytest.mark.correctness
def test_frame5_cyclic_wraps_the_first_period_under_when(decidb_cli, oracle_solver):
    """p48: `WHEN cyclicPlan: inventory - AT(PREVIOUS: inventory) OVER (period CYCLIC
    WITHIN product) BETWEEN -2 AND 2` (ABS is written as BETWEEN, whose prefix covers
    both comparisons) plus a keyed `PER product: AT(FIRST: ...) <= 1` (p52). For the
    cyclic A the wrap bounds period 3 by period 1 + 2 = 3; without CYCLIC it would
    climb to 5; without WHEN the non-cyclic B would be chained too (B sums 7, not
    19); a WHEN covering only the upper comparison would chain B from below (1, 5, 3)."""
    got = _rows(decidb_cli, """
        CREATE TEMP TABLE plan(product VARCHAR, period INT, cyclicPlan BOOLEAN, PRIMARY KEY (product, period));
        INSERT INTO plan VALUES ('A',1,true),('A',2,true),('A',3,true),('B',1,false),('B',2,false),('B',3,false);
        SELECT product, period, inventory FROM plan DECIDE inventory(INT) BETWEEN 0 AND 9
        SUCH THAT WHEN cyclicPlan: inventory - AT(PREVIOUS: inventory) OVER (period CYCLIC WITHIN product) BETWEEN -2 AND 2
              AND PER product: AT(FIRST: inventory) OVER (period WITHIN product) <= 1
        MAXIMIZE SUM(inventory)""", "product", "period", "inventory")

    oracle_solver.create_model("frame5")
    for p in ("A", "B"):
        for t in (1, 2, 3):
            oracle_solver.add_variable(f"inv_{p}{t}", VarType.INTEGER, lb=0.0, ub=9.0)
        oracle_solver.add_constraint({f"inv_{p}1": 1.0}, "<=", 1.0)
    for t, prev in ((1, 3), (2, 1), (3, 2)):  # A only: the ring 3 -> 1 -> 2 -> 3
        oracle_solver.add_constraint({f"inv_A{t}": 1.0, f"inv_A{prev}": -1.0}, "<=", 2.0)
        oracle_solver.add_constraint({f"inv_A{t}": 1.0, f"inv_A{prev}": -1.0}, ">=", -2.0)
    oracle_solver.set_objective({f"inv_{p}{t}": 1.0 for p in "AB" for t in (1, 2, 3)}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    assert [inv for _, _, inv in got] == [1, 3, 3, 1, 9, 9]
    assert sum(inv for _, _, inv in got) == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.per_clause
@pytest.mark.correctness
def test_p52_keyed_per_reads_the_first_of_its_own_timeline(decidb_cli, oracle_solver):
    """p52: `PER P: floor <= AT(FIRST: demand) OVER (period WITHIN P)` -- the key
    determines the WITHIN partition, which is all an absolute selector needs (§5).
    A's first demand is 3 and B's 2; LAST (or a DESC walk) would read 4 and 1;
    without WITHIN the first period holds both products and AT is refused."""
    got = _rows(decidb_cli, _PLAN + f"""
        SELECT product, period, floor {_PFROM} DECIDE PER P: floor(INT) BETWEEN 0 AND 20
        SUCH THAT PER P: floor <= AT(FIRST: demand) OVER (period WITHIN P)
        MAXIMIZE SUM(PER P: floor)""", "product", "period", "floor")

    oracle_solver.create_model("p52")
    for p in ("A", "B"):
        oracle_solver.add_variable(f"floor_{p}", VarType.INTEGER, lb=0.0, ub=20.0)
        oracle_solver.add_constraint({f"floor_{p}": 1.0}, "<=", float(_DEMAND[(p, 1)]))
    oracle_solver.set_objective({"floor_A": 1.0, "floor_B": 1.0}, ObjSense.MAXIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    floors = {p: f for p, _, f in got}
    assert floors == {"A": 3, "B": 2}
    assert sum(floors.values()) == pytest.approx(result.objective_value)


@pytest.mark.error
@pytest.mark.error_binder
@pytest.mark.per_clause
def test_p53_per_coarser_than_within_is_refused(decidb_cli):
    """p53: `PER category` with `WITHIN product` -- a category spans several product
    timelines, so the partition is not determined by the key (§5)."""
    decidb_cli.assert_error("""
        CREATE TEMP TABLE cat(category VARCHAR, product VARCHAR, period INT, cap INT, PRIMARY KEY (product, period));
        INSERT INTO cat VALUES ('c','A',1,5),('c','A',2,2),('c','B',1,4);
        SELECT product, period, y FROM cat DECIDE PER category: y(INT) BETWEEN 0 AND 20
        SUCH THAT PER category: y <= AT(FIRST: cap) OVER (period WITHIN product)
        MAXIMIZE SUM(y)""", match=r"WITHIN \(product\) partition")


# ===========================================================================
# Objectives (deck p62-63)
# ===========================================================================

_NURSE = """
SELECT shift, work, short, ot FROM (VALUES ('mon', 5, 3, 2), ('tue', 4, 4, 1), ('wed', 3, 2, 3)) s(shift, need, regular, pref)
DECIDE work(INT) BETWEEN 0 AND 6, short(INT) BETWEEN 0 AND 10, ot(INT) BETWEEN 0 AND 6
SUCH THAT work + short >= need AND ot >= work - regular AND PER (): SUM(work) <= 10
"""
_SHIFTS = {"mon": (5, 3, 2), "tue": (4, 4, 1), "wed": (3, 2, 3)}  # need, regular, pref


@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_p63_lexicographic_nurse_schedule(decidb_cli, oracle_solver):
    """p63: MINIMIZE understaffing THEN MINIMIZE overtime THEN MAXIMIZE preference.
    10 staff for 12 needed leaves 2 short whatever the split (stage 1); the 9
    regular hours leave 1 overtime hour, on mon (4, 4, 2) or on wed (3, 4, 3)
    (stage 2 ties); wed's higher preference takes it (stage 3: 19 against 18).
    Dropping stage 2 lets preference pull mon to 5 and tue to 2 (21, but 3 overtime
    hours); dropping stage 3 leaves the tie to the solver (mon here); putting
    overtime first staffs no overtime at all (3 short)."""
    got = _rows(decidb_cli, _NURSE + "MINIMIZE SUM(short) THEN MINIMIZE SUM(ot) THEN MAXIMIZE SUM(pref * work)",
                "shift", "work", "short", "ot")
    explicit = _rows(decidb_cli, _NURSE + "minimize per (): sum(short) then minimize per (): sum(ot) "
                     "then maximize per (): sum(pref * work)", "shift", "work", "short", "ot")

    frozen = []
    for stage, (coeffs, sense) in enumerate([
            ({f"short_{s}": 1.0 for s in _SHIFTS}, ObjSense.MINIMIZE),
            ({f"ot_{s}": 1.0 for s in _SHIFTS}, ObjSense.MINIMIZE),
            ({f"work_{s}": float(v[2]) for s, v in _SHIFTS.items()}, ObjSense.MAXIMIZE)]):
        oracle_solver.create_model(f"nurse_stage{stage + 1}")
        for s, (need, regular, _) in _SHIFTS.items():
            oracle_solver.add_variable(f"work_{s}", VarType.INTEGER, lb=0.0, ub=6.0)
            oracle_solver.add_variable(f"short_{s}", VarType.INTEGER, lb=0.0, ub=10.0)
            oracle_solver.add_variable(f"ot_{s}", VarType.INTEGER, lb=0.0, ub=6.0)
            oracle_solver.add_constraint({f"work_{s}": 1.0, f"short_{s}": 1.0}, ">=", float(need))
            oracle_solver.add_constraint({f"ot_{s}": 1.0, f"work_{s}": -1.0}, ">=", -float(regular))
        oracle_solver.add_constraint({f"work_{s}": 1.0 for s in _SHIFTS}, "<=", 10.0)
        for c, sense_e, value in frozen:  # hold every earlier stage at its optimum
            oracle_solver.add_constraint(c, "<=" if sense_e is ObjSense.MINIMIZE else ">=", value)
        oracle_solver.set_objective(coeffs, sense)
        result = oracle_solver.solve()
        assert result.status == SolverStatus.OPTIMAL, stage
        frozen.append((coeffs, sense, result.objective_value))

    assert got == explicit
    assert got == [("mon", 3, 2, 0), ("tue", 4, 0, 0), ("wed", 3, 0, 1)]
    assert (sum(short for _, _, short, _ in got), sum(ot for *_, ot in got)) == (
        pytest.approx(frozen[0][2]), pytest.approx(frozen[1][2]))
    assert sum(w * _SHIFTS[s][2] for s, w, _, _ in got) == pytest.approx(frozen[2][2])


@pytest.mark.var_integer
@pytest.mark.obj_minimize
@pytest.mark.correctness
def test_p62_arithmetic_over_query_wide_decisions_balances_the_depots(decidb_cli, oracle_solver):
    """§6: `MINIMIZE hi - lo` over two `PER ()` decisions is an objective (linear
    arithmetic over query-wide decisions), and `PER D.depotID: SUM(ship) BY (D.depotID)
    BETWEEN lo AND hi` is a BETWEEN body under a key with decision bounds. Carrying
    at least 20 with loads capped 12 / 8 / 7, the spread reaches 0 only at 7 each.
    Minimizing hi alone would leave lo at 0; reading the load BY () would balance
    the network total at 20. Dropping the PER is equivalent (a shipment's row is
    its depot's row)."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship, lo, hi {_FROM}
        DECIDE ship(INT) BETWEEN 0 AND S.demand, PER (): lo(INT), PER (): hi(INT)
        SUCH THAT PER D.depotID: SUM(ship) BY (D.depotID) BETWEEN lo AND hi AND PER (): SUM(ship) >= 20
        MINIMIZE hi - lo""", "shipmentID", "ship", "lo", "hi")

    _ship_model(oracle_solver, "hilo")
    oracle_solver.add_variable("lo", VarType.INTEGER, lb=0.0, ub=30.0)
    oracle_solver.add_variable("hi", VarType.INTEGER, lb=0.0, ub=30.0)
    for d in _DEPOT:
        load = {f"ship_{s}": 1.0 for s in _ships_at(d)}
        oracle_solver.add_constraint({**load, "lo": -1.0}, ">=", 0.0)
        oracle_solver.add_constraint({**load, "hi": -1.0}, "<=", 0.0)
    oracle_solver.add_constraint({f"ship_{s}": 1.0 for s in _SHIP}, ">=", 20.0)
    oracle_solver.set_objective({"hi": 1.0, "lo": -1.0}, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    bounds = {(lo, hi) for _, _, lo, hi in got}
    assert bounds == {(7, 7)}
    loads = {d: sum(ship for s, ship, _, _ in got if _SHIP[s][0] == d) for d in _DEPOT}
    assert loads == {"D1": 7, "D2": 7, "D3": 7}
    (lo, hi), = bounds
    assert hi - lo == pytest.approx(result.objective_value)


@pytest.mark.var_integer
@pytest.mark.correctness
def test_p62_satisfy_returns_a_feasible_assignment(decidb_cli, oracle_solver):
    """p62: SATISFY asks for any feasible point, so the point is checked against the
    constraints, not against a value. Every shipment moves at least 1 and every
    depot carries at least its priority capacity (6 / 4 / 5): the all-lower-bound
    point (depot loads 3 / 2 / 1) that the solver returns once the constraint is
    dropped violates all three."""
    got = _rows(decidb_cli, _DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 1 AND S.demand
        SUCH THAT PER D.depotID: SUM(ship) BY (D.depotID) >= D.priorityCapacity SATISFY""",
        "shipmentID", "ship")

    oracle_solver.create_model("satisfy")
    for s, (_, demand, _, _) in _SHIP.items():
        oracle_solver.add_variable(f"ship_{s}", VarType.INTEGER, lb=1.0, ub=float(demand))
    for d, (_, _, _, prio_cap, *_) in _DEPOT.items():
        oracle_solver.add_constraint({f"ship_{s}": 1.0 for s in _ships_at(d)}, ">=", float(prio_cap))
    oracle_solver.set_objective({}, ObjSense.MINIMIZE)
    assert oracle_solver.solve().status == SolverStatus.OPTIMAL

    by_ship = dict(got)
    assert all(1 <= by_ship[s] <= _SHIP[s][1] for s in _SHIP)
    for d, (_, _, _, prio_cap, *_) in _DEPOT.items():
        assert sum(by_ship[s] for s in _ships_at(d)) >= prio_cap


@pytest.mark.error
@pytest.mark.error_parser
def test_p62_satisfy_and_a_stage_are_mutually_exclusive(decidb_cli):
    """p62: the two objective forms are mutually exclusive -- SATISFY after a stage is
    a parser error that names SATISFY; two stages without THEN name THEN."""
    decidb_cli.assert_error(_DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT ship <= 5 MAXIMIZE SUM(ship) SATISFY""", match=r"cannot follow an objective")
    decidb_cli.assert_error(_DECK + f"""
        SELECT S.shipmentID, ship {_FROM} DECIDE ship(INT) BETWEEN 0 AND S.demand
        SUCH THAT ship <= 5 MAXIMIZE SUM(ship) MINIMIZE SUM(ship)""", match=r"chained with THEN")


# ===========================================================================
# The production plan end to end (deck p73-75)
# ===========================================================================

@pytest.mark.var_real
@pytest.mark.sql_joins
@pytest.mark.correctness
def test_production_plan_end_to_end(decidb_cli, oracle_solver):
    """p73-75: balance with the previous inventory (the per-product opening stock
    enters through a precomputed column, since ELSE takes a constant), the deck's
    ramp `AT(NEXT ELSE NULL: produce) - produce <= max_ramp` plus a ramp-down row
    `produce - AT(NEXT ELSE NULL: produce) <= max_ramp` (added: on the deck's row
    alone a missing NEXT read as 0 is harmless), opening and closing inventory via
    FIRST / LAST, and the holding-plus-run cost (A 24 + B 15). B's ramp of 1 forbids
    the cheaper (1, 4, 2) plan (13); reading the missing NEXT at the last period as
    0 caps B's closing production at its ramp 1, which is infeasible (B could then
    make at most 3 + 2 + 1 = 6 of the 7 it needs); dropping the FIRST row opens A
    at (1.5, 0.5), dropping the LAST row closes A at (4, 0)."""
    got = _rows(decidb_cli, _PLAN + """
        WITH R AS (SELECT D.product, D.period, D.demand, P.safety_stock, P.max_rate, P.warehouse_cap,
                          P.max_ramp, P.hold_cost, P.run_cost,
                          CASE WHEN D.period = 1 THEN P.opening_stock ELSE 0 END AS opening_in
                   FROM demand_forecast D JOIN product_policy P USING (product))
        SELECT product, period, produce, inventory FROM R
        DECIDE produce(REAL) BETWEEN 0 AND max_rate, inventory(REAL) BETWEEN 0 AND warehouse_cap
        SUCH THAT inventory = AT(PREVIOUS ELSE 0: inventory) OVER (period WITHIN product) + opening_in + produce - demand
              AND AT(NEXT ELSE NULL: produce) OVER (period WITHIN product) - produce <= max_ramp
              AND produce - AT(NEXT ELSE NULL: produce) OVER (period WITHIN product) <= max_ramp
              AND AT(FIRST ELSE NULL: inventory) OVER (period WITHIN product) >= safety_stock
              AND AT(LAST ELSE NULL: inventory) OVER (period WITHIN product) >= safety_stock
        MINIMIZE SUM(hold_cost * inventory + run_cost * produce)""", "product", "period", "produce", "inventory")

    oracle_solver.create_model("plan")
    obj = {}
    for (p, t) in _DEMAND:
        opening, safety, rate, cap, ramp, hold, run = _POLICY[p]
        oracle_solver.add_variable(f"produce_{p}{t}", VarType.CONTINUOUS, lb=0.0, ub=float(rate))
        oracle_solver.add_variable(f"inv_{p}{t}", VarType.CONTINUOUS, lb=0.0, ub=float(cap))
        obj[f"produce_{p}{t}"], obj[f"inv_{p}{t}"] = float(run), float(hold)
    for (p, t), demand in _DEMAND.items():
        opening, safety, rate, cap, ramp, hold, run = _POLICY[p]
        row = {f"inv_{p}{t}": 1.0, f"produce_{p}{t}": -1.0}
        if t > 1:
            row[f"inv_{p}{t - 1}"] = -1.0
        oracle_solver.add_constraint(row, "=", (opening if t == 1 else 0) - demand)
        if (p, t + 1) in _DEMAND:  # no NEXT at the last period: both ramp rows are skipped
            step = {f"produce_{p}{t + 1}": 1.0, f"produce_{p}{t}": -1.0}
            oracle_solver.add_constraint(step, "<=", float(ramp))
            oracle_solver.add_constraint({k: -v for k, v in step.items()}, "<=", float(ramp))
        if t in (1, 3):
            oracle_solver.add_constraint({f"inv_{p}{t}": 1.0}, ">=", float(safety))
    oracle_solver.set_objective(obj, ObjSense.MINIMIZE)
    result = oracle_solver.solve()
    assert result.status == SolverStatus.OPTIMAL

    plan = {(p, t): (pr, inv) for p, t, pr, inv in got}
    assert [plan[k] for k in sorted(_DEMAND)] == pytest.approx(
        [(2, 1), (4, 0), (5, 1), (2, 3), (3, 0), (2, 1)])
    cost = sum(pr * _POLICY[p][6] + inv * _POLICY[p][5] for (p, _), (pr, inv) in plan.items())
    assert cost == pytest.approx(result.objective_value)
