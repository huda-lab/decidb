"""A few seeds of each fuzzer in ``test/decide/fuzz`` (run more with ``python -m fuzz``), and the proof that each can
fail: every deliberately broken oracle a module carries reports a mismatch within a few seeds.

The runner's verdicts are pinned one branch at a time, the CLI's outcomes with stand-in executables for the ones a
healthy build never shows (a crash, an INTERNAL error, a hang, a refusal of every query).
"""

import os
import random
import re
import sys

import pytest

from fuzz import __main__ as entry
from fuzz import bounds, known, nests, order_keys, runner
from fuzz.runner import Case, Expected, Outcome, Reference, judge
from solver.types import ObjSense, VarType
from tests._anr_oracle import (
    PER_ROW,
    AnrModel,
    Col,
    Decision,
    Objective,
    Over,
    Range,
    Var,
    clause,
    eq,
    ge,
    le,
    next_,
    previous,
    reduce,
)

#: Seeds each module answers cleanly today on both solvers, chosen to judge values: each has instances.
SEEDS = {
    # A nest under `=`, a nest over data and one over pinned decisions read at every row, and `<>` over a MAX of a MIN,
    # which the language reads.
    "nests": (22, 8, 5, 24),
    # A range over a keyed decision, a position holding two payloads (an expected error), a point under a reducer, and
    # a range whose window reads a keyed decision at a row with no instance of it, which is no member (D4).
    "frames": (4, 6, 15, 37),
    # A MIN over groups, two frame values under `=` (an expected error), a MAX plus a SUM.
    "bounds": (2, 7, 10),
    # A UNION ALL and a default collation, a subquery with NULL keys first, a join with NULL keys.
    "order_keys": (9, 23, 30),
}
#: How many seeds a broken oracle may take to find a mismatch.
BROKEN_SEEDS = 25
PASSING = {"ok", "refused"}


def stand_in(tmp_path, monkeypatch, body: str) -> None:
    """A CLI that reads its script and does ``body``, in place of the real one."""
    exe = tmp_path / "decidb"
    exe.write_text(f"#!/bin/sh\ncat > /dev/null\n{body}\n")
    exe.chmod(0o755)
    monkeypatch.setenv("DECIDB_EXE_PATH", str(exe))


@pytest.mark.parametrize("name", runner.MODULES)
def test_module_agrees_with_its_oracle(name):
    """Every spelling of every seed, under each direct-solve mode, answers as the module's oracle reads it."""
    report = runner.run(name, SEEDS[name])
    assert not report.failures, "\n\n".join(map(str, report.failures))
    assert report.counts["ok"] >= len(SEEDS[name])


def finds_wrong(module, model, seed: int, outcomes: dict) -> bool:
    """Whether ``model`` judges the CLI wrong on a spelling of the seed's case, under direct solve off. A broken oracle
    mostly asks the very queries the true one does, so ``outcomes`` runs each script once."""
    case = module.generate(seed, model)
    expected = case.reference.expected()
    for script in case.scripts.values():
        if script not in outcomes:
            outcomes[script] = runner.execute(runner.session("off", script), None)
    return any(judge(case, expected, outcomes[script])[0] not in PASSING for script in case.scripts.values())


@pytest.mark.parametrize("name", runner.MODULES)
def test_each_broken_oracle_reports_a_mismatch(name):
    """Every broken oracle of the module finds the CLI wrong within a few seeds: a fuzzer whose oracle cannot fail
    proves nothing."""
    module = runner.load(name)
    outcomes: dict = {}
    for broken, model in module.BROKEN.items():
        seeds = range(1, BROKEN_SEEDS + 1)
        found = any(finds_wrong(module, model, seed, outcomes) for seed in seeds)
        assert found, f"{name}: {broken} finds nothing wrong in seeds {seeds.start}..{seeds.stop - 1}"


