"""Per-rule fixtures for the direct-solve contract suite (`test_direct_rule_contract.py`).

The contract a direct-solve rule owes the user is the same for every rule: the DECIDE
schema and rows, the solver's read of every input row before any row is released,
prepared plans, plan serialization, EXPLAIN, `require` reasons, and the solver path
under `off`, a forced backend, and `DIAGNOSE`. A rule supplies the queries below and the
suite checks all of it. Adding a rule means adding one `RuleFixture` to `RULE_FIXTURES`.
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class RuleFixture:
    #: The rule name EXPLAIN and `require` messages print.
    rule: str
    #: A query the rule proves, ordered so its rows are comparable, with one optimal assignment.
    hit: str
    #: SQL that creates a temporary table, then a query the rule proves over it. The suite
    #: prepares the query, changes the table, and checks the plan is rebuilt.
    table_setup: str
    table_hit: str
    table_change: str
    #: (declaration, constraints, objective, reason substring) over the source in `near_miss_source`, each one
    #: condition away from a hit. The rule must decline it with that reason.
    near_misses: tuple
    near_miss_source: str
    #: A column of `near_miss_source` the near-miss queries select.
    near_miss_column: str
    #: (DECIDE subquery text, error substring): a problem whose last of 5,000 rows is invalid. The error must
    #: raise however little of the result a parent reads; `LIMIT 0` may skip it.
    late_errors: tuple
    #: The source column the late-error subqueries expose.
    late_column: str


S1 = RuleFixture(
    rule="S1_CARDINALITY_INTERVAL",
    hit="""
        SELECT id, score, x FROM (
            FROM (VALUES (0, CAST('2.0' AS DOUBLE)), (1, CAST('9.0' AS DOUBLE)), (2, CAST('-1.0' AS DOUBLE)))
                t(id, score)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1
            MAXIMIZE SUM(score * x)
        ) q ORDER BY id
    """,
    table_setup="""
        CREATE TEMP TABLE direct_life(id INTEGER, score DOUBLE);
        INSERT INTO direct_life VALUES (1,2),(2,9);
    """,
    table_hit="""
        SELECT id,x FROM (
            FROM direct_life DECIDE x(BOOL)
            SUCH THAT SUM(x)<=1 MAXIMIZE SUM(score*x)
        ) q
    """,
    table_change="ALTER TABLE direct_life ADD COLUMN extra INTEGER;",
    near_miss_source="FROM (VALUES (1, 9.0::DOUBLE, 1), (2, 10.0::DOUBLE, 1)) t(id,p,cap)",
    near_miss_column="id",
    near_misses=(
        ("x(BOOL), y(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x)", "variable_shape"),
        ("x(BOOL)", "SUM(x)<=1 AND x<=1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1 AND x<=cap", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1 PER id AND SUM(x)>=1 PER cap", "MAXIMIZE SUM(p*x)", "constraint_scope"),
        ("x(BOOL)", "SUM(x)<>1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x+1)<=3", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(id*x)<=2", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=cap+1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1.5+cap", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("t.x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x)", "variable_shape"),
        ("t.x(BOOL)", "SUM(t: x)<=1", "MAXIMIZE SUM(p*x)", "variable_shape"),
        ("x(BOOL)", "SUM(x)<=random()", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM((p+random())*x)", "coefficient_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x)+SUM(CAST(cap::VARCHAR AS DOUBLE)*x)", "coefficient_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(MAX(p*x)) PER id", "objective_scope"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x) WHEN id=1", "objective_scope"),
        ("x(BOOL)", "SUM(x)<=1", "", "problem_shape"),
        ("x(BOOL)", "SUM(x)<=9007199254740993", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "norm(x,'inf')<=1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "norm(x,1)<=1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x) - norm(p*x,1)", "objective_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MINIMIZE norm(p*x,2)", "objective_shape"),
    ),
    late_errors=tuple(
        (
            f"""
            FROM (SELECT i, {score_sql} AS score FROM range(5000) t(i)) s
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 0 MAXIMIZE SUM(score*x)
            """,
            message,
        )
        for score_sql, message in (
            ("CASE WHEN i=4999 THEN NULL ELSE 1.0 END", 'column "score" is NULL'),
            ("CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END", "invalid value (NaN or Infinity)"),
            ("CASE WHEN i=4999 THEN 'Infinity'::DOUBLE ELSE 1.0 END", "invalid value (NaN or Infinity)"),
        )
    ),
    late_column="i",
)


RULE_FIXTURES = (S1,)


def near_miss_cases():
    """One (fixture, case) pair per near miss, so each is its own test item."""
    return [(fixture, case) for fixture in RULE_FIXTURES for case in fixture.near_misses]


def late_error_cases():
    """One (fixture, case) pair per late-error problem."""
    return [(fixture, case) for fixture in RULE_FIXTURES for case in fixture.late_errors]
