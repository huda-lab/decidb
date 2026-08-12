#!/usr/bin/env bash
# run_tests.sh — Run DECIDE pytest suite inside a managed virtualenv.
#
# Usage:
#   ./run_tests.sh                    # Run all tests
#   ./run_tests.sh -m var_boolean     # Run only boolean variable tests
#   ./run_tests.sh -k test_q01        # Run tests matching pattern
#   ./run_tests.sh --setup-only       # Just create/update the venv
#
# Tests run in parallel across 8 workers by default; DECIDE_TEST_JOBS=0 forces
# a serial run (useful when debugging a single test's output).
#
# The virtualenv is created at test/decide/.venv/ on first run.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${SCRIPT_DIR}/.venv"
REQ_FILE="${SCRIPT_DIR}/requirements.txt"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ── Create or reuse virtualenv ───────────────────────────────────────────
# Always drive pip as `python3 -m pip`, never `.venv/bin/pip`: the console
# script bakes an absolute shebang at creation time, so renaming or moving the
# repo leaves it pointing at a path that no longer exists.  That failure used
# to be swallowed here, and dependency refreshes silently stopped happening.
PIP=("${VENV_DIR}/bin/python3" -m pip)
PIP_ARGS=(-q --trusted-host pypi.org --trusted-host files.pythonhosted.org)

if [ -d "${VENV_DIR}" ] && ! "${VENV_DIR}/bin/python3" -m pip --version >/dev/null 2>&1; then
    echo "Virtualenv at ${VENV_DIR} is unusable (moved repo?) — rebuilding ..."
    rm -rf "${VENV_DIR}"
fi

if [ ! -d "${VENV_DIR}" ]; then
    echo "Creating virtualenv at ${VENV_DIR} ..."
    python3 -m venv "${VENV_DIR}"
    echo "Installing dependencies ..."
    "${PIP[@]}" install --upgrade pip "${PIP_ARGS[@]}"
    "${PIP[@]}" install -r "${REQ_FILE}" "${PIP_ARGS[@]}"
    echo "Virtualenv ready."
else
    # Ensure deps are up to date (fast no-op if already satisfied).  A failure
    # here is a warning, not a hard stop, but it must be visible.
    if ! "${PIP[@]}" install -r "${REQ_FILE}" "${PIP_ARGS[@]}"; then
        echo "WARNING: dependency refresh failed; running against the venv as-is." >&2
        echo "         Delete ${VENV_DIR} and re-run to rebuild it." >&2
    fi
fi

# ── Verify Gurobi oracle is usable ──────────────────────────────────────
# The oracle is Gurobi-only; catch missing install or bad license up front
# instead of surfacing it as a cryptic mid-test failure.
if ! "${VENV_DIR}/bin/python3" -c "import gurobipy; e = gurobipy.Env(empty=True); e.setParam('OutputFlag', 0); e.start()" 2>/dev/null; then
    echo "ERROR: gurobipy cannot start an environment." >&2
    echo "       Check that a valid Gurobi license is installed (grbprobe)," >&2
    echo "       or that GRB_LICENSE_FILE points to one." >&2
    exit 1
fi

# ── Handle --setup-only ──────────────────────────────────────────────────
if [[ "${1:-}" == "--setup-only" ]]; then
    echo "Setup complete. Virtualenv at: ${VENV_DIR}"
    exit 0
fi

# ── Run pytest ───────────────────────────────────────────────────────────
# The suite parallelizes almost perfectly.  Its cost is dominated by tests that
# sit on a wall-clock solver time limit rather than by CPU work — one file,
# test_query_diagnostics_slow.py, is ~46% of a serial run — so the wall time
# collapses across workers: ~132s serial vs ~25s at -n 8 on an 11-core host.
#
# Override with DECIDE_TEST_JOBS (0 runs everything in-process, i.e. serial).
# An explicit -n from the caller always wins.
JOBS="${DECIDE_TEST_JOBS:-8}"
PARALLEL=(-n "${JOBS}")
for arg in "$@"; do
    case "${arg}" in
        -n|-n*|--numprocesses|--numprocesses=*) PARALLEL=() ;;
    esac
done

cd "${SCRIPT_DIR}"
exec "${VENV_DIR}/bin/python3" -m pytest tests/ -v \
    ${PARALLEL[@]+"${PARALLEL[@]}"} "$@"
