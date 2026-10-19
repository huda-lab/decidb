"""Explicit NULL imputation is legal inside DECIDE reducer coefficients.

NULL values still fail by default. These tests pin the explicit opt-in path:
data-only COALESCE/IFNULL nodes are evaluated by DuckDB as coefficient atoms,
while the same operators over a DECIDE variable remain unsupported.
"""

import pytest

from solver.types import ObjSense, SolverStatus, VarType


_CONSTRAINT_ROWS = [
    (1, None, 8.0),
    (2, 3.0, 7.0),
    (3, 5.0, 6.0),
]


@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_coalesce_inside_constraint_reducer_matches_oracle(decidb_cli, oracle_solver):
    """A NULL weight explicitly imputed to zero participates with coefficient 0."""
    rows, cols = decidb_cli.execute("""
        SELECT id, weight, profit, keep
        FROM (VALUES
            (1, NULL::DOUBLE, 8.0),
            (2, 3.0,          7.0),
            (3, 5.0,          6.0)
        ) items(id, weight, profit)
        DECIDE keep(BOOL)
        SUCH THAT SUM(keep * COALESCE(weight, 0)) <= 3
        MAXIMIZE SUM(keep * profit)
    """)

    oracle_solver.create_model("coalesce_constraint_reducer")
    for row_id, _, _ in _CONSTRAINT_ROWS:
        oracle_solver.add_variable(f"keep_{row_id}", VarType.BINARY)
    oracle_solver.add_constraint(
        {
            f"keep_{row_id}": 0.0 if weight is None else weight
            for row_id, weight, _ in _CONSTRAINT_ROWS
        },
        "<=",
        3.0,
        name="weight_cap",
    )
    oracle_solver.set_objective(
        {f"keep_{row_id}": profit for row_id, _, profit in _CONSTRAINT_ROWS},
        ObjSense.MAXIMIZE,
    )
    oracle = oracle_solver.solve()
    assert oracle.status == SolverStatus.OPTIMAL

    ci = {name: idx for idx, name in enumerate(cols)}
    actual = {int(row[ci["id"]]): int(row[ci["keep"]]) for row in rows}
    expected = {
        row_id: round(oracle.variable_values[f"keep_{row_id}"])
        for row_id, _, _ in _CONSTRAINT_ROWS
    }
    assert actual == expected == {1: 1, 2: 1, 3: 0}


@pytest.mark.parametrize("imputer", ["COALESCE", "IFNULL"])
@pytest.mark.var_boolean
@pytest.mark.cons_aggregate
@pytest.mark.obj_maximize
@pytest.mark.correctness
def test_imputation_inside_objective_reducer_matches_oracle(decidb_cli, oracle_solver, imputer):
    """Both spellings bind to a data-only operator coefficient inside SUM."""
    source_rows = [(1, None), (2, 4.0), (3, 9.0)]
    rows, cols = decidb_cli.execute(f"""
        SELECT id, score, keep
        FROM (VALUES
            (1, NULL::DOUBLE),
            (2, 4.0),
            (3, 9.0)
        ) items(id, score)
        DECIDE keep(BOOL)
        SUCH THAT SUM(keep) <= 1
        MAXIMIZE SUM(keep * {imputer}(score, 0))
    """)

    oracle_solver.create_model(f"{imputer.lower()}_objective_reducer")
    for row_id, _ in source_rows:
        oracle_solver.add_variable(f"keep_{row_id}", VarType.BINARY)
    oracle_solver.add_constraint(
        {f"keep_{row_id}": 1.0 for row_id, _ in source_rows},
        "<=",
        1.0,
        name="cardinality",
    )
    oracle_solver.set_objective(
        {
            f"keep_{row_id}": 0.0 if score is None else score
            for row_id, score in source_rows
        },
        ObjSense.MAXIMIZE,
    )
    oracle = oracle_solver.solve()
    assert oracle.status == SolverStatus.OPTIMAL

    ci = {name: idx for idx, name in enumerate(cols)}
    actual = {int(row[ci["id"]]): int(row[ci["keep"]]) for row in rows}
    expected = {
        row_id: round(oracle.variable_values[f"keep_{row_id}"])
        for row_id, _ in source_rows
    }
    assert actual == expected == {1: 0, 2: 0, 3: 1}


@pytest.mark.parametrize("clauses", [
    "SUCH THAT SUM(COALESCE(keep, 0)) <= 1 MAXIMIZE SUM(keep)",
    "SUCH THAT SUM(keep) <= 1 MAXIMIZE SUM(COALESCE(keep, 0))",
])
@pytest.mark.error
def test_coalesce_over_decide_variable_inside_reducer_is_rejected(decidb_cli, clauses):
    """Imputation may wrap data, never a value controlled by the solver."""
    decidb_cli.assert_error(f"""
        SELECT id, keep
        FROM range(1, 4) items(id)
        DECIDE keep(BOOL)
        {clauses}
    """, match=r"(?i)unsupported operator expression.*DECIDE variable")
