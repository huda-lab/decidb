"""Stored expectations: a Golden Master for the tests whose oracle was flat's run of the same query.

The tree evaluator's tests compared each query's model, stdout and stderr with what flat gave for it. Each of those is
stored here, under ``expected/``, keyed by what it ran: the kind of output, the SQL and the environment the test gave
it, with a value per solver, or one for every solver where they agree. A test compares the tree's output with the
stored one. ``DECIDB_RECORD_EXPECTED=1`` records the reference run instead, as ``golden/capture.sh`` does for the model
goldens: read the diff before committing it. Record with one worker (``DECIDE_TEST_JOBS=0``), under Gurobi and then
under HiGHS (``DECIDB_FORCE_SOLVER=highs``), the two solvers the suite runs on."""

import hashlib
import json
import os
from pathlib import Path

_STORE = Path(__file__).resolve().parent / "expected"
_SOLVERS = ("gurobi", "highs")
#: A value every solver gives.
_ANY = "any"

RECORDING = os.environ.get("DECIDB_RECORD_EXPECTED") == "1"


def expected(kind, sql, env, solver, produce):
    """The stored `kind` output of `sql` under `env` and `solver`, or, when recording, `produce()` stored."""
    env = dict(sorted(env.items()))
    key = json.dumps([kind, sql, env])
    path = _STORE / f"{kind}-{hashlib.sha1(key.encode()).hexdigest()[:16]}.json"
    record = json.loads(path.read_text()) if path.exists() else None
    if RECORDING:
        value = produce()
        values = dict(record["values"]) if record else {}
        if _ANY in values:
            values = {each: values[_ANY] for each in _SOLVERS}
        values[solver] = value
        if set(values) == set(_SOLVERS) and len({json.dumps(v) for v in values.values()}) == 1:
            values = {_ANY: value}
        _STORE.mkdir(exist_ok=True)
        record = {"kind": kind, "sql": sql, "env": env, "values": values}
        path.write_text(json.dumps(record, indent=1, ensure_ascii=False) + "\n")
        return value
    values = record["values"] if record else {}
    assert solver in values or _ANY in values, f"no stored {kind} for {sql!r} under {env} on {solver} (_expected.py)"
    return values.get(solver, values.get(_ANY))
