"""Differential fuzzers for the DECIDE evaluator: nests, frames, bounds and order keys.

Each module generates a query from a seed together with its reading in ``tests/_anr_oracle.py``. The runner runs the
query on the CLI and judges the answer against that reading, so a later rewrite of the evaluator is measured against
what the language defines and not against what the code did. From ``test/decide``::

    .venv/bin/python -m fuzz nests --seed 1 --count 200
    .venv/bin/python -m fuzz all --count 300 --solver highs
    .venv/bin/python -m fuzz frames --count 20 --broken cyclic-ignored

A seed always generates the same case. Each case runs in several spellings (a stored table, inline VALUES with the rows
in another order, the other shapes a module names) under both direct-solve modes, and a failure prints as a complete
script with the command that reruns it, to paste into ``build/release/decidb``. ``DECIDB_EXE_PATH`` picks another CLI,
as it does for the suite. ``--broken`` swaps in a deliberately wrong oracle, which must report mismatches: that is how
a fuzzer shows it can fail. A generator stays clear of the open defects the Q7 review lists, each where it draws, and a
failure ``known.py`` records is counted under its defect and fails no run; the fix of a defect takes its seeds out.

``make decide-test`` runs only ``tests/test_fuzz_smoke.py``: a few seeds of each module.
"""
