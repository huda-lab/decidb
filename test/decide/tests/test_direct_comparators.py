"""The hand-written comparators of `benchmark/decide/direct_solve/hand/`: one per benchmark query, giving its answer.

`run.py --compare` times each query's direct plan beside its comparator, which is only a measure of the plan if both
compute the same answer. This checks that on the test database (TPC-H at a small scale): the row count, the sum of the
decision and the objective that `run.py` reads agree to 1e-9. Rows may differ in the last digits of a sum, since the two
plans return them in different orders, and R1's marginal offer takes the budget a difference of two long sums leaves it.
"""

import importlib.util
from pathlib import Path

import pytest

from ._direct_oracle_common import _allow_direct_path  # noqa: F401  (autouse fixture)

BENCHMARK = Path(__file__).resolve().parents[3] / "benchmark" / "decide" / "direct_solve"


def _load_runner():
    spec = importlib.util.spec_from_file_location("direct_solve_run", BENCHMARK / "run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUNNER = _load_runner()
QUERIES = sorted(path.stem for path in (BENCHMARK / "queries").glob("*.sql"))


def test_every_benchmark_query_has_one_comparator_and_no_comparator_is_left_over():
    assert sorted(path.stem for path in (BENCHMARK / "hand").glob("*.sql")) == QUERIES


@pytest.mark.correctness
@pytest.mark.parametrize("name", QUERIES)
def test_the_comparator_gives_the_answer_of_the_direct_plan(decidb_cli, name):
    body, decision, objective = RUNNER.load_query(name)
    parent = f"SELECT COUNT(*), SUM({decision}), {objective} FROM (%s) q"
    direct, _ = decidb_cli.execute(f"SET decide_direct_solve='require'; SET threads=1; {parent % body}")
    hand, _ = decidb_cli.execute(f"SET threads=1; {parent % RUNNER.load_hand(name)}")
    assert direct[0][0] == hand[0][0] > 0
    for a, b in zip(direct[0][1:], hand[0][1:]):
        assert a == pytest.approx(b, rel=1e-9, abs=1e-9), (direct, hand)
