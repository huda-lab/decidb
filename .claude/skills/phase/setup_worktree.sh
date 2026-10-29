#!/bin/bash
# Prepare a lane's git worktree to build and test DeciDB. Run from inside the worktree.
#
# A worktree shares the repository but not its untracked files: the test databases, the oracle cache and the Python
# environment live only in the main tree. This clones them in (APFS clones: no extra disk), links the Python
# environment (third-party packages only), and builds with a capped job count so several lanes can build at once.
set -euo pipefail

WT="$(git rev-parse --show-toplevel)"
MAIN="$(cd "$(git rev-parse --git-common-dir)/.." && pwd)"
if [ "$WT" = "$MAIN" ]; then
	echo "setup_worktree.sh: run this inside a lane worktree, not the main tree ($MAIN)" >&2
	exit 1
fi

for f in test/decide/decide.db test/decide/_tpch_oracle.duckdb test/decide/results/oracle_cache.json; do
	if [ -f "$MAIN/$f" ] && [ ! -e "$WT/$f" ]; then
		mkdir -p "$(dirname "$WT/$f")"
		cp -c "$MAIN/$f" "$WT/$f" 2>/dev/null || cp "$MAIN/$f" "$WT/$f"
	fi
done
if [ ! -e "$WT/test/decide/.venv" ]; then
	ln -s "$MAIN/test/decide/.venv" "$WT/test/decide/.venv"
fi

# ccache keys on paths relative to this base, so every lane reuses the others' compiled objects.
export CCACHE_BASEDIR="$(dirname "$MAIN")"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
cd "$WT"
make release 2>&1 | tail -2
echo "worktree ready: $WT (main tree $MAIN)"