@pytest.mark.parametrize(
    "key, form",
    [
        pytest.param(nests.Counted(("g",)), "y[a] + y[b] <= 4", id="the-counting-clause"),
        pytest.param(("g",), "2 * y[a] + y[b] <= 4", id="a-nest"),
    ],
)
def test_ignores_nest_per_reads_only_a_nest_wrong(key, form):
    """``ignores-nest-per`` counts ``sum(per t.g: y)`` once per class in the counting clause, as the language does,
    and at every row anywhere else: class a holds ``y[a]`` in two rows. So what it finds wrong is a nest's ``per``."""
    rows = [dict(id=1, g="a"), dict(id=2, g="a"), dict(id=3, g="b")]
    model = nests.BROKEN["ignores-nest-per"](rows, [Decision("y", ("g",), VarType.INTEGER, 0.0, 3.0)])
    instances = model.generate([clause(le(reduce("sum", Var("y"), per=key), 4))])
    assert {model.render(instance) for instance in instances} == {form}


def test_the_pinned_nest_counts():
    """The pinned kind's nest is a SUM under a ``per`` with classes, which counts a keyed decision once per class where
    every row would count it again, so a ``per`` read wrong there moves an ``s``."""
    for seed in range(40):
        nest = nests.over_pinned(random.Random(seed), AnrModel).reference.clauses[-1].body.rhs
        assert (nest.kind, nest.operand.per is PER_ROW) == ("sum", False), seed


def test_a_smoke_seed_reads_a_nest_per():
    """``ignores-nest-per`` finds the CLI wrong on a smoke seed, the pinned one (5): the smoke seeds catch a ``per``
    read wrong inside a nest, not only in a counting clause."""
    assert any(finds_wrong(nests, nests.BROKEN["ignores-nest-per"], seed, {}) for seed in SEEDS["nests"])


def one_decision() -> Reference:
    """``x`` in 0..1, maximized: the optimum is 1."""
    model = AnrModel([{"id": 1}], [Decision("x", var_type=VarType.INTEGER, ub=1.0)])
    return Reference(model, [], Objective(ObjSense.MAXIMIZE, reduce("sum", Var("x"))), {"x": range(2)})


OPTIMAL = Expected("optimal", objective=1.0)


@pytest.mark.parametrize(
    "expected, outcome, category",
    [
        pytest.param(OPTIMAL, Outcome("internal", text="INTERNAL Error"), "internal", id="a-failed-run"),
        pytest.param(
            Expected("error", phrase="must hold one row"),
            Outcome("error", text="... must hold one row."),
            "ok",
            id="expected-error",
        ),
        pytest.param(
            OPTIMAL, Outcome("error", text="Binder Error: it can take fractional values"), "refused", id="refused"
        ),
        pytest.param(OPTIMAL, Outcome("error", text="Binder Error: anything else"), "false-error", id="false-error"),
        pytest.param(Expected("infeasible"), Outcome("infeasible", text="is infeasible"), "ok", id="infeasible"),
        pytest.param(OPTIMAL, Outcome("infeasible", text="is infeasible"), "false-infeasible", id="false-infeasible"),
        pytest.param(
            Expected("infeasible"), Outcome("answer", rows=[{"id": 1, "x": 0}]), "false-answer", id="false-answer"
        ),
        pytest.param(OPTIMAL, Outcome("answer", rows=[{"id": 1, "x": 1}]), "ok", id="optimal"),
        pytest.param(OPTIMAL, Outcome("answer", rows=[{"id": 1, "x": 0}]), "wrong-answer", id="wrong-answer"),
    ],
)
def test_judge(expected, outcome, category):
    case = Case(one_decision(), {}, refusals=("can take fractional values",))
    assert judge(case, expected, outcome)[0] == category


def test_fault_names_the_direction():
    reference = one_decision()
    assert "worse" in reference.fault_in([{"id": 1, "x": 0}], OPTIMAL)
    assert "better" in reference.fault_in([{"id": 1, "x": 1}], Expected("optimal", objective=0.0))
    assert "breaks the language" in reference.fault_in([{"id": 1, "x": 2}], OPTIMAL)


def test_reference_reads_each_status():
    reference = one_decision()
    assert reference.expected() == OPTIMAL
    reference.clauses = [clause(ge(Var("x"), 2))]
    assert reference.expected().status == "infeasible"


