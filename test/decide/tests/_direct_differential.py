"""Seeded differential comparison of the direct path with the solver path.

Any direct-solve rule can be checked the same way: a generator produces random small
queries, each runs under `require` (direct) and `off` (solver), and the two must agree.
A rule supplies only its generator. A generator takes a `random.Random` and returns
`(sql, shaped)`: the query, whose single result row is `(row count, primary objective)`,
and whether it was built as the rule's shape rather than as a deliberate near miss.
"""

import re

import pytest

from decidb_cli import DecidBCliError

# (pattern, class). The first match wins; anything unmatched compares by text.
ERROR_CLASSES = [
    (r"(?i)parser error|syntax error", "parser"),
    (r"decide_direct_solve=require", "direct_miss"),
    (r"(?i)infeasible", "infeasible"),
    (r"(?i)empty row set|empty aggregate", "empty_aggregate"),
    (r"(?i)is NULL|NULL or NaN|contains NULL", "null_bound"),
    (r"(?i)varies", "equality_varies"),
]


def error_class(message):
    for pattern, name in ERROR_CLASSES:
        if re.search(pattern, message):
            return name
    return "other:" + message.strip().splitlines()[0][:80]


def outcome(cli, mode, sql):
    try:
        rows, _ = cli.execute(f"SET decide_direct_solve='{mode}'; {sql}")
    except DecidBCliError as error:
        return "error", error_class(str(error))
    return "ok", rows[0]


def compare_direct_with_solver(cli, generate, rng, cases, min_hit_share=0.7, min_compared_share=0.2):
    """Runs `cases` generated queries on both paths and asserts they agree.

    A query the rule does not prove is skipped: the solver path stays authoritative.
    Otherwise both paths succeed or fail together, failures have the same class, and
    successes agree on row count and primary objective. The selected count is not
    compared, because a tied zero-contribution row may be chosen by only one path.
    """
    shaped = shaped_hits = compared = 0
    for _ in range(cases):
        sql, rule_shaped = generate(rng)
        shaped += rule_shaped
        direct = outcome(cli, "require", sql)
        assert direct != ("error", "parser"), f"generator produced invalid syntax:\n{sql}"
        if direct == ("error", "direct_miss"):
            continue
        shaped_hits += rule_shaped
        solver = outcome(cli, "off", sql)
        assert solver != ("error", "parser"), f"generator produced invalid syntax:\n{sql}"
        assert direct[0] == solver[0], f"direct {direct} vs solver {solver}\n{sql}"
        if direct[0] == "error":
            assert direct[1] == solver[1], f"error class differs: direct {direct[1]} vs solver {solver[1]}\n{sql}"
            continue
        compared += 1
        direct_count, direct_objective = direct[1]
        solver_count, solver_objective = solver[1]
        assert direct_count == solver_count, f"row count differs\n{sql}"
        if direct_objective is None or solver_objective is None:
            assert direct_objective == solver_objective, f"objective differs\n{sql}"
        else:
            assert direct_objective == pytest.approx(solver_objective, rel=1e-6, abs=1e-6), (
                f"objective differs: direct {direct_objective} vs solver {solver_objective}\n{sql}"
            )
    # Guard against a generator drifting into vacuity: most rule-shaped queries must
    # reach the direct path and a meaningful share must succeed on both paths.
    assert shaped_hits >= min_hit_share * shaped, f"only {shaped_hits} of {shaped} rule-shaped queries hit"
    assert compared >= min_compared_share * cases, f"only {compared} successful comparisons"
