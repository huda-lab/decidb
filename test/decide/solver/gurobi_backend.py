"""Gurobi backend for the oracle solver."""

from __future__ import annotations

import time

from .base import OracleSolver
from .types import ObjSense, SolverResult, SolverStatus, VarType

# Maps our VarType to Gurobi variable type constants.
_GUROBI_VTYPE = {
    VarType.BINARY: "B",
    VarType.INTEGER: "I",
    VarType.CONTINUOUS: "C",
}


class GurobiSolver(OracleSolver):
    """Oracle solver backed by gurobipy."""

    def __init__(self) -> None:
        import gurobipy as gp
        self._gp = gp
        self._model = None
        self._vars: dict[str, object] = {}

    def create_model(self, name: str) -> None:
        gp = self._gp
        env = gp.Env(empty=True)
        env.setParam("OutputFlag", 0)
        env.start()
        self._model = gp.Model(name, env=env)
        self._vars = {}

    def add_variable(
        self,
        name: str,
        var_type: VarType = VarType.BINARY,
        lb: float = 0.0,
        ub: float | None = None,
    ) -> None:
        gp = self._gp
        if ub is None:
            ub = 1.0 if var_type == VarType.BINARY else gp.GRB.INFINITY
        v = self._model.addVar(
            lb=lb,
            ub=ub,
            vtype=_GUROBI_VTYPE[var_type],
            name=name,
        )
        self._vars[name] = v

    def add_constraint(
        self,
        coeffs: dict[str, float],
        sense: str,
        rhs: float,
        name: str = "",
    ) -> None:
        gp = self._gp
        expr = gp.LinExpr()
        for vname, coeff in coeffs.items():
            expr.add(self._vars[vname], coeff)
        if sense == "<=":
            tc = expr <= rhs
        elif sense == ">=":
            tc = expr >= rhs
        elif sense == "=":
            tc = expr == rhs
        else:
            raise ValueError(f"unsupported sense: {sense!r}")
        self._model.addConstr(tc, name=name)

    def set_objective(
        self,
        coeffs: dict[str, float],
        sense: ObjSense,
    ) -> None:
        gp = self._gp
        expr = gp.LinExpr()
        for vname, coeff in coeffs.items():
            expr.add(self._vars[vname], coeff)
        grb_sense = (
            gp.GRB.MAXIMIZE if sense == ObjSense.MAXIMIZE else gp.GRB.MINIMIZE
        )
        self._model.setObjective(expr, grb_sense)

    def set_quadratic_objective(
        self,
        linear: dict[str, float],
        quadratic: dict[tuple[str, str], float],
        sense: ObjSense,
        constant: float = 0.0,
    ) -> None:
        gp = self._gp
        expr = gp.QuadExpr()
        for vname, coeff in linear.items():
            expr.add(self._vars[vname] * coeff)
        merged = _merge_symmetric_quadratic(quadratic)
        for (a, b), coeff in merged.items():
            expr.add(self._vars[a] * self._vars[b] * coeff)
        grb_sense = (
            gp.GRB.MAXIMIZE if sense == ObjSense.MAXIMIZE else gp.GRB.MINIMIZE
        )
        self._model.setObjective(expr, grb_sense)
        if constant:
            self._model.ObjCon = constant
        if merged:
            self._model.Params.NonConvex = 2

    def add_quadratic_constraint(
        self,
        linear: dict[str, float],
        quadratic: dict[tuple[str, str], float],
        sense: str,
        rhs: float,
        name: str = "",
    ) -> None:
        gp = self._gp
        expr = gp.QuadExpr()
        for vname, coeff in linear.items():
            expr.add(self._vars[vname] * coeff)
        merged = _merge_symmetric_quadratic(quadratic)
        for (a, b), coeff in merged.items():
            expr.add(self._vars[a] * self._vars[b] * coeff)
        if sense == "<=":
            tc = expr <= rhs
        elif sense == ">=":
            tc = expr >= rhs
        elif sense == "=":
            tc = expr == rhs
        else:
            raise ValueError(f"unsupported sense: {sense!r}")
        self._model.addQConstr(tc, name=name)
        if merged:
            self._model.Params.NonConvex = 2

    def add_indicator_constraint(
        self,
        indicator_var: str,
        indicator_val: int,
        coeffs: dict[str, float],
        sense: str,
        rhs: float,
        name: str = "",
    ) -> None:
        gp = self._gp
        expr = gp.LinExpr()
        for vname, coeff in coeffs.items():
            expr.add(self._vars[vname], coeff)
        if sense == "<=":
            tc = expr <= rhs
        elif sense == ">=":
            tc = expr >= rhs
        elif sense == "=":
            tc = expr == rhs
        else:
            raise ValueError(f"unsupported sense: {sense!r}")
        self._model.addGenConstrIndicator(
            self._vars[indicator_var], bool(indicator_val), tc, name=name,
        )

    def solve(self, time_limit: float = 60.0) -> SolverResult:
        gp = self._gp
        self._model.setParam("TimeLimit", time_limit)
        self._model.update()

        t0 = time.perf_counter()
        self._model.optimize()
        if self._model.status == gp.GRB.INF_OR_UNBD:
            # Presolve dual reductions blur infeasible vs unbounded; Gurobi's
            # documented recipe is to disable them and re-solve, which yields
            # a definitive INFEASIBLE or UNBOUNDED status.
            self._model.setParam("DualReductions", 0)
            self._model.optimize()
        solve_time = time.perf_counter() - t0

        status_map = {
            gp.GRB.OPTIMAL: SolverStatus.OPTIMAL,
            gp.GRB.INFEASIBLE: SolverStatus.INFEASIBLE,
            gp.GRB.UNBOUNDED: SolverStatus.UNBOUNDED,
            gp.GRB.TIME_LIMIT: SolverStatus.TIME_LIMIT,
        }
        solver_status = status_map.get(self._model.status, SolverStatus.ERROR)

        obj_val = None
        var_vals = {}
        if solver_status == SolverStatus.OPTIMAL:
            obj_val = self._model.objVal
            var_vals = {name: v.X for name, v in self._vars.items()}

        return SolverResult(
            status=solver_status,
            objective_value=obj_val,
            variable_values=var_vals,
            solve_time_seconds=solve_time,
        )

    def solver_name(self) -> str:
        return "Gurobi"


def _merge_symmetric_quadratic(
    quadratic: dict[tuple[str, str], float],
) -> dict[tuple[str, str], float]:
    """Combine (a, b) and (b, a) entries into one canonical key.

    Off-diagonal terms in a Q matrix are symmetric: the coefficient of x·y is
    the sum of (x, y) and (y, x). Callers may pass either or both; this
    normalizes to a single entry with the combined coefficient.
    """
    merged: dict[tuple[str, str], float] = {}
    for (a, b), coeff in quadratic.items():
        key = (a, b) if a <= b else (b, a)
        merged[key] = merged.get(key, 0.0) + coeff
    return merged