def test_a_range_reaching_a_null_fallback_reads_none():
    """D86 in a range's signature: rows 1 and 3 reach their `else t.o`, which is NULL there, so it is no fallback
    and no field, and each sums the positions that exist: 3 + 7, 3 + 7 + 2, 7 + 2."""
    rows = [dict(id=1, s=1, v=3, o=None), dict(id=2, s=2, v=7, o=5), dict(id=3, s=3, v=2, o=None)]
    window = Range("sum", previous(1), next_(1), Col("v"), Over("s"), fallback=Col("o"))
    model = AnrModel(rows, [Decision("y", var_type=VarType.CONTINUOUS)])
    assert [model.render(instance) for instance in model.generate([clause(eq(Var("y"), window))])] == [
        "y1 = 10",
        "y2 = 12",
        "y3 = 9",
    ]


@pytest.mark.parametrize(
    "sense, lhs, phrases",
    [
        pytest.param("<=", Var("x") + reduce("avg", Var("x")), (), id="an-avg-under-<="),
        pytest.param("<>", reduce("sum", Var("x") - reduce("avg", Col("v"))), nests.FRACTIONAL, id="avg-over-data"),
        pytest.param("<>", reduce("avg", Var("x")), (), id="avg-over-x"),
        pytest.param("<>", reduce("avg", 2 * Var("x")), (), id="avg-over-twice-x"),
        pytest.param("<>", Var("x") + reduce("avg", Var("x")), nests.FRACTIONAL, id="avg-beside-a-term"),
        pytest.param("<>", reduce("avg", Var("x"), when=lambda row: row["p"]), nests.FRACTIONAL, id="avg-own-when"),
        pytest.param("<>", reduce("avg", Var("x"), per=("g",)), nests.FRACTIONAL, id="avg-own-per"),
        pytest.param("<>", reduce("avg", Col("v") * Var("x")), nests.FRACTIONAL, id="avg-over-a-column-times-x"),
        pytest.param("<>", reduce("avg", Var("y")), nests.FRACTIONAL, id="avg-over-a-keyed-decision"),
        pytest.param("<>", reduce("min", Var("x")), nests.EXTREMUM, id="min-over-x"),
        pytest.param("<>", reduce("sum", Var("x")), (), id="sum-over-x"),
        pytest.param("<>", Col("w") * reduce("max", Var("x")), (), id="a-max-times-a-column"),
        pytest.param("<>", reduce("max", Var("x") - reduce("max", Col("v"))), (), id="max-over-a-reducer"),
    ],
)
def test_nests_refusals_follow_the_shape(sense, lhs, phrases):
    """Only under ``<>``. ``AVG(x) <> K`` is ``SUM(x) <> K·n`` (guide 3), so an AVG that is the whole side, with no
    ``when`` or ``per`` of its own, over ``x`` or a constant times it, is answered; any other AVG on the decisions'
    side is refused, as errors.md says beside a term or with its own ``when`` or ``per``. A MIN or MAX that is the
    whole side, over a leaf, is refused (``RewriteMinMax``); one times a column or over a reducer is answered, and
    rightly, though errors.md lists '<> on a MIN/MAX, alone or composed' as refused."""
    assert nests.refusals(sense, lhs) == phrases


@pytest.mark.parametrize(
    "seed, category",
    [pytest.param(3, "false-error", id="an-avg-under->="), pytest.param(7, "refused", id="an-avg-under-<>")],
)
def test_a_refusal_passes_only_where_its_shape_is_refused(tmp_path, monkeypatch, seed, category):
    """A CLI that refuses every query as fractional is wrong on a nest that holds an AVG compared by ``>=`` and right
    on one compared by ``<>``."""
    stand_in(tmp_path, monkeypatch, "echo \"Binder Error: 'avg(x)' can take fractional values\" >&2; exit 1")
    assert set(runner.check_seed("nests", None, {}, seed).counts) == {category}


