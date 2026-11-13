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
    #: The CLI fixture that forces a backend able to solve `applies`.
    forced_backend_cli: str = "decidb_cli_highs"


_S1_NAN_LAST = "CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END"


def _s1_late_error(decision_sql, score_sql="1.0", g_sql="i % 50", cap_sql="3.0"):
    """A 5,000-row problem whose last row is invalid, in fifty groups of a hundred rows. Rows 0 to 4998 are ordinary."""
    return f"""
        FROM (SELECT i, {g_sql} AS g, {score_sql} AS score, {cap_sql} AS cap FROM range(5000) t(i)) s
        DECIDE {decision_sql}
        """


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
        # An objective `when` that is not a source predicate: on the objective and on its sum.
        ("x(BOOL)", "SUM(x)<=2", "MAXIMIZE when txid_current() >= 0: SUM(p*x)"),
        ("x(BOOL)", "SUM(x)<=2", "MAXIMIZE SUM(when txid_current() >= 0: p*x)"),
    ),
    # A query that looks like a near miss and applies: a right-hand aggregate is a window column of the source (H-19), so
    # S1 reads it as a source-valued bound. The conformance test asserts that `require` applies and that `auto`, `off`
    # and `require` answer alike; `S1_AGGREGATE_BOUNDS_NOW_APPLIED` gives the oracle the value of the bounds it checks.
    near_miss_applies=(("x(BOOL)", "SUM(x)<=COUNT(*)/0", "MAXIMIZE SUM(p*x)"),),
    near_miss_source="FROM (VALUES (1, 9.0::DOUBLE, 1), (2, 10.0::DOUBLE, 1)) t(id,p,cap)",
    near_miss_column="id",
    late_errors=(
        # A NaN score on the last of 5,000 rows, over every row and over the last of fifty groups. The window hands out
        # one group at a time, so a check that stands above it reaches a late group only when a parent reads that far.
        (
            _s1_late_error("x(BOOL) SUCH THAT SUM(x) <= 3 MAXIMIZE SUM(score*x)", score_sql=_S1_NAN_LAST),
            "score is NaN or infinite on some rows",
        ),
        (
            _s1_late_error("x(BOOL) SUCH THAT per g: SUM(x) by (g) <= 3 MAXIMIZE SUM(score*x)", score_sql=_S1_NAN_LAST),
            "score is NaN or infinite on some rows",
        ),
        # What a group's bound reads: a NaN limit on the last row of the last group.
        (
            _s1_late_error(
                "x(BOOL) SUCH THAT SUM(x) by (g) <= cap MAXIMIZE SUM(score*x)",
                cap_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 3.0 END",
            ),
            "the bound cap is NaN on some rows",
        ),
        # What a row fixes: a NaN value on the last row, and a last row that two clauses fix to both values.
        (
            _s1_late_error(
                "x(BOOL) SUCH THAT SUM(x) by (g) <= 3 AND x <= cap MAXIMIZE SUM(score*x)",
                cap_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "the bound cap is NaN on some rows",
        ),
        (
            _s1_late_error("x(BOOL) SUCH THAT SUM(x) by (g) <= 3 AND x >= (i=4999) AND x <= 0 MAXIMIZE SUM(score*x)"),
            "infeasible",
        ),
        # What a group's bound gives: an `=` limit that takes two values in the last group.
        (
            _s1_late_error(
                "x(BOOL) SUCH THAT SUM(x) by (g) = cap MAXIMIZE SUM(score*x)",
                cap_sql="CASE WHEN i=4999 THEN 2.0 ELSE 3.0 END",
            ),
            "takes more than one value",
        ),
        # A lower bound the last group cannot reach: its only row is alone in a group of its own.
        (
            _s1_late_error(
                "x(BOOL) SUCH THAT per g: SUM(x) by (g) >= 2 MAXIMIZE SUM(score*x)",
                g_sql="CASE WHEN i=4999 THEN 77 ELSE i % 50 END",
            ),
            "infeasible",
        ),
    ),
    late_column="i",
)


def _s2_late_error(decision_sql, score_sql="1.0"):
    """A 5,000-row problem whose last row is invalid, in two levels of groups: `h` (a hundred groups of fifty rows) lies
    inside `g` (fifty groups of a hundred rows). Rows 0 to 4998 are ordinary."""
    return f"""
        FROM (SELECT i, i % 50 AS g, i % 100 AS h, {score_sql} AS score FROM range(5000) t(i)) s
        DECIDE {decision_sql}
        """


_S2_QUOTAS = "SUM(x) by (g, h) <= 30 AND SUM(x) by (g) <= 60 AND SUM(x) <= 1000"

