"""Per-rule fixtures for the direct-solve conformance suite (`test_direct_rule_conformance.py`).

What a direct-solve rule owes the user is the same for every rule: the DECIDE
schema and rows, the solver's read of every input row before any row is released,
prepared plans, plan serialization, EXPLAIN, `require` declines, and the solver path
under `off`, a forced backend, and `DIAGNOSE`. A rule supplies the queries below and the
suite checks all of it. Adding a rule means adding one `RuleFixture` to `RULE_FIXTURES`.
"""

from dataclasses import dataclass

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
    #: SQL that creates a temporary table, then a query the rule applies to over it. The suite
    #: prepares the query, changes the table, and checks the plan is rebuilt.
    table_setup: str
    table_applies: str
    table_change: str
    #: (declaration, constraints, objective) over the source in `near_miss_source`, each one condition away from
    #: applying. The rule must decline it under `require`.
    near_misses: tuple
    #: More triples over the same source that the rule applies to, though they look like near misses.
    near_miss_applies: tuple
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
    # Per-row bounds on x with no WHEN that S1 leaves on the solver, because the solver reads them as bounds on the
    # variable itself (a bound that always holds, or a strict bound against a fraction that shifts into {0, 1}, S1
    # admits). `x>1`, `x>1.5` and `x>2` cannot hold, so the query is infeasible before the solver reads a row; S1 does
    # not reproduce that early error. Then a strict lower count bound at exactly 2^53, which would mean 2^53 + 1, a
    # value a double cannot hold.
    near_misses=(
        ("x(BOOL)", "SUM(x)<=2 AND x>1", "MAXIMIZE SUM(p*x)"),
        ("x(BOOL)", "SUM(x)<=2 AND x>1.5", "MAXIMIZE SUM(p*x)"),
        ("x(BOOL)", "SUM(x)<=2 AND x>2", "MAXIMIZE SUM(p*x)"),
        ("x(BOOL)", "SUM(x)>9007199254740992", "MAXIMIZE SUM(p*x)"),
    ),
    # A query that looks like a near miss and applies: a right-hand aggregate is a window column of the source (H-19), so
    # S1 reads it as a source-valued bound. The conformance test asserts that `require` applies and that `auto`, `off`
    # and `require` answer alike; `S1_AGGREGATE_BOUNDS_NOW_APPLIED` gives the oracle the value of the bounds it checks.
    near_miss_applies=(("x(BOOL)", "SUM(x)<=COUNT(*)/0", "MAXIMIZE SUM(p*x)"),),
    near_miss_source="FROM (VALUES (1, 9.0::DOUBLE, 1), (2, 10.0::DOUBLE, 1)) t(id,p,cap)",
    near_miss_column="id",
    late_errors=(),
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
    near_miss_applies=(),
    near_misses=(),
    late_errors=(
        # Errors A1 raises, on the last of 5,000 rows: an unbounded problem and an integer bound beyond the range.
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
    ),
    late_column="i",
)


def _r1_late_error(decision_sql, w_sql="1.0", cap_sql="10.0"):
    """A 5,000-row problem whose last row is invalid. Rows 0 to 4998 are ordinary."""
    return f"""
        FROM (SELECT i, {w_sql} AS w, 1.0 AS v, {cap_sql} AS cap FROM range(5000) t(i)) s
        DECIDE {decision_sql}
        """


