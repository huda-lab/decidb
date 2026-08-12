#!/usr/bin/env bash
# Capture the solver-model dump for every query in corpus.sql.
#
# The dump is a characterization oracle: a change that claims not to alter the
# built model is verified by diffing dumps, not by comparing optimal values
# (two different models routinely share an optimum).
#
#   ./capture.sh                 # write baseline.dump
#   ./capture.sh after.dump      # write somewhere else, then:
#   diff baseline.dump after.dump
#
# Query results go to <output>.results so a change in query OUTPUT is visible
# separately from a change in the MODEL.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
decidb="$repo/build/release/decidb"
out="${1:-$here/baseline.dump}"

if [[ ! -x "$decidb" ]]; then
    echo "decidb not built at $decidb -- run 'make release' first" >&2
    exit 1
fi

rm -f "$out" "$out.results"

# The dump appends, so one process over the whole corpus yields one file with
# the models in query order.
# A query that errors is still useful signal (the error text lands in .results),
# so a nonzero exit must not abort the capture.
DECIDB_DUMP_MODEL="$out" "$decidb" < "$here/corpus.sql" > "$out.results" 2>&1 || true

if [[ ! -f "$out" ]]; then
    echo "no dump produced -- did every query fail to reach the solver?" >&2
    exit 1
fi

models=$(grep -c '^=== DECIDB MODEL DUMP ===' "$out" || true)
errors=$(grep -ci 'error' "$out.results" || true)
echo "wrote $out ($models models, $(wc -l < "$out" | tr -d ' ') lines)"
echo "wrote $out.results ($errors lines mentioning 'error')"