S2 = RuleFixture(
    rule="S2_LAMINAR_MATROID",
    condition="min/max SUM(s*x), x in {0,1}, nested SUM(x) <= k quotas",
    # The worked example of the rule's definition: one candidate per department, one per division, two overall. The
    # scores are distinct, so the optimum is unique and the rows equal the solver's exactly; the query also runs on
    # forced HiGHS.
    applies="""
        SELECT id, division, department, score, x FROM (
            FROM (VALUES (0, 'North', 'Sales', CAST('9.0' AS DOUBLE)), (1, 'North', 'Sales', CAST('7.0' AS DOUBLE)),
                         (2, 'North', 'Research', CAST('8.0' AS DOUBLE)), (3, 'South', 'Sales', CAST('6.0' AS DOUBLE)),
                         (4, 'South', 'Sales', CAST('5.0' AS DOUBLE)), (5, 'West', 'Research', CAST('4.0' AS DOUBLE)))
                t(id, division, department, score)
            DECIDE x(BOOL) SUCH THAT SUM(x) by (division, department) <= 1 AND SUM(x) by (division) <= 1
                AND SUM(x) <= 2
            MAXIMIZE SUM(score * x)
        ) q ORDER BY id
    """,
    empty_applies="""
        SELECT id, division, department, score, x FROM (
            FROM (SELECT * FROM (VALUES (0, 'North', 'Sales', CAST('9.0' AS DOUBLE)),
                                        (1, 'South', 'Sales', CAST('7.0' AS DOUBLE)))
                      t(id, division, department, score) WHERE false) t
            DECIDE x(BOOL) SUCH THAT SUM(x) by (division, department) <= 1 AND SUM(x) by (division) <= 1
                AND SUM(x) <= 2
            MAXIMIZE SUM(score * x)
        ) q ORDER BY id
    """,
    table_setup="""
        CREATE TEMP TABLE direct_s2_life(id INTEGER, division VARCHAR, score DOUBLE);
        INSERT INTO direct_s2_life VALUES (1,'a',2),(2,'a',9);
    """,
    table_applies="""
        SELECT id,x FROM (
            FROM direct_s2_life DECIDE x(BOOL)
            SUCH THAT SUM(x) by (division)<=1 AND SUM(x)<=2 MAXIMIZE SUM(score*x)
        ) q
    """,
    table_change="ALTER TABLE direct_s2_life ADD COLUMN extra INTEGER;",
    # The plain form with one condition changed, so the rule has to decline it: scopes that do not nest, a lower
    # limit, and a `when` on the coarser scope only. The other declines are `test_decidb_direct_decline.cpp`'s.
    near_misses=(
        ("x(BOOL)", "SUM(x) by (g) <= 1 AND SUM(x) by (h) <= 2", "MAXIMIZE SUM(p*x)"),
        ("x(BOOL)", "SUM(x) by (g) <= 1 AND SUM(x) >= 1", "MAXIMIZE SUM(p*x)"),
        ("x(BOOL)", "SUM(x) by (g, h) <= 1 AND when p > 9: SUM(x) by (g) <= 2", "MAXIMIZE SUM(p*x)"),
    ),
    # Forms that look like near misses and apply: a `when` on the finer scope only, a zero limit, a `per` on one scope
    # only (the form of the definition's example, which writes `per` on the finer clauses), and a limit read from the
    # data.
    near_miss_applies=(
        ("x(BOOL)", "when p > 9: SUM(x) by (g, h) <= 1 AND SUM(x) by (g) <= 2", "MAXIMIZE SUM(p*x)"),
        ("x(BOOL)", "SUM(x) by (g, h) <= 0 AND SUM(x) by (g) <= 2", "MAXIMIZE SUM(p*x)"),
        ("x(BOOL)", "per g: SUM(x) by (g) <= 1 AND SUM(x) <= 2", "MAXIMIZE SUM(p*x)"),
        ("x(BOOL)", "SUM(x) by (g) <= 1 AND SUM(x) <= cap", "MAXIMIZE SUM(p*x)"),
    ),
    near_miss_source="FROM (VALUES (1, 9.0::DOUBLE, 'a', 'x', 2), (2, 10.0::DOUBLE, 'a', 'y', 1), "
    "(3, 8.0::DOUBLE, 'b', 'x', 2)) t(id,p,g,h,cap)",
    near_miss_column="id",
    late_errors=(
        # A NaN score on the last of 5,000 rows, which the plan reads below its rank windows.
        (
            _s2_late_error(f"x(BOOL) SUCH THAT {_S2_QUOTAS} MAXIMIZE SUM(score*x)", score_sql=_S1_NAN_LAST),
            "score is NaN or infinite on some rows",
        ),
        # A level that fails only in its last group. Two rows fixed to 1 share one group of the finer level (rows 4899
        # and 4999 are in group h = 99) or of the coarser level (rows 4949 and 4999 are in group g = 49), and its cap is
        # 1: the verdict of each level is raised before the first row leaves, whichever level the group is in.
        (
            _s2_late_error(
                "x(BOOL) SUCH THAT SUM(x) by (g, h) <= 1 AND SUM(x) by (g) <= 60 AND SUM(x) <= 1000"
                " AND when i = 4899 OR i = 4999: x = 1 MAXIMIZE SUM(score*x)"
            ),
            "infeasible",
        ),
        (
            _s2_late_error(
                "x(BOOL) SUCH THAT SUM(x) by (g, h) <= 30 AND SUM(x) by (g) <= 1 AND SUM(x) <= 1000"
                " AND when i = 4949 OR i = 4999: x = 1 MAXIMIZE SUM(score*x)"
            ),
            "infeasible",
        ),
    ),
    late_column="i",
)


def _a1_late_error(decision_sql, cap_sql):
    """A 5,000-row problem whose last row is invalid. Rows 0 to 4998 are ordinary."""
    return f"""
        FROM (SELECT i, 1.0 AS score, {cap_sql} AS cap FROM range(5000) t(i)) s
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


def _r1_late_error(decision_sql, w_sql="1.0", cap_sql="10.0", g_sql="i % 50"):
    """A 5,000-row problem whose last row is invalid. Rows 0 to 4998 are ordinary. `g` is a key of fifty groups."""
    return f"""
        FROM (SELECT i, {w_sql} AS w, 1.0 AS v, {cap_sql} AS cap, {g_sql} AS g FROM range(5000) t(i)) s
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
    # The plain form with one condition changed, so the rule has to decline it: an INT decision (a unit cannot be
    # partly filled). The other declines and the forms that apply are `test_decidb_direct_decline.cpp`'s.
    near_misses=(("x(INT)", "x <= cap AND SUM(w*x) <= 5", "MAXIMIZE SUM(p*x)"),),
    near_miss_applies=(),
    near_miss_source="FROM (VALUES (1, 9.0::DOUBLE, 2.0::DOUBLE, 5.0::DOUBLE), "
    "(2, 10.0::DOUBLE, 3.0::DOUBLE, 3.0::DOUBLE)) t(id,p,w,cap)",
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
        # The same in the last of fifty budgets: a NaN weight, and a lower limit that the last group's only row cannot
        # reach.
        (
            _r1_late_error(
                "x(REAL) SUCH THAT x >= 0 AND x <= cap AND SUM(w*x) by (g) <= 60 MAXIMIZE SUM(v*x)",
                w_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "w is NaN or infinite on some rows",
        ),
        (
            _r1_late_error(
                "x(REAL) SUCH THAT x >= 0 AND x <= cap AND SUM(w*x) by (g) >= 100 MAXIMIZE SUM(v*x)",
                g_sql="CASE WHEN i=4999 THEN 77 ELSE i % 50 END",
            ),
            "infeasible",
        ),
    ),
    late_column="i",
)


