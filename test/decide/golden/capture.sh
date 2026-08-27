#!/usr/bin/env bash
# Capture the solver-model dump for every query in corpus.sql, for ONE backend.
#
# The dump is a characterization oracle: a change that claims not to alter the
# built model is verified by diffing dumps, not by comparing optimal values
# (two different models routinely share an optimum).
#
# The backend is part of the answer, not an ambient fact about the host. Gurobi
# states ABS/MIN/MAX/`<>` natively and HiGHS lowers them, so the two build
# genuinely different models from the same SQL and each needs its own baseline.
# `--solver` pins DECIDB_FORCE_SOLVER so a captured file always means one
# specific backend. DECIDB_NATIVE_CONSTRUCTS is pinned to `on` for the same
# reason: it is the A/B switch between stating a construct natively and lowering
# it, so leaving it ambient would let `DECIDB_NATIVE_CONSTRUCTS=force` in the
# caller's environment capture a different configuration and diff it against the
# shipped baseline. A baseline records the model DeciDB ships, never an arm.
#
#   ./capture.sh --solver gurobi          # write baseline.gurobi.dump
#   ./capture.sh --solver highs           # write baseline.highs.dump
#   ./capture.sh --solver gurobi out.dump # write somewhere else
#
# Regenerating a baseline is how an intentional model change is accepted: run
# this for each solver, read the diff, and commit it with the change that caused
# it. `check.sh` is the read-only half and is what `make decide-test` runs.
#
# Query results go to <output>.results so a change in query OUTPUT is visible
# separately from a change in the MODEL.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
decidb="$repo/build/release/decidb"

solver=""
out=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --solver) solver="${2:-}"; shift 2 ;;
        --solver=*) solver="${1#*=}"; shift ;;
        -h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) out="$1"; shift ;;
    esac
done

if [[ -z "$solver" ]]; then
    echo "usage: capture.sh --solver <gurobi|highs> [output]" >&2
    echo "  the backend decides the model, so a dump without one means nothing" >&2
    exit 2
fi

# Validate the name here rather than trusting DECIDB_FORCE_SOLVER to complain.
# It does not: a name no backend answers to falls through to auto-selection by
# design (`SelectSolverBackend`, ilp_solver.cpp). Silent fallback is fine for an
# ad-hoc run and fatal for a baseline, which would then record the host default
# under a name that promises otherwise. Keep in step with solver_registry.cpp.
case "$solver" in
    gurobi|highs) ;;
    *) echo "unknown solver '$solver' -- expected one of: gurobi, highs" >&2; exit 2 ;;
esac
out="${out:-$here/baseline.$solver.dump}"

if [[ ! -x "$decidb" ]]; then
    echo "decidb not built at $decidb -- run 'make release' first" >&2
    exit 1
fi

rm -f "$out" "$out.results"

# The dump appends, so one process over the whole corpus yields one file with
# the models in query order.
# A query that errors is still useful signal (the error text lands in .results),
# so a nonzero exit must not abort the capture.
DECIDB_DUMP_MODEL="$out" DECIDB_FORCE_SOLVER="$solver" \
    DECIDB_NATIVE_CONSTRUCTS=on \
    "$decidb" < "$here/corpus.sql" > "$out.results" 2>&1 || true

if grep -q "that solver is not available" "$out.results" 2>/dev/null; then
    echo "solver '$solver' is not available on this host" >&2
    rm -f "$out" "$out.results"
    exit 3
fi

if [[ ! -f "$out" ]]; then
    echo "no dump produced -- did every query fail to reach the solver?" >&2
    exit 1
fi

models=$(grep -c '^=== DECIDB MODEL DUMP ===' "$out" || true)
errors=$(grep -ci 'error' "$out.results" || true)
echo "wrote $out ($models models, $(wc -l < "$out" | tr -d ' ') lines)"
echo "wrote $out.results ($errors lines mentioning 'error')"
