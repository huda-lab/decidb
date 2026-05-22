"""Smoke tests for the quadratic oracle API.

These don't exercise DecidB at all — they only verify that
set_quadratic_objective and add_quadratic_constraint in solver/gurobi_backend.py
solve tiny hand-checked problems correctly, so downstream tests in
test_quadratic.py / test_quadratic_constraints.py can rely on the API.
"""

from __future__ import annotations

import math

import pytest

from solver.types import ObjSense, SolverStatus, VarType


@pytest.mark.quadratic
def test_convex_min_unconstrained(oracle_solver):
    """min (x - 3)^2, x in [0, 10]. Optimum at x=3, value=0."""
    oracle_solver.create_model("qp_convex_min")
    oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=10.0)
    oracle_solver.set_quadratic_objective(
        linear={"x": -6.0},
        quadratic={("x", "x"): 1.0},
        sense=ObjSense.MINIMIZE,
        constant=9.0,
    )
    r = oracle_solver.solve()
    assert r.status == SolverStatus.OPTIMAL
    assert abs(r.objective_value - 0.0) < 1e-4
    assert abs(r.variable_values["x"] - 3.0) < 1e-4


@pytest.mark.quadratic
def test_concave_max_negated(oracle_solver):
    """max -(x - 5)^2, x in [0, 10]. Optimum at x=5, value=0."""
    oracle_solver.create_model("qp_concave_max")
    oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=10.0)
    oracle_solver.set_quadratic_objective(
        linear={"x": 10.0},
        quadratic={("x", "x"): -1.0},
        sense=ObjSense.MAXIMIZE,
        constant=-25.0,
    )
    r = oracle_solver.solve()
    assert r.status == SolverStatus.OPTIMAL
    assert abs(r.objective_value - 0.0) < 1e-4
    assert abs(r.variable_values["x"] - 5.0) < 1e-4


@pytest.mark.quadratic
def test_nonconvex_max_square(oracle_solver):
    """max x^2, x in [0, 4]. Optimum at x=4 (boundary), value=16.

    Non-convex maximization with PSD Q — requires NonConvex=2, which the
    backend sets automatically when any quadratic term is present.
    """
    oracle_solver.create_model("qp_nonconvex_max")
    oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=4.0)
    oracle_solver.set_quadratic_objective(
        linear={},
        quadratic={("x", "x"): 1.0},
        sense=ObjSense.MAXIMIZE,
    )
    r = oracle_solver.solve()
    assert r.status == SolverStatus.OPTIMAL
    assert abs(r.objective_value - 16.0) < 1e-4
    assert abs(r.variable_values["x"] - 4.0) < 1e-4


@pytest.mark.quadratic
def test_quadratic_constraint(oracle_solver):
    """max x + y subject to x^2 + y^2 <= 1, x,y in [0, 1].

    Analytical optimum: x = y = 1/sqrt(2), objective = sqrt(2).
    """
    oracle_solver.create_model("qcp")
    oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=1.0)
    oracle_solver.add_variable("y", VarType.CONTINUOUS, lb=0.0, ub=1.0)
    oracle_solver.add_quadratic_constraint(
        linear={},
        quadratic={("x", "x"): 1.0, ("y", "y"): 1.0},
        sense="<=",
        rhs=1.0,
    )
    oracle_solver.set_objective({"x": 1.0, "y": 1.0}, ObjSense.MAXIMIZE)
    r = oracle_solver.solve()
    assert r.status == SolverStatus.OPTIMAL
    assert abs(r.objective_value - math.sqrt(2)) < 1e-3


@pytest.mark.quadratic
def test_bilinear_objective(oracle_solver):
    """max x * y subject to x + y <= 2, x,y in [0, 2].

    Optimum at x = y = 1, objective = 1.
    """
    oracle_solver.create_model("bilinear")
    oracle_solver.add_variable("x", VarType.CONTINUOUS, lb=0.0, ub=2.0)
    oracle_solver.add_variable("y", VarType.CONTINUOUS, lb=0.0, ub=2.0)
    oracle_solver.add_constraint({"x": 1.0, "y": 1.0}, "<=", 2.0)
    oracle_solver.set_quadratic_objective(
        linear={},
        quadratic={("x", "y"): 1.0},
        sense=ObjSense.MAXIMIZE,
    )
    r = oracle_solver.solve()
    assert r.status == SolverStatus.OPTIMAL
    assert abs(r.objective_value - 1.0) < 1e-4
    assert abs(r.variable_values["x"] - 1.0) < 1e-3
    assert abs(r.variable_values["y"] - 1.0) < 1e-3