def _n1_late_error(decision_sql, s_sql="1.0", k_sql="'a'", l_sql="9.0", w_sql="1.0"):
    """A 5,000-row problem whose last row is invalid. Rows 0 to 4998 are ordinary."""
    return f"""
        FROM (SELECT i, {s_sql} AS s, {k_sql} AS k, {l_sql} AS l, {w_sql} AS w FROM range(5000) t(i)) t
        DECIDE {decision_sql}
        """


N1 = RuleFixture(
    rule="N1_EUCLIDEAN_BALL",
    condition="SUM(x^2) <= B: linear objective or nearest point to a target",
    # Scores 3 and 4 under a limit of 25: x = 3, 4. The solver stops at its barrier tolerance (3.0005, 3.9996), so the
    # query rounds its output to two places, which makes its rows equal on both paths.
    applies="""
        SELECT id, s, ROUND(x, 2) AS x FROM (
            FROM (VALUES (1, 3.0::DOUBLE), (2, 4.0::DOUBLE), (3, -1.0::DOUBLE)) t(id, s)
            DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) <= 25
            MAXIMIZE SUM(s * x)
        ) q ORDER BY id
    """,
    empty_applies="""
        SELECT id, s, x FROM (
            FROM (SELECT * FROM (VALUES (1, 3.0::DOUBLE), (2, 4.0::DOUBLE)) t(id, s) WHERE false) t
            DECIDE x(REAL) SUCH THAT SUM(POWER(x, 2)) <= 25
            MAXIMIZE SUM(s * x)
        ) q ORDER BY id
    """,
    table_setup="""
        CREATE TEMP TABLE direct_n1_life(id INTEGER, s DOUBLE);
        INSERT INTO direct_n1_life VALUES (1,3),(2,4);
    """,
    table_applies="""
        SELECT id,x FROM (
            FROM direct_n1_life DECIDE x(REAL)
            SUCH THAT SUM(POWER(x,2))<=25 MAXIMIZE SUM(s*x)
        ) q
    """,
    table_change="ALTER TABLE direct_n1_life ADD COLUMN extra INTEGER;",
    # Each is the plain form with one condition changed: an INT decision, a limit whose coefficient reads the row (a
    # negative one makes the limit not convex), a row-valued slope beside a square in the objective, a ball off center, a
    # box the projection onto the ball does not hold for, and a limit from below.
    near_misses=(
        ("x(INT)", "SUM(POWER(x, 2)) <= 25", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "SUM(w*POWER(x, 2)) <= 25", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "SUM(w*x*x) <= 25", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "SUM(POWER(w*x, 2)) <= 25", "MINIMIZE SUM(POWER(x - p, 2))"),
        ("x(REAL)", "SUM(POWER(x - w, 2)) <= 25", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "x <= 50 AND SUM(POWER(x, 2)) <= 25", "MINIMIZE SUM(POWER(x, 2))"),
        ("x(REAL)", "x >= 1 AND SUM(POWER(x, 2)) <= 25", "MINIMIZE SUM(p*x)"),
        ("x(REAL)", "SUM(POWER(x, 2)) >= 1 AND SUM(POWER(x, 2)) <= 25", "MINIMIZE SUM(p*x)"),
    ),
    # Forms that look like near misses and apply: a limit read from the data or left as an aggregate of the rows, a box
    # written out, a free domain, a `by` key with a square, and a weight inside the square (the factor q of each row is
    # the weight times its slope squared, never negative).
    near_miss_applies=(
        ("x(REAL)", "SUM(POWER(x, 2)) <= lim", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "SUM(POWER(SQRT(w) * x, 2)) <= 25", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "SUM(POWER(w * x, 2)) <= 25", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "SUM(x*x) <= 0.5 * SUM(cap)", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "x >= 0 AND SUM(POWER(x, 2)) <= 25", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "x >= -1e30 AND SUM(POWER(x, 2)) <= 25", "MAXIMIZE SUM(p*x)"),
        ("x(REAL)", "SUM(POWER(x, 2)) by (id) <= 25", "MINIMIZE SUM(POWER(x - w, 2))"),
    ),
    near_miss_source="FROM (VALUES (1, 9.0::DOUBLE, 2.0::DOUBLE, 5.0::DOUBLE, 6.0::DOUBLE), "
    "(2, 10.0::DOUBLE, 3.0::DOUBLE, 3.0::DOUBLE, 6.0::DOUBLE)) t(id,p,w,cap,lim)",
    near_miss_column="id",
    late_errors=(
        # Errors N1 raises, on the last of 5,000 rows: a group with a negative limit, an improving row outside every ball,
        # a NaN score, a NaN limit and a score too large to square.
        (
            _n1_late_error(
                "x(REAL) SUCH THAT SUM(POWER(x,2)) by (k) <= l MAXIMIZE SUM(s*x)",
                k_sql="CASE WHEN i=4999 THEN 'b' ELSE 'a' END",
                l_sql="CASE WHEN i=4999 THEN -1.0 ELSE 9.0 END",
            ),
            "infeasible",
        ),
        (
            _n1_late_error(
                "x(REAL) SUCH THAT SUM(POWER(x,2)) by (k) <= 9 MAXIMIZE SUM(s*x)",
                k_sql="CASE WHEN i=4999 THEN NULL ELSE 'a' END",
            ),
            "unbounded",
        ),
        (
            _n1_late_error(
                "x(REAL) SUCH THAT SUM(POWER(x,2)) <= 9 MAXIMIZE SUM(s*x)",
                s_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "s is NaN or infinite on some rows",
        ),
        (
            _n1_late_error(
                "x(REAL) SUCH THAT SUM(POWER(x,2)) by (k) <= l MAXIMIZE SUM(s*x)",
                l_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 9.0 END",
            ),
            "NaN on some rows",
        ),
        # A NaN score on the last row of the last of fifty groups.
        (
            _n1_late_error(
                "x(REAL) SUCH THAT SUM(POWER(x,2)) by (k) <= 9 MAXIMIZE SUM(s*x)",
                s_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
                k_sql="(i % 50)::VARCHAR",
            ),
            "s is NaN or infinite on some rows",
        ),
        # A score past a DOUBLE when squared, in a group of its own: nothing but this row reads its sum.
        (
            _n1_late_error(
                "x(REAL) SUCH THAT SUM(POWER(x,2)) by (k) <= 9 MAXIMIZE SUM(s*x)",
                s_sql="CASE WHEN i=4999 THEN 1e200 ELSE 1.0 END",
                k_sql="i::VARCHAR",
            ),
            "too large for a DOUBLE",
        ),
        # A weight inside the square on the last row: a negative one under SQRT, a NaN one, and one whose square is past a
        # DOUBLE.
        (
            _n1_late_error(
                "x(REAL) SUCH THAT SUM(POWER(SQRT(w) * x, 2)) <= 9 MAXIMIZE SUM(s*x)",
                w_sql="CASE WHEN i=4999 THEN -1.0 ELSE 1.0 END",
            ),
            "cannot take square root of a negative number",
        ),
        (
            _n1_late_error(
                "x(REAL) SUCH THAT SUM(POWER(SQRT(w) * x, 2)) <= 9 MAXIMIZE SUM(s*x)",
                w_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "SQRT(w) is NaN or infinite on some rows",
        ),
        (
            _n1_late_error(
                "x(REAL) SUCH THAT SUM(POWER(w * x, 2)) by (k) <= 9 MAXIMIZE SUM(s*x)",
                w_sql="CASE WHEN i=4999 THEN 1e200 ELSE 1.0 END",
                k_sql="i::VARCHAR",
            ),
            "too large for a DOUBLE",
        ),
    ),
    late_column="i",
    # HiGHS has no quadratic constraints.
    forced_backend_cli="decidb_cli_gurobi",
)


def _a2_late_error(decision_sql, v_sql="1.0", cap_sql="5.0"):
    """A 5,000-row problem whose last row is invalid. Rows 0 to 4998 are ordinary. `g` is a key of seven classes."""
    return f"""
        FROM (SELECT i, i % 7 AS g, {v_sql} AS v, {cap_sql} AS cap FROM range(5000) t(i)) s
        DECIDE {decision_sql}
        """


A2 = RuleFixture(
    rule="A2_SINGLE_FACILITY_LOCATION",
    condition=(
        "min SUM((c - v)^2), SUM(ABS(c - v)) or MAX(ABS(c - v)), one shared c for each class, "
        "MAX(lo) <= c <= MIN(hi)"
    ),
    # The worked example of the rule's definition: the mean of 2, 4 and 9 is 5, inside the interval, and every row
    # carries it. Forced HiGHS stops at its own tolerance (4.9999999), so the query rounds its output to two places,
    # which makes its rows equal on every path.
    applies="""
        SELECT id, v, ROUND(c, 2) AS c FROM (
            FROM (VALUES (0, CAST('2.0' AS DOUBLE)), (1, CAST('4.0' AS DOUBLE)), (2, CAST('9.0' AS DOUBLE))) t(id, v)
            DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10
            MINIMIZE SUM(POWER(c - v, 2))
        ) q ORDER BY id
    """,
    empty_applies="""
        SELECT id, v, c FROM (
            FROM (SELECT * FROM (VALUES (0, CAST('2.0' AS DOUBLE)), (1, CAST('4.0' AS DOUBLE))) t(id, v)
                  WHERE false) t
            DECIDE per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10
            MINIMIZE SUM(POWER(c - v, 2))
        ) q ORDER BY id
    """,
    table_setup="""
        CREATE TEMP TABLE direct_a2_life(id INTEGER, v DOUBLE);
        INSERT INTO direct_a2_life VALUES (1,2),(2,4);
    """,
    table_applies="""
        SELECT id,c FROM (
            FROM direct_a2_life DECIDE per (): c(REAL)
            SUCH THAT c BETWEEN 0 AND 10 MINIMIZE SUM(POWER(c - v, 2))
        ) q
    """,
    table_change="ALTER TABLE direct_a2_life ADD COLUMN extra INTEGER;",
    # Each is the plain form with one condition changed: a whole-numbered decision (the mean is not a whole number), a
    # linear term beside the square, a weight fixed only within the query, and the losses A2 does not take.
    near_misses=(
        ("per (): c(INT)", "c BETWEEN 0 AND 10", "MINIMIZE SUM(POWER(c - p, 2))"),
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE SUM(POWER(c - p, 2) + w * c)"),
        # An absolute value weighted by the data (the solver weighs it by the sign of each row's weight), a maximized
        # one, the maximum of one whose weight is read from the data or whose sense is a maximum, and the L0 norm.
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE SUM(w * ABS(c - p))"),
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MAXIMIZE SUM(ABS(c - p))"),
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE MAX(w * ABS(c - p))"),
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MAXIMIZE MAX(ABS(c - p))"),
        # The L0 norm is declined (the IR marks it not proved), so the solver keeps it.
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE norm(c - p, 0)"),
        # A weight fixed only within the query is no value of the source row.
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE SUM(txid_current() * POWER(c - p, 2))"),
        # An objective `when` that is not a source predicate: on the objective and on its sum.
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE when txid_current() >= 0: SUM(POWER(c - p, 2))"),
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE SUM(when txid_current() >= 0: POWER(c - p, 2))"),
    ),
    # Forms that look like near misses and apply: the square written the other way round, a slope on the decision, a
    # factor, a maximized negative square, a weight read from the data, a `when`, an interval open on one side, a bound
    # read from the data (the interval of a class is the tightest of its rows'), and a floor lowered under a `when`.
    near_miss_applies=(
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE SUM(POWER(p - c, 2))"),
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE SUM(POWER(2 * c - p, 2))"),
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE 3 * SUM(POWER(c - p, 2)) / 2"),
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MAXIMIZE -SUM(POWER(c - p, 2))"),
        # The other losses: the absolute value (two spellings) and the maximum absolute value (two), keyed or not.
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE SUM(ABS(c - p))"),
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE norm(c - p, 1)"),
        ("per cap: c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE 3 * SUM(2 * ABS(p - 2 * c))"),
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE MAX(ABS(c - p))"),
        ("per cap: c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE norm(c - p, 'inf')"),
        # A weight read from the data, of either sign once the sense is applied: the plan reads the sign of the total.
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE SUM(w * POWER(c - p, 2))"),
        ("per cap: c(REAL)", "c BETWEEN 0 AND 10", "MAXIMIZE SUM(w * POWER(c - p, 2))"),
        ("per (): c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE when cap > 3: SUM(POWER(c - p, 2))"),
        ("per (): c(REAL)", "c <= 10", "MINIMIZE SUM(POWER(c - p, 2))"),
        ("per (): c(REAL)", "c >= -1e30 AND c <= 10", "MINIMIZE SUM(POWER(c - p, 2))"),
        # A keyed decision: one center per class of cap.
        ("per cap: c(REAL)", "c BETWEEN 0 AND 10", "MINIMIZE SUM(POWER(c - p, 2))"),
        ("per (): c(REAL)", "c BETWEEN 0 AND cap", "MINIMIZE SUM(POWER(c - p, 2))"),
        ("per cap: c(REAL)", "when p > 9: c >= w - 4 AND c <= 10", "MINIMIZE SUM(POWER(c - p, 2))"),
        ("per (): c(REAL)", "when p > 9: c >= -4", "MINIMIZE SUM(POWER(c - p, 2))"),
    ),
    near_miss_source="FROM (VALUES (1, 9.0::DOUBLE, 5, 2.0::DOUBLE), (2, 10.0::DOUBLE, 3, 4.0::DOUBLE)) t(id,p,cap,w)",
    near_miss_column="id",
    late_errors=(
        # Errors A2 raises, on the last of 5,000 rows: a NaN value and a value too large for its objective coefficient.
        (
            _a2_late_error(
                "per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10 MINIMIZE SUM(POWER(c - v, 2))",
                v_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "v is NaN or infinite on some rows",
        ),
        (
            _a2_late_error(
                "per (): c(REAL) SUCH THAT c BETWEEN 0 AND 10 MINIMIZE SUM(POWER(c - v, 2))",
                v_sql="CASE WHEN i=4999 THEN 1e308 ELSE 1.0 END",
            ),
            "too large for a DOUBLE",
        ),
        # A NaN bound on the last row, for a query-wide and for a keyed decision.
        (
            _a2_late_error(
                "per (): c(REAL) SUCH THAT c <= cap MINIMIZE SUM(POWER(c - v, 2))",
                cap_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 5.0 END",
            ),
            "the bound cap is NaN on some rows",
        ),
        (
            _a2_late_error(
                "per g: c(REAL) SUCH THAT c <= cap MINIMIZE SUM(POWER(c - v, 2))",
                cap_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 5.0 END",
            ),
            "the bound cap is NaN on some rows",
        ),
        # The same on a keyed decision: the last row belongs to one of seven classes.
        (
            _a2_late_error(
                "per g: c(REAL) SUCH THAT c BETWEEN 0 AND 10 MINIMIZE SUM(POWER(c - v, 2))",
                v_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "v is NaN or infinite on some rows",
        ),
    ),
    late_column="i",
)


_O1_PER_SIDE = "per n: SUM(x) by (n) = 1 AND per r: SUM(x) by (r) = 1"
_O1_SQUARED = "MINIMIZE SUM(POWER(n.n_nationkey - r.r_regionkey, 2) * x)"
# The rule reads positions that are declared NOT NULL on a table each side scans, which a VALUES list is not. TPC-H's
# `nation` and `region` declare every column NOT NULL, and five nations (keys 5 to 9) pair with the five regions.
_O1_ALL_PAIRS = "FROM nation n CROSS JOIN region r"
_O1_SOURCE = f"{_O1_ALL_PAIRS} WHERE n.n_nationkey BETWEEN 5 AND 9"


O1 = RuleFixture(
    rule="O1_MONGE_ASSIGNMENT",
    condition=(
        "x in {0,1}, SUM(x) = 1 in each left and each right class, "
        "min SUM(ABS(a - b)*x) or SUM((a - b)^2*x), min/max SUM(a*b*x)"
    ),
    # Nations 5 to 9 against regions 0 to 4: the ranks pair 5-0 up to 9-4. The optimum is unique, so the rows equal the
    # solver's exactly and the query also runs on forced HiGHS. The one-side WHERE is part of the plain form.
    applies=f"""
        SELECT n_nationkey, r_regionkey, x FROM (
            {_O1_SOURCE}
            DECIDE x(BOOL) SUCH THAT {_O1_PER_SIDE}
            {_O1_SQUARED}
        ) q ORDER BY n_nationkey, r_regionkey
    """,
    # A region filter no statistic proves empty, so the side is still a scan of the table.
    empty_applies=f"""
        SELECT n_nationkey, r_regionkey, x FROM (
            {_O1_SOURCE} AND r.r_name LIKE '%none%'
            DECIDE x(BOOL) SUCH THAT {_O1_PER_SIDE}
            {_O1_SQUARED}
        ) q ORDER BY n_nationkey, r_regionkey
    """,
    table_setup="""
        CREATE TEMP TABLE direct_o1_left(lid INTEGER, lpos DOUBLE NOT NULL);
        CREATE TEMP TABLE direct_o1_right(rid INTEGER, rpos DOUBLE NOT NULL);
        INSERT INTO direct_o1_left VALUES (1,1),(2,4);
        INSERT INTO direct_o1_right VALUES (7,2),(8,6);
    """,
    table_applies="""
        SELECT lid, rid, x FROM (
            FROM direct_o1_left l CROSS JOIN direct_o1_right r
            DECIDE x(BOOL) SUCH THAT per l: SUM(x) by (l) = 1 AND per r: SUM(x) by (r) = 1
            MINIMIZE SUM(POWER(l.lpos - r.rpos, 2) * x)
        ) q
    """,
    table_change="ALTER TABLE direct_o1_left ADD COLUMN extra INTEGER;",
    # Each is the plain form with one condition changed: a key column that does not determine the position (n_name is
    # NOT NULL and never repeats here, but the catalog declares no key on it), a position that is computed, a third
    # constraint, and a `when` in a constraint and on the objective. A filter across both sides (a join, not a cross
    # product) and a position not declared NOT NULL need a source of their own, so they are
    # `test_direct_o1_monge_assignment.py`'s.
    near_misses=(
        ("x(BOOL)", "per n.n_name: SUM(x) by (n.n_name) = 1 AND per r: SUM(x) by (r) = 1", _O1_SQUARED),
        ("x(BOOL)", _O1_PER_SIDE, "MINIMIZE SUM(POWER(n.n_nationkey * 2 - r.r_regionkey, 2) * x)"),
        ("x(BOOL)", f"{_O1_PER_SIDE} AND SUM(x) <= 5", _O1_SQUARED),
        ("x(BOOL)", "per n: SUM(when n.n_nationkey > 0: x) by (n) = 1 AND per r: SUM(x) by (r) = 1", _O1_SQUARED),
        ("x(BOOL)", _O1_PER_SIDE, "MINIMIZE when n.n_nationkey > 0: SUM(POWER(n.n_nationkey - r.r_regionkey, 2) * x)"),
    ),
    # Forms that look like near misses and apply: a key column that is the position (the key holds it), the difference
    # the other way round and a constant factor.
    near_miss_applies=(
        ("x(BOOL)", "per n.n_nationkey: SUM(x) by (n.n_nationkey) = 1 AND per r: SUM(x) by (r) = 1", _O1_SQUARED),
        ("x(BOOL)", _O1_PER_SIDE, "MINIMIZE SUM(POWER(r.r_regionkey - n.n_nationkey, 2) * x)"),
        ("x(BOOL)", _O1_PER_SIDE, "MINIMIZE 2 * SUM(POWER(n.n_nationkey - r.r_regionkey, 2) * x)"),
    ),
    near_miss_source=_O1_SOURCE,
    near_miss_column="n_nationkey",
    late_errors=(
        # The classes of the sides do not match (25 nations, 5 regions), which only a read of every row can tell.
        (f"{_O1_ALL_PAIRS} DECIDE x(BOOL) SUCH THAT {_O1_PER_SIDE} {_O1_SQUARED}", "infeasible"),
    ),
    late_column="n_nationkey",
)


def _s3_late_error(decision_sql, w_sql="1.0", cap_sql="3.0"):
    """A 5,000-row problem whose last row is invalid. Rows 0 to 4998 are ordinary. `g` is a key of fifty groups."""
    return f"""
        FROM (SELECT i, i % 50 AS g, {w_sql} AS w, {cap_sql} AS cap FROM range(5000) t(i)) s
        DECIDE {decision_sql}
        """


S3 = RuleFixture(
    rule="S3_MAX_CARDINALITY_KNAPSACK",
    condition="max c*SUM(x), SUM(w*x) <= B; min c*SUM(x), SUM(w*x) >= B",
    # The worked example of the rule's definition: weights 2, 3, 4 and 6 under a budget of 9 select the first three. The
    # weights are distinct, so the optimum is unique and the rows equal the solver's exactly; the query also runs on
    # forced HiGHS.
    applies="""
        SELECT id, w, x FROM (
            FROM (VALUES (0, CAST('2.0' AS DOUBLE)), (1, CAST('3.0' AS DOUBLE)), (2, CAST('4.0' AS DOUBLE)),
                         (3, CAST('6.0' AS DOUBLE))) t(id, w)
            DECIDE x(BOOL) SUCH THAT SUM(w * x) <= 9
            MAXIMIZE SUM(x)
        ) q ORDER BY id
    """,
    empty_applies="""
        SELECT id, w, x FROM (
            FROM (SELECT * FROM (VALUES (0, CAST('2.0' AS DOUBLE)), (1, CAST('3.0' AS DOUBLE))) t(id, w)
                  WHERE false) t
            DECIDE x(BOOL) SUCH THAT SUM(w * x) <= 9
            MAXIMIZE SUM(x)
        ) q ORDER BY id
    """,
    table_setup="""
        CREATE TEMP TABLE direct_s3_life(id INTEGER, w DOUBLE);
        INSERT INTO direct_s3_life VALUES (1,2),(2,3);
    """,
    table_applies="""
        SELECT id,x FROM (
            FROM direct_s3_life DECIDE x(BOOL)
            SUCH THAT SUM(w*x)<=4 MAXIMIZE SUM(x)
        ) q
    """,
    table_change="ALTER TABLE direct_s3_life ADD COLUMN extra INTEGER;",
    # Each is the plain form with one condition changed: a value per row in the objective (the general 0-1 knapsack), a
    # strict bound over an integer weight, an objective `when` that is not the budget's (a two-value knapsack), a
    # second constraint and a minimum with a maximized count (select every row). A weight of exactly 1 is S1's count,
    # which `require` answers with S1, so it is no near miss here; `test_direct_s3_max_cardinality_knapsack.py` checks
    # that S3 leaves it to S1.
    near_misses=(
        ("x(BOOL)", "SUM(w*x) <= 4", "MAXIMIZE SUM(p*x)"),
        ("x(BOOL)", "SUM(wi*x) < 4", "MAXIMIZE SUM(x)"),
        ("x(BOOL)", "when p > 9: SUM(w*x) <= 4", "MAXIMIZE when p > 5: SUM(x)"),
        ("x(BOOL)", "SUM(w*x) <= 4 AND SUM(p*x) <= 12", "MAXIMIZE SUM(x)"),
        ("x(BOOL)", "SUM(w*x) >= 3", "MAXIMIZE SUM(x)"),
    ),
    # Forms that look like near misses and apply: a constant factor on the count, a constant weight of 2, a `when` on
    # the budget (the row it drops is selected), the same `when` on the objective (the row is not selected), a row
    # fixed to 1 (it takes its weight from the budget) and a minimum with a minimized count.
    near_miss_applies=(
        ("x(BOOL)", "SUM(w*x) <= 4", "MAXIMIZE 2 * SUM(x)"),
        ("x(BOOL)", "SUM(2*x) <= 4", "MAXIMIZE SUM(x)"),
        ("x(BOOL)", "when p > 9: SUM(w*x) <= 4", "MAXIMIZE SUM(x)"),
        ("x(BOOL)", "when p > 9: SUM(w*x) <= 4", "MAXIMIZE when p > 9: SUM(x)"),
        ("x(BOOL)", "SUM(w*x) <= 4 AND when p > 9: x = 1", "MAXIMIZE SUM(x)"),
        ("x(BOOL)", "SUM(w*x) >= 3", "MINIMIZE SUM(x)"),
    ),
    near_miss_source="FROM (VALUES (1, 9.0::DOUBLE, 2.0::DOUBLE, 2), (2, 10.0::DOUBLE, 3.0::DOUBLE, 3)) t(id,p,w,wi)",
    near_miss_column="id",
    late_errors=(
        # A NaN weight on the last of 5,000 rows.
        (
            _s3_late_error(
                "x(BOOL) SUCH THAT SUM(w*x) <= 3000 MAXIMIZE SUM(x)",
                w_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "w is NaN or infinite on some rows",
        ),
        # The same in the last of fifty budgets, and a limit below zero that only the last group's last row gives.
        (
            _s3_late_error(
                "x(BOOL) SUCH THAT SUM(w*x) by (g) <= 30 MAXIMIZE SUM(x)",
                w_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "w is NaN or infinite on some rows",
        ),
        (
            _s3_late_error(
                "x(BOOL) SUCH THAT SUM(w*x) by (g) <= cap MAXIMIZE SUM(x)",
                cap_sql="CASE WHEN i=4999 THEN -1.0 ELSE 3.0 END",
            ),
            "infeasible",
        ),
    ),
    late_column="i",
)


def _r4_late_error(decision_sql, d_sql="2.0", cap_sql="4.0", w_sql="1.0", g_sql="i % 50", l_sql="9.0"):
    """A 5,000-row problem whose last row is invalid. Rows 0 to 4998 are ordinary. `g` is a key of fifty groups."""
    return f"""
        FROM (SELECT i, {d_sql} AS d, {cap_sql} AS cap, {w_sql} AS w, {g_sql} AS g, {l_sql} AS l
              FROM range(5000) t(i)) s
        DECIDE {decision_sql}
        """


_R4_PLAIN = "x <= cap AND SUM(x) <= 9 MAXIMIZE MIN(x / d)"

R4 = RuleFixture(
    rule="R4_MAX_MIN_FAIRNESS",
    condition="max MIN(c*x), 0 <= x <= cap, SUM(w*x) <= B",
    # The worked example of the rule's definition: demands 2 and 4 under a budget of 9 and capacities 4 and 8 give the
    # level 1.5 and the amounts 3 and 6. The optimum is unique, so the rows equal the solver's exactly after rounding
    # (its answers carry the solver's tolerance); the query also runs on forced HiGHS.
    applies=f"""
        SELECT id, d, ROUND(x, 4) AS x FROM (
            FROM (VALUES (1, 2.0::DOUBLE, 4.0::DOUBLE), (2, 4.0::DOUBLE, 8.0::DOUBLE)) t(id, d, cap)
            DECIDE x(REAL) SUCH THAT {_R4_PLAIN}
        ) q ORDER BY id
    """,
    empty_applies=f"""
        SELECT id, d, x FROM (
            FROM (SELECT * FROM (VALUES (1, 2.0::DOUBLE, 4.0::DOUBLE), (2, 4.0::DOUBLE, 8.0::DOUBLE)) t(id, d, cap)
                  WHERE false) t
            DECIDE x(REAL) SUCH THAT {_R4_PLAIN}
        ) q ORDER BY id
    """,
    table_setup="""
        CREATE TEMP TABLE direct_r4_life(id INTEGER, d DOUBLE, cap DOUBLE);
        INSERT INTO direct_r4_life VALUES (1,2,4),(2,4,8);
    """,
    table_applies=f"""
        SELECT id,x FROM (
            FROM direct_r4_life DECIDE x(REAL)
            SUCH THAT {_R4_PLAIN}
        ) q
    """,
    table_change="ALTER TABLE direct_r4_life ADD COLUMN extra INTEGER;",
    # Each is the plain form with one condition changed: an INT decision, a budget from below, a budget per class, a
    # decision shared by a key under a budget of finer groups or under a `when`, a decision of the whole query, a second
    # budget clause, a lower bound on the decision, a per-row constant inside the MIN, a second part of the objective, a
    # constant factor on the MIN and a minimized MIN.
    near_misses=(
        ("x(INT)", "x <= cap AND SUM(x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= cap AND SUM(x) >= 1", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= cap AND per id: SUM(x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("per w: x(REAL)", "x <= cap AND SUM(x) by (id) <= 9", "MAXIMIZE MIN(x / d)"),
        ("per w: x(REAL)", "x <= cap AND when cap > 1: SUM(x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("per (): x(REAL)", "x <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= cap AND SUM(x) <= 9 AND SUM(2 * x) <= 20", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x >= 1 AND x <= cap AND SUM(x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= cap AND SUM(x) <= 9", "MAXIMIZE MIN(x / d + w)"),
        ("x(REAL)", "x <= cap AND SUM(x) <= 9", "MAXIMIZE MIN(x / d) + MIN(x / w)"),
        ("x(REAL)", "x <= cap AND SUM(x) <= 9", "MAXIMIZE 2 * MIN(x / d)"),
        ("x(REAL)", "x <= cap AND SUM(x) <= 9", "MINIMIZE MIN(x / d)"),
    ),
    # Forms that look like near misses and apply: the floor written out, a constant weight other than 1, a negative one
    # and one read from the data, a budget per group, under a `when` and read from the data, a decision shared by a key, no capacity, a constant
    # capacity, and a literal constant inside the MIN (canonicalization moves it out of the MIN).
    near_miss_applies=(
        ("x(REAL)", "x >= 0 AND x <= cap AND SUM(x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= cap AND SUM(2 * x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= cap AND SUM(-x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= cap AND SUM(w * x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "SUM(x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= cap AND SUM(x) by (id) <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= cap AND when w > 1: SUM(x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= cap AND SUM(x) <= cap", "MAXIMIZE MIN(x / d)"),
        ("per w: x(REAL)", "x <= cap AND SUM(x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("per w: x(REAL)", "x <= cap AND SUM(x) by (w) <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= 3 AND SUM(x) <= 9", "MAXIMIZE MIN(x / d)"),
        ("x(REAL)", "x <= cap AND SUM(x) <= 9", "MAXIMIZE MIN(x / d + 1)"),
    ),
    near_miss_source="FROM (VALUES (1, 2.0::DOUBLE, 1.0::DOUBLE, 4.0::DOUBLE), "
    "(2, 4.0::DOUBLE, 2.0::DOUBLE, 8.0::DOUBLE)) t(id,d,w,cap)",
    near_miss_column="id",
    late_errors=(
        # Errors R4 raises, on the last of 5,000 rows: a zero demand, a NaN demand, a NaN weight, a NaN capacity and a negative one.
        (
            _r4_late_error(f"x(REAL) SUCH THAT {_R4_PLAIN}", d_sql="CASE WHEN i=4999 THEN 0.0 ELSE 2.0 END"),
            "1 / d is NaN or infinite on some rows",
        ),
        (
            _r4_late_error(f"x(REAL) SUCH THAT {_R4_PLAIN}", d_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 2.0 END"),
            "1 / d is NaN or infinite on some rows",
        ),
        (
            _r4_late_error(
                "x(REAL) SUCH THAT x <= cap AND SUM(w * x) <= 9 MAXIMIZE MIN(x / d)",
                w_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "w is NaN or infinite on some rows",
        ),
        # The last of fifty budgets: a NaN weight, a NaN limit and a negative one.
        (
            _r4_late_error(
                "x(REAL) SUCH THAT x <= cap AND SUM(w * x) by (g) <= 9 MAXIMIZE MIN(x / d)",
                w_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "w is NaN or infinite on some rows",
        ),
        (
            _r4_late_error(
                "x(REAL) SUCH THAT x <= cap AND SUM(x) by (g) <= l MAXIMIZE MIN(x / d)",
                l_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 9.0 END",
            ),
            "the bound l is NaN on some rows",
        ),
        (
            _r4_late_error(
                "x(REAL) SUCH THAT x <= cap AND SUM(x) by (g) <= l MAXIMIZE MIN(x / d)",
                l_sql="CASE WHEN i=4999 THEN -1.0 ELSE 9.0 END",
            ),
            "infeasible",
        ),
        # A decision shared by the rows of fifty keys: a NaN weight and a negative capacity in the last.
        (
            _r4_late_error(
                "per g: x(REAL) SUCH THAT x <= cap AND SUM(w * x) by (g) <= 9 MAXIMIZE MIN(x / d)",
                w_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 1.0 END",
            ),
            "w is NaN or infinite on some rows",
        ),
        (
            _r4_late_error(
                "per g: x(REAL) SUCH THAT x <= cap AND SUM(x) <= 9 MAXIMIZE MIN(x / d)",
                cap_sql="CASE WHEN i=4999 THEN -1.0 ELSE 4.0 END",
            ),
            "infeasible",
        ),
        (
            _r4_late_error(
                f"x(REAL) SUCH THAT {_R4_PLAIN}", cap_sql="CASE WHEN i=4999 THEN 'NaN'::DOUBLE ELSE 4.0 END"
            ),
            "the bound cap is NaN on some rows",
        ),
        (
            _r4_late_error(f"x(REAL) SUCH THAT {_R4_PLAIN}", cap_sql="CASE WHEN i=4999 THEN -1.0 ELSE 4.0 END"),
            "infeasible",
        ),
    ),
    late_column="i",
)


RULE_FIXTURES = (S1, S2, A1, R1, N1, A2, O1, S3, R4)


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