def test_bounds_refuse_two_values_under_equals():
    """``sum(x) = t.c`` over one group bound by 2 and 3 is D85's error; a MAX's says infeasible (finding 12)."""
    rows = [dict(id=1, c=2.0), dict(id=2, c=3.0)]
    model = bounds.BoundModel(rows, [Decision("x", var_type=VarType.CONTINUOUS)])
    with pytest.raises(bounds.TwoValues):
        model.generate([clause(eq(reduce("sum", Var("x")), Col("c")))])
    assert len(model.generate([clause(eq(reduce("max", Var("x")), Col("c")))])) == 2


@pytest.mark.parametrize(
    "ks, orphan",
    [pytest.param((1, None), False, id="a-class-in-the-group"), pytest.param((None, None), True, id="no-class")],
)
def test_bounds_find_a_group_with_no_class(ks, orphan):
    rows = [dict(g="a", k=k) for k in ks] + [dict(g="b", k=1)]
    assert bounds.holds_orphan_group(rows, ("g",), "k") is orphan


@pytest.mark.parametrize(
    "desc, keys", [pytest.param(False, [None, "a", "B"], id="asc"), pytest.param(True, ["B", "a", None], id="desc")]
)
def test_order_keys_place_null(desc, keys):
    """``nulls_first_on_asc_last_on_desc`` puts NULL first ascending and last descending, as DuckDB documents it, and
    NOCASE sorts ``a`` before ``B``."""
    key = order_keys.OrderModel.order("NOCASE", "nulls_first_on_asc_last_on_desc", desc)
    assert [row["k"] for row in sorted([dict(k="B"), dict(k=None), dict(k="a")], key=key, reverse=desc)] == keys


@pytest.mark.parametrize(
    "script, kind",
    [
        ("SELECT 1 AS id, 2 AS x;", "answer"),
        (
            "SELECT id, x FROM (VALUES (1)) t(id) DECIDE x(INT) SUCH THAT x >= 2 AND x <= 1 MAXIMIZE sum(x);",
            "infeasible",
        ),
        ("SELECT id, x FROM (VALUES (1)) t(id) DECIDE x(INT) SUCH THAT y <= 1 MAXIMIZE sum(x);", "error"),
    ],
)
def test_execute_reads_the_cli(script, kind):
    assert runner.execute(script, None).kind == kind


@pytest.mark.parametrize(
    "body, kind, timeout",
    [
        ("kill -SEGV $$", "crash", 60.0),
        ("echo 'INTERNAL Error: a stand-in' >&2", "internal", 60.0),
        # Only the hang is cut short: a new file's first run can take seconds while other workers start theirs.
        ("exec sleep 30", "timeout", 3.0),
    ],
)
def test_execute_reads_a_failing_cli(tmp_path, monkeypatch, body, kind, timeout):
    stand_in(tmp_path, monkeypatch, body)
    monkeypatch.setattr(runner, "_TIMEOUT", timeout)
    assert runner.execute("SELECT 1;", None).kind == kind


def test_an_oracle_that_fails_is_a_finding(monkeypatch):
    def fails(self):
        raise KeyError("a reading the oracle does not have")

    monkeypatch.setattr(Reference, "expected", fails)
    report = runner.check_seed("frames", None, {}, 1)
    assert [failure.category for failure in report.failures] == ["oracle-error"]
    assert report.failures[0].script.startswith("SET threads=1;")


#: Two seeds frames' ``one-further`` fails on, one after the other.
POOLED = (3, 4)


def test_a_failure_prints_a_script_the_cli_runs(monkeypatch, capsys):
    """``python -m fuzz`` exits 1 on a mismatch and prints the case as a script with the command that reruns it. Two
    jobs take the process pool, whose report follows the seeds. A HiGHS script names its solver, and run back through
    the CLI on HiGHS it answers."""
    first, second = POOLED
    arguments = f"frames --seed {first} --count 2 --jobs 2 --show 8 --broken one-further --solver highs"
    monkeypatch.setattr(sys, "argv", ["fuzz", *arguments.split()])
    assert entry.main() == 1
    printed = capsys.readouterr().out
    assert printed.startswith(f"frames: seeds {first}..{second}: ")
    seeds = [int(seed) for seed in re.findall(r"^-- \S+: python -m fuzz frames --seed (\d+) --count 1", printed, re.M)]
    assert set(seeds) == {first, second} and seeds == sorted(seeds), seeds
    assert f"python -m fuzz frames --seed {first} --count 1 --broken one-further --solver highs (" in printed
    script = re.search(r"^-- run with DECIDB_FORCE_SOLVER=highs\nSET threads=1;\n.*?(?=\n\n|\Z)", printed, re.S | re.M)
    assert runner.execute(script.group(0), dict(os.environ, DECIDB_FORCE_SOLVER="highs")).kind == "answer"