R1 = RuleFixture(
    rule="R1_CONTINUOUS_KNAPSACK",
    condition="min/max SUM(v*x), l <= x <= u, L <= SUM(w*x) <= U",
    # The worked example of the rule's definition: the budget is used up, row 2 is partly filled (x = 4, 2, 0). Every
    # value is a whole number, so the rows equal the solver's exactly and the query also runs on forced HiGHS.
    applies="""
        SELECT id, w, v, cap, x FROM (
            FROM (VALUES (1, 2.0::DOUBLE, 10.0::DOUBLE, 4.0::DOUBLE), (2, 3.0::DOUBLE, 9.0::DOUBLE, 3.0::DOUBLE),
                         (3, 2.0::DOUBLE, 4.0::DOUBLE, 5.0::DOUBLE)) t(id, w, v, cap)
            DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap AND SUM(w * x) <= 14
            MAXIMIZE SUM(v * x)
        ) q ORDER BY id
    """,
    empty_applies="""
        SELECT id, w, v, cap, x FROM (
            FROM (SELECT * FROM (VALUES (1, 2.0::DOUBLE, 10.0::DOUBLE, 4.0::DOUBLE),
                                        (2, 3.0::DOUBLE, 9.0::DOUBLE, 3.0::DOUBLE)) t(id, w, v, cap) WHERE false) t
            DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND cap AND SUM(w * x) <= 14
            MAXIMIZE SUM(v * x)
        ) q ORDER BY id
    """,
    table_setup="""
        CREATE TEMP TABLE direct_r1_life(id INTEGER, w DOUBLE, v DOUBLE, cap DOUBLE);
        INSERT INTO direct_r1_life VALUES (1,2,10,4),(2,3,9,3);
    """,
    table_applies="""
        SELECT id,x FROM (
            FROM direct_r1_life DECIDE x(REAL)
            SUCH THAT x BETWEEN 0 AND cap AND SUM(w*x)<=10 MAXIMIZE SUM(v*x)
        ) q
    """,
    table_change="ALTER TABLE direct_r1_life ADD COLUMN extra INTEGER;",
    # Each is the plain form with one condition changed, so the rule has to decline it: an INT decision (a unit cannot
    # be partly filled), two budgets with different weights (no single price per unit of budget), an objective with a
    # square (a term that is not a per-row linear value; the closest the IR offers to a coupled term for one
    # decision), and a constraint that reads two decisions of a row (it links them).
    near_misses=(
        ("x(INT)", "x <= cap AND SUM(w*x) <= 5", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "x <= cap AND SUM(w*x) <= 5 AND SUM(p*x) <= 7", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "x <= cap AND SUM(w*x) <= 5", "MAXIMIZE SUM(p*x) - SUM(POWER(x, 2))"),
        # Two decisions in one budget apply; a per-row constraint that reads both links them.
        ("x(REAL), y(REAL)", "x <= cap AND y <= cap AND x + y <= 4 AND SUM(w*x + w*y) <= 5", "MAXIMIZE SUM(p*x + p*y)"),
    ),
    # Forms that look like near misses and apply: a minimum, an exact fill, and one sum compared twice (a range), two
    # decisions in one budget (the pieces of a benefit), and a budget read from the data (a column of the source, and
    # an aggregate of it, which the right-hand-side rewrite makes a column).
    near_miss_applies=(
        ("x(REAL)", "x <= cap AND SUM(w*x) <= lim", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "x <= cap AND SUM(w*x) <= 0.5 * SUM(cap*w)", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "x <= cap AND SUM(w*x) >= 3", "MINIMIZE SUM(p*x)"),
        ("x(REAL)", "x <= cap AND SUM(w*x) = 5", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "x <= cap AND SUM(w*x) >= 3 AND SUM(w*x) <= 7", "MINIMIZE SUM(p*x)"),
        ("x(REAL), y(REAL)", "x <= cap AND y <= cap AND SUM(w*x + w*y) <= 5", "MAXIMIZE SUM(p*x + p*y)"),
    ),
    near_miss_source="FROM (VALUES (1, 9.0::DOUBLE, 2.0::DOUBLE, 5.0::DOUBLE, 6.0::DOUBLE), "
    "(2, 10.0::DOUBLE, 3.0::DOUBLE, 3.0::DOUBLE, 6.0::DOUBLE)) t(id,p,w,cap,lim)",
    near_miss_column="id",
    late_errors=(
        # Errors R1 raises, on the last of 5,000 rows: an empty box, an unbounded problem and a NaN weight.
        (
            _r1_late_error(
                "x(REAL) SUCH THAT x >= 0 AND x <= cap AND SUM(w*x) <= 3000 MAXIMIZE SUM(v*x)",
                cap_sql="CASE WHEN i=4999 THEN -1.0 ELSE 10.0 END",
            ),
            "infeasible",
        ),
        (
            _r1_late_error(
                "x(REAL) SUCH THAT x >= 0 AND x <= cap AND SUM(w*x) <= 3000 MAXIMIZE SUM(v*x)",
                w_sql="CASE WHEN i=4999 THEN -1.0 ELSE 1.0 END",
                cap_sql="CASE WHEN i=4999 THEN 1e30 ELSE 10.0 END",
            ),
            "unbounded",
        ),
        (
            _r1_late_error(
                "x(REAL) SUCH THAT x >= 0 AND x <= cap AND SUM(w*x) <= 3000 MAXIMIZE SUM(v*x)",
                w_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "w is NaN or infinite on some rows",
        ),
    ),
    late_column="i",
)


RULE_FIXTURES = (S1, A1, R1)


# Bounds S1 applies that the oracle checks, as (constraint, the bound's value over the source
# `t(id, p, cap)` of rows (1, 9.0, 1) and (2, 10.0, 1)). A count divided by zero is infinity in DuckDB: no limit.
S1_AGGREGATE_BOUNDS_NOW_APPLIED = (
    # A source column or an expression over it that can raise: read on every row, as the solver reads it.
    ("SUM(x)<=CAST(cap AS TINYINT)", 1.0),
    ("SUM(x)<=cap+1", 2.0),
    ("SUM(x)<=1.5+cap", 2.5),
    ("SUM(x)<=COUNT(*)/0", float("inf")),
    ("SUM(x)<=COUNT(*)/COUNT(*)", 1.0),
)


def near_miss_cases():
    """One (fixture, case) pair per near miss, so each is its own test item."""
    return [(fixture, case) for fixture in RULE_FIXTURES for case in fixture.near_misses]


def late_error_cases():
    """One (fixture, case) pair per late-error problem."""
    return [(fixture, case) for fixture in RULE_FIXTURES for case in fixture.late_errors]
