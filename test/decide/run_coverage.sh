#!/usr/bin/env bash
# run_coverage.sh — Run DECIDE tests against the instrumented build and keep one branch-coverage
# profile per test (H-30).
#
# Usage:
#   ./run_coverage.sh OUT_DIR [pytest args...]      # e.g. OUT_DIR=build/coverage/run1
#   DECIDE_TEST_JOBS=0 ./run_coverage.sh OUT_DIR ... # serial run (cleanest per-test times)
#   ./coverage_report.py report OUT_DIR              # then read the report
#
# One-time build of the instrumented binary, from the repository root (build/release is untouched):
#   mkdir -p build/coverage && cd build/coverage
#   printf '[clang]\nsrc:*/src/optimizer/decide/direct/*\nsrc:*/src/optimizer/decide/decide_rewrite_rhs_window.cpp\n' > profile_list.txt
#   F="-fprofile-instr-generate -fcoverage-mapping -fprofile-list=$PWD/profile_list.txt"
#   cmake -DENABLE_EXTENSION_AUTOLOADING= -DENABLE_EXTENSION_AUTOINSTALL= -DLOCAL_EXTENSION_REPO="" \
#         -DOVERRIDE_GIT_DESCRIBE="" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="$F" -DCMAKE_C_FLAGS="$F" \
#         -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate" ../..
#   cmake --build . --target shell -- -j8
# (The cmake variables are the ones `make release` passes.) Do not run build/coverage/decidb by hand
# without LLVM_PROFILE_FILE set: it drops a default.profraw in the current directory.

# This wraps run_tests.sh, so the test selection arguments are the usual pytest ones.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
EXE="${REPO_ROOT}/build/coverage/decidb"

if [ "$#" -lt 1 ]; then
    echo "usage: $0 OUT_DIR [pytest args...]" >&2
    exit 2
fi
mkdir -p "$1"
OUT_DIR="$(cd "$1" && pwd)"
shift

if [ ! -x "${EXE}" ]; then
    echo "ERROR: ${EXE} not found; build the instrumented binary first." >&2
    exit 1
fi

export DECIDB_EXE_PATH="${EXE}"
export DECIDB_COVERAGE_DIR="${OUT_DIR}"
export PYTHONPATH="${SCRIPT_DIR}${PYTHONPATH:+:${PYTHONPATH}}"

# `-m` is not a test filter here. conftest.py prunes the oracle cache after a run that has no
# -k/-m, on the grounds that it saw every test; a run of a subset (node ids) would delete the
# cache entries of every other test, and a miss needs a Gurobi licence. A marker expression that
# matches everything keeps that pruning off. No test carries this marker.
exec "${SCRIPT_DIR}/run_tests.sh" -p coverage_plugin -m "not h30_no_such_marker" "$@"
