"""Per-rule fixtures for the direct-solve conformance suite (`test_direct_rule_conformance.py`).

What a direct-solve rule owes the user is the same for every rule: the DECIDE
schema and rows, the solver's read of every input row before any row is released,
prepared plans, plan serialization, EXPLAIN, `require` reasons, and the solver path
under `off`, a forced backend, and `DIAGNOSE`. A rule supplies the queries below and the
suite checks all of it. Adding a rule means adding one `RuleFixture` to `RULE_FIXTURES`.
"""

from dataclasses import dataclass

# A near miss that now applies: a right-hand aggregate is a window column of the source (H-19) and a bound that can
# raise is read on every row, so S1 reads it as a source-valued bound. The conformance test asserts that `require`
# applies and that `auto`, `off` and `require` answer alike; `S1_AGGREGATE_BOUNDS_NOW_APPLIED` gives the oracle the
# bound's value.
APPLIES_UNDER_REQUIRE = "<applies under require>"

# Per-row bounds on x with no WHEN that S1 leaves on the solver, as (comparison, constant). The solver reads them as
# Boolean arithmetic, and each one here cannot hold, so the query is infeasible before the solver reads a row. S1 does
# not reproduce that early error. A bound that always holds (`x >= -3`, `x > -3`) changes nothing and S1 admits it. A
# strict bound against a fraction is admitted when it shifts to a whole bound inside {0, 1} (`x > 0.5`, `x < 1.5`,
# `x < 0.5`, `x > -0.5`): the solver shifts it with the same function. The same bounds with a WHEN are plain arithmetic
# and are admitted.
S1_SOLVER_ONLY_ROW_BOUNDS = {
    ("<=", "-3"),
    ("<=", "-1"),
    ("<=", "-0.5"),
    ("<", "-3"),
    ("<", "-1"),
    ("<", "-0.5"),
    ("<", "0"),
    (">=", "1.5"),
    (">=", "2"),
    (">", "1"),
    (">", "1.5"),
    (">", "2"),
    ("=", "-3"),
    ("=", "-1"),
    ("=", "-0.5"),
    ("=", "1.5"),
    ("=", "2"),
}