@pytest.mark.parametrize("category, status", [("internal", 0), ("wrong-answer", 1)])
def test_a_recorded_failure_fails_no_run(tmp_path, monkeypatch, capsys, category, status):
    """A failure ``known.py`` records, alike in module, seed, solver and category, is counted under its defect and fails
    no run: a run with no ``--solver`` on a CLI that has Gurobi meets the Gurobi record. One of another category is
    new. D9 records the failure under another module, another solver and another seed, ahead of D0, and is never met.
    The stand-in answers only the probe for Gurobi."""
    decoy = {
        ("nests", "gurobi", "internal"): (1,),
        ("frames", "highs", "internal"): (1,),
        ("frames", "gurobi", "internal"): (2,),
    }
    record = {("frames", "gurobi", category): (1,)}
    monkeypatch.setattr(known, "DEFECTS", {"D9": ("a decoy", decoy), "D0": ("a stand-in", record)})
    monkeypatch.delenv("DECIDB_FORCE_SOLVER", raising=False)
    stand_in(tmp_path, monkeypatch, "[ -n \"$DECIDB_FORCE_SOLVER\" ] || echo 'INTERNAL Error: a stand-in' >&2")
    monkeypatch.setattr(sys, "argv", "fuzz frames --seed 1 --count 1 --jobs 1".split())
    assert entry.main() == status
    printed = capsys.readouterr().out
    assert ("known D0: seeds 1" in printed) is (status == 0)
    assert "known D9" not in printed


def test_a_broken_oracle_meets_no_record(tmp_path, monkeypatch, capsys):
    """A broken oracle's mismatches are what the run looks for, so a record of the same failure does not excuse one."""
    monkeypatch.setattr(known, "DEFECTS", {"D0": ("a stand-in", {("frames", "gurobi", "internal"): (1,)})})
    monkeypatch.delenv("DECIDB_FORCE_SOLVER", raising=False)
    stand_in(tmp_path, monkeypatch, "[ -n \"$DECIDB_FORCE_SOLVER\" ] || echo 'INTERNAL Error: a stand-in' >&2")
    monkeypatch.setattr(sys, "argv", "fuzz frames --seed 1 --count 1 --jobs 1 --broken one-further".split())
    assert entry.main() == 1
    assert "known D0" not in capsys.readouterr().out


@pytest.mark.parametrize(
    "forced, body, solver",
    [
        pytest.param("highs", "", "highs", id="forced"),
        pytest.param(None, "", "gurobi", id="the-cli-has-gurobi"),
        pytest.param(None, "echo 'Invalid Input Error: gurobi is not available on this host' >&2", "highs", id="not"),
    ],
)
def test_a_run_names_the_solver_it_solves_with(tmp_path, monkeypatch, forced, body, solver):
    """``known.py`` records a failure under the solver that ran, so ``--solver gurobi`` and the CLI's own choice of
    Gurobi meet the same record, and a host without Gurobi meets the HiGHS ones."""
    stand_in(tmp_path, monkeypatch, body)
    assert runner.solver_of({"DECIDB_FORCE_SOLVER": forced} if forced else {}) == solver


def test_an_unknown_broken_oracle_is_refused(monkeypatch):
    monkeypatch.setattr(sys, "argv", ["fuzz", "nests", "--broken", "no-such-oracle"])
    with pytest.raises(SystemExit, match="nests has no broken oracle 'no-such-oracle'"):
        entry.main()
