#!/usr/bin/env bash
# Diff the model DeciDB builds today against the committed baselines.
#
# This is the read-only half of the golden harness and the part `make
# decide-test` runs. It fails on any difference, because the point of a
# characterization oracle is that a difference is news: two different models
# routinely share an optimum, so a change that alters the matrix while keeping
# every answer is invisible to the pytest suite and visible only here.
#
# A failure is not automatically a bug. It means the built model moved, and the
# move has to be read and accounted for. When it is intended:
#
#   ./capture.sh --solver gurobi
#   ./capture.sh --solver highs
#
# then commit the regenerated baselines alongside the change that caused them.
#
# A solver missing from the host is skipped, not failed -- which backends are
# installed is a fact about the machine. A baseline that has never been captured
# IS a failure: silently passing on a missing reference is how the last one went
# stale unnoticed.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

status=0
checked=0

for solver in gurobi highs; do
    baseline="$here/baseline.$solver.dump"

    set +e
    out="$("$here/capture.sh" --solver "$solver" "$tmp/$solver.dump" 2>&1)"
    rc=$?
    set -e

    if [[ $rc -eq 3 ]]; then
        echo "golden[$solver]: skipped, not available on this host"
        continue
    fi
    if [[ $rc -ne 0 ]]; then
        echo "golden[$solver]: CAPTURE FAILED"; echo "$out"; status=1; continue
    fi
    if [[ ! -f "$baseline" ]]; then
        echo "golden[$solver]: no baseline at $baseline"
        echo "  capture one with: ./test/decide/golden/capture.sh --solver $solver"
        status=1; continue
    fi

    checked=$((checked + 1))
    ok=1
    if ! diff -u "$baseline" "$tmp/$solver.dump" > "$tmp/$solver.modeldiff"; then
        echo "golden[$solver]: MODEL CHANGED ($(grep -c '^[+-][^+-]' "$tmp/$solver.modeldiff") lines)"
        head -60 "$tmp/$solver.modeldiff"
        ok=0
    fi
    # Results are diffed separately so a changed ANSWER is never mistaken for a
    # changed model, and vice versa.
    if ! diff -u "$baseline.results" "$tmp/$solver.dump.results" > "$tmp/$solver.resdiff"; then
        echo "golden[$solver]: QUERY RESULTS CHANGED ($(grep -c '^[+-][^+-]' "$tmp/$solver.resdiff") lines)"
        head -40 "$tmp/$solver.resdiff"
        ok=0
    fi
    if [[ $ok -eq 1 ]]; then
        echo "golden[$solver]: identical to baseline"
    else
        status=1
    fi
done

if [[ $checked -eq 0 && $status -eq 0 ]]; then
    echo "golden: no solver available to check against" >&2
    exit 1
fi
exit $status