@dataclass(frozen=True)
class RuleFixture:
    #: The rule name EXPLAIN and `require` messages print.
    rule: str
    #: The one fixed line EXPLAIN prints as the condition: the problem class, with no number from the query.
    condition: str
    #: A query the rule applies to, ordered so its rows are comparable, with one optimal assignment.
    applies: str
    #: The `applies` query over a source with no rows. The solver answers it, so the rule must give its outcome.
    empty_applies: str
    #: Queries whose constant bound contradicts the decision's built-in domain (`x <= -1` on a decision that cannot
    #: be negative, `BOOL x >= 2`), over an empty source and over a source with rows. The solver rejects every one
    #: before it reads a row. A rule either declines them or gives the solver's outcome.
    impossible_bound_cases: tuple
    #: SQL that creates a temporary table, then a query the rule applies to over it. The suite
    #: prepares the query, changes the table, and checks the plan is rebuilt.
    table_setup: str
    table_applies: str
    table_change: str
    #: (declaration, constraints, objective, reason substring) over the source in `near_miss_source`, each one
    #: condition away from applying. The rule must decline it with that reason.
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
    rule="S1_UNIFORM_MATROID",
    condition="min/max SUM(s*x), x in {0,1}, lo <= SUM(x) <= hi",
    applies="""
        SELECT id, score, x FROM (
            FROM (VALUES (0, CAST('2.0' AS DOUBLE)), (1, CAST('9.0' AS DOUBLE)), (2, CAST('-1.0' AS DOUBLE)))
                t(id, score)
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1
            MAXIMIZE SUM(score * x)
        ) q ORDER BY id
    """,
    empty_applies="""
        SELECT id, score, x FROM (
            FROM (SELECT * FROM (VALUES (0, CAST('2.0' AS DOUBLE)), (1, CAST('9.0' AS DOUBLE))) t(id, score)
                  WHERE false) t
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1
            MAXIMIZE SUM(score * x)
        ) q ORDER BY id
    """,
    impossible_bound_cases=tuple(
        f"""
        SELECT id, score, x FROM (
            FROM (SELECT * FROM (VALUES (0, CAST('2.0' AS DOUBLE)), (1, CAST('9.0' AS DOUBLE))) t(id, score)
                  WHERE {where}) t
            DECIDE x(BOOL) SUCH THAT SUM(x) <= 1 AND {bound}
            MAXIMIZE SUM(score * x)
        ) q ORDER BY id
        """
        for where in ("false", "true")
        for bound in ("x <= -1", "x >= 2")
    ),
    table_setup="""
        CREATE TEMP TABLE direct_life(id INTEGER, score DOUBLE);
        INSERT INTO direct_life VALUES (1,2),(2,9);
    """,
    table_applies="""
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
        ("x(BOOL)", "SUM(x)<=1 AND x<=cap+1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1 PER id AND SUM(x)>=1 PER cap", "MAXIMIZE SUM(p*x)", "constraint_scope"),
        ("x(BOOL)", "SUM(x)<>1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        # A count body with an offset or a negation is admitted (`SUM(x+1)`, `SUM(1-x)`); one with a column in it, or a
        # weight on x other than 1 or -1, is not. A bound that can raise is admitted: the plan evaluates it on every row,
        # as the solver does, so both raise on the same inputs (the three marked APPLIES_UNDER_REQUIRE).
        ("x(BOOL)", "SUM(x+cap)<=3", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(2*x+1)<=3", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=CAST(cap AS TINYINT)", "MAXIMIZE SUM(p*x)", APPLIES_UNDER_REQUIRE),
        ("x(BOOL)", "SUM(id*x)<=2", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=cap+1", "MAXIMIZE SUM(p*x)", APPLIES_UNDER_REQUIRE),
        # The WHEN scopes only the count here, while the left side still sums every row.
        ("x(BOOL)", "SUM(x)<=COUNT(*) WHEN (id>0)", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1.5+cap", "MAXIMIZE SUM(p*x)", APPLIES_UNDER_REQUIRE),
        ("t.x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x)", "variable_shape"),
        ("t.x(BOOL)", "SUM(t: x)<=1", "MAXIMIZE SUM(p*x)", "variable_shape"),
        ("x(BOOL)", "SUM(x)<=random()", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM((p+random())*x)", "coefficient_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x)+SUM(CAST(cap::VARCHAR AS DOUBLE)*x)", "coefficient_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(MAX(p*x)) PER id", "objective_scope"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x) WHEN id=1", "objective_scope"),
        ("x(BOOL)", "SUM(x)<=9007199254740993", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "norm(x,'inf')<=1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "norm(x,1)<=1", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x) - norm(p*x,1)", "objective_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MINIMIZE norm(p*x,2)", "objective_shape"),
        ("x(BOOL)", "SUM(x)<='Infinity'::DOUBLE", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        # A per-row bound that can raise at runtime.
        ("x(BOOL)", "SUM(x)<=2 AND x<=CAST(cap AS TINYINT)", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        # A constant factor that is not usable: the solver reads a NULL factor as an objective of zeros, and a subquery
        # factor has no known sign when the plan is built.
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE SUM(p*x)/0", "objective_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE CAST(NULL AS DOUBLE)*SUM(p*x)", "objective_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE (SELECT 2)*SUM(p*x)", "objective_shape"),
        ("x(BOOL)", "SUM(x)<=1", "MAXIMIZE (SELECT -2)*SUM(p*x)", "objective_shape"),
        # A bound that reads the group through aggregates. H-19 turned the ones below that apply into window columns; the one
        # with its own WHEN stays on the solver's fold.
        (
            "x(BOOL)",
            "SUM(x)<=(COUNT(*) WHEN (id>0))/2",
            "MAXIMIZE SUM(p*x)",
            "a WHEN on the right-hand aggregate",
        ),
        (
            "x(BOOL)",
            "SUM(x)<=MIN(p+1)/2",
            "MAXIMIZE SUM(p*x)",
            APPLIES_UNDER_REQUIRE,
        ),
        ("x(BOOL)", "SUM(x)<=AVG(p)", "MAXIMIZE SUM(p*x)", APPLIES_UNDER_REQUIRE),
        ("x(BOOL)", "SUM(x)<=COUNT(*)/0", "MAXIMIZE SUM(p*x)", APPLIES_UNDER_REQUIRE),
        ("x(BOOL)", "SUM(x)<=COUNT(*)/COUNT(*)", "MAXIMIZE SUM(p*x)", APPLIES_UNDER_REQUIRE),
        ("x(BOOL)", "SUM(x)<=abs(COUNT(*)-5)", "MAXIMIZE SUM(p*x)", APPLIES_UNDER_REQUIRE),
        # 1e23 is not a double: the window column keeps its exact value.
        (
            "x(BOOL)",
            "SUM(x)<=COUNT(*)*100000000000000000000000",
            "MAXIMIZE SUM(p*x)",
            APPLIES_UNDER_REQUIRE,
        ),
    )
    + tuple(
        ("x(BOOL)", f"SUM(x)<=2 AND x{op}{constant}", "MAXIMIZE SUM(p*x)", "constraint_shape")
        for op, constant in sorted(S1_SOLVER_ONLY_ROW_BOUNDS, key=lambda bound: (bound[0], float(bound[1])))
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
            ("CASE WHEN i=4999 THEN NULL ELSE 1.0 END", "is NULL"),
            ("CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END", "invalid value (NaN or Infinity)"),
            ("CASE WHEN i=4999 THEN 'Infinity'::DOUBLE ELSE 1.0 END", "invalid value (NaN or Infinity)"),
        )
    ),
    late_column="i",
)


def _a1_late_error(decision_sql, score_sql="1.0", cap_sql="10.0"):
    """A 5,000-row problem whose last row is invalid. Rows 0 to 4998 are ordinary."""
    return f"""
        FROM (SELECT i, {score_sql} AS score, {cap_sql} AS cap FROM range(5000) t(i)) s
        DECIDE {decision_sql}
        """


A1 = RuleFixture(
    rule="A1_SEPARABLE_BOX",
    condition="min/max SUM(c*x), l <= x <= u, each row independent",
    # Whole-number answers with a unique optimum, so the rows equal the solver's exactly and the query also runs on
    # forced HiGHS (which has no mixed-integer quadratic and no non-convex quadratic).
    applies="""
        SELECT id, score, cap, x FROM (
            FROM (VALUES (0, CAST('2.0' AS DOUBLE), 3), (1, CAST('-1.0' AS DOUBLE), 5), (2, CAST('4.0' AS DOUBLE), 2))
                t(id, score, cap)
            DECIDE x(INT) SUCH THAT x BETWEEN 0 AND cap
            MAXIMIZE SUM(score * x)
        ) q ORDER BY id
    """,
    empty_applies="""
        SELECT id, score, cap, x FROM (
            FROM (SELECT * FROM (VALUES (0, CAST('2.0' AS DOUBLE), 3), (1, CAST('-1.0' AS DOUBLE), 5)) t(id, score, cap)
                  WHERE false) t
            DECIDE x(INT) SUCH THAT x BETWEEN 0 AND cap
            MAXIMIZE SUM(score * x)
        ) q ORDER BY id
    """,
    impossible_bound_cases=tuple(
        f"""
        SELECT id, score, cap, x FROM (
            FROM (SELECT * FROM (VALUES (0, CAST('2.0' AS DOUBLE), 3), (1, CAST('-1.0' AS DOUBLE), 5)) t(id, score, cap)
                  WHERE {where}) t
            DECIDE {declaration} SUCH THAT {bound}
            MAXIMIZE SUM(score * x)
        ) q ORDER BY id
        """
        for where in ("false", "true")
        for declaration, bound in (
            ("x(INT)", "x <= -1"),
            ("x(REAL)", "x <= -0.5"),
            ("x(BOOL)", "x >= 2"),
        )
    ),
    table_setup="""
        CREATE TEMP TABLE direct_a1_life(id INTEGER, score DOUBLE, cap INTEGER);
        INSERT INTO direct_a1_life VALUES (1,2,3),(2,-1,5);
    """,
    table_applies="""
        SELECT id,x FROM (
            FROM direct_a1_life DECIDE x(INT)
            SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(score*x)
        ) q
    """,
    table_change="ALTER TABLE direct_a1_life ADD COLUMN extra INTEGER;",
    near_miss_source="FROM (VALUES (1, 9.0::DOUBLE, 5), (2, 10.0::DOUBLE, 3)) t(id,p,cap)",
    near_miss_column="id",
    near_misses=(
        # Decisions that are not row-scoped, or rows that are coupled.
        ("t.x(REAL)", "x<=cap", "MAXIMIZE SUM(p*x)", "variable_shape"),
        ("x(REAL), y(REAL)", "x+y<=3 AND x<=cap", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(REAL), y(REAL)", "x<=cap AND y<=cap", "MAXIMIZE SUM(x*y)", "objective_shape"),
        ("x(REAL)", "SUM(x)<=5", "MAXIMIZE SUM(p*x)", "found an aggregate constraint"),
        ("x(REAL)", "norm(x,1)<=5", "MAXIMIZE SUM(p*x)", "found an aggregate constraint"),
        # Bounds that are not intervals, or not bounds.
        ("x(INT)", "x<=cap AND x<>1", "MAXIMIZE SUM(p*x)", "found <> or IN"),
        ("x(INT)", "x IN (1,2,3)", "MAXIMIZE SUM(p*x)", "found <> or IN"),
        ("x(REAL)", "POWER(x,2)<=9", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        ("x(REAL)", "ABS(x)<=3", "MAXIMIZE SUM(p*x)", "constraint_shape"),
        # Values that are not per-row source values.
        ("x(REAL)", "x<=random()", "MAXIMIZE SUM(p*x)", "coefficient_shape"),
        ("x(REAL)", "x<=cap", "MAXIMIZE SUM(random()*x)", "coefficient_shape"),
        ("x(REAL)", "x<=cap WHEN (random()<2)", "MAXIMIZE SUM(p*x)", "constraint_scope"),
        ("x(REAL)", "x<=cap", "MAXIMIZE SUM(p*x) WHEN (random()<2)", "objective_scope"),
        # Objectives outside the class. Ones the solver itself rejects are in test_direct_a1_separable_box.py.
        ("x(REAL)", "x<=cap", "MAXIMIZE SUM(p*x) PER id", "objective_scope"),
        ("x(REAL)", "x<=cap", "MAXIMIZE AVG(p*x)", "objective_shape"),
        ("x(REAL)", "x<=cap", "MAXIMIZE MIN(p*x)", "objective_shape"),
        ("x(REAL)", "x<=cap", "MINIMIZE norm(x,1)", "objective_shape"),
    ),
    late_errors=(
        # Every kind of error A1 raises, on the last of 5,000 rows.
        (
            _a1_late_error(
                "x(REAL) SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(score*x)",
                score_sql="CASE WHEN i=4999 THEN NULL ELSE 1.0 END",
            ),
            "is NULL",
        ),
        (
            _a1_late_error(
                "x(REAL) SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(score*x)",
                score_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "invalid value (NaN or Infinity)",
        ),
        (
            _a1_late_error(
                "x(REAL) SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(score*x)",
                score_sql="CASE WHEN i=4999 THEN 'Infinity'::DOUBLE ELSE 1.0 END",
            ),
            "invalid value (NaN or Infinity)",
        ),
        (
            _a1_late_error(
                "x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x-score,2))",
                score_sql="CASE WHEN i=4999 THEN NULL ELSE 1.0 END",
            ),
            "is NULL",
        ),
        (
            _a1_late_error(
                "x(REAL) SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(score*x)",
                cap_sql="CASE WHEN i=4999 THEN NULL ELSE 10.0 END",
            ),
            "is NULL",
        ),
        (
            _a1_late_error(
                "x(REAL) SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(score*x)",
                cap_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 10.0 END",
            ),
            "invalid value (NaN)",
        ),
        (
            _a1_late_error(
                "x(REAL) SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(score*x)",
                cap_sql="CASE WHEN i=4999 THEN -1.0 ELSE 10.0 END",
            ),
            "infeasible",
        ),
        (
            _a1_late_error(
                "x(REAL) SUCH THAT x <= cap MAXIMIZE SUM(score*x)", cap_sql="CASE WHEN i=4999 THEN 1e30 ELSE 10.0 END"
            ),
            "unbounded",
        ),
        (
            _a1_late_error(
                "x(INT) SUCH THAT x <= cap MAXIMIZE SUM(score*x)", cap_sql="CASE WHEN i=4999 THEN 1e17 ELSE 10.0 END"
            ),
            "beyond the range",
        ),
        # Each coefficient is finite, but the square's weight (1e200 squared) overflows on the last row.
        (
            _a1_late_error(
                "x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(score*x-1,2))",
                score_sql="CASE WHEN i=4999 THEN 1e200 ELSE 1.0 END",
            ),
            "invalid value (NaN or Infinity)",
        ),
    ),
    late_column="i",
)


RULE_FIXTURES = (S1, A1)


# The near misses marked APPLIES_UNDER_REQUIRE, as (constraint, the bound's value over the source `t(id, p, cap)` of
# rows (1, 9.0, 1) and (2, 10.0, 1)). A count divided by zero is infinity in DuckDB: no limit.
S1_AGGREGATE_BOUNDS_NOW_APPLIED = (
    # A source column or an expression over it that can raise: read on every row, as the solver reads it.
    ("SUM(x)<=CAST(cap AS TINYINT)", 1.0),
    ("SUM(x)<=cap+1", 2.0),
    ("SUM(x)<=1.5+cap", 2.5),
    ("SUM(x)<=MIN(p+1)/2", 5.0),
    ("SUM(x)<=AVG(p)", 9.5),
    ("SUM(x)<=COUNT(*)/0", float("inf")),
    ("SUM(x)<=COUNT(*)/COUNT(*)", 1.0),
    ("SUM(x)<=abs(COUNT(*)-5)", 3.0),
    ("SUM(x)<=COUNT(*)*100000000000000000000000", 2e23),
)


def near_miss_cases():
    """One (fixture, case) pair per near miss, so each is its own test item."""
    return [(fixture, case) for fixture in RULE_FIXTURES for case in fixture.near_misses]


def late_error_cases():
    """One (fixture, case) pair per late-error problem."""
    return [(fixture, case) for fixture in RULE_FIXTURES for case in fixture.late_errors]
