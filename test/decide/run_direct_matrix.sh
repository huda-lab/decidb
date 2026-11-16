#!/usr/bin/env bash
# run_direct_matrix.sh -- run the five checks of the full gate and print one summary line each.
#
# Usage (from anywhere; run `make release` first so the binary is current):
#   test/decide/run_direct_matrix.sh
#
# The checks are the ones context/docs/direct_solve/framework/todo.md lists:
#   auto        make decide-test
#   off         DECIDB_TEST_DIRECT_SOLVE=off make decide-test
#   serializer  DECIDB_VERIFY_SERIALIZER=1 make decide-test
#   highs       DECIDB_FORCE_SOLVER=highs make decide-test
#   C++         build/release/test/unittest "[decidb]"
#
# Every check runs with the three mode switches cleared, and each pytest mode sets only its own, so a caller's
# DECIDB_FORCE_SOLVER=highs leaves `auto` and C++ on the default backend. Any other DECIDB_ variable stays on.
#
# A check passes when its command exits 0; the log is not read for the verdict. Each line names the exit status and
# then pytest's counts, `auto: exit 0; 2322 passed, 0 xfailed, 0 failed (2322 outcomes)`, or the C++ summary; the
# outcome totals of the modes should agree. A check that exits nonzero also names its log. Full logs go to
# $MATRIX_LOG_DIR (default: a new test/decide/results/matrix_<time> folder, which git ignores). The last two lines name
# the failed checks (or say none failed) and the log folder. The exit code is 0 only when every check exits 0.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LOG_DIR="${MATRIX_LOG_DIR:-${SCRIPT_DIR}/results/matrix_$(date +%Y%m%dT%H%M%S)}"
mkdir -p "${LOG_DIR}"
cd "${REPO_ROOT}"
unset DECIDB_TEST_DIRECT_SOLVE DECIDB_VERIFY_SERIALIZER DECIDB_FORCE_SOLVER

failed_checks=""

# report LABEL RC LOG DETAIL: the check's line; a nonzero RC fails the check.
report() {
    local label="$1" rc="$2" log="$3" detail="$4"
    if [[ "${rc}" -eq 0 ]]; then
        echo "${label}: exit 0; ${detail}"
    else
        echo "${label}: exit ${rc}, FAILED (see ${log}); ${detail}"
        failed_checks="${failed_checks} ${label}"
    fi
}

# count LOG WORD: the number before WORD on pytest's final summary line (0 when absent).
count() {
    local n
    n="$(tail -n 5 "$1" | grep -E '(passed|failed|xfailed)' | tail -n 1 | grep -oE "[0-9]+ $2" | head -n 1 | cut -d' ' -f1)"
    echo "${n:-0}"
}

# pytest_counts LOG: the outcome counts of pytest's final summary line.
pytest_counts() {
    local log="$1" passed xfailed failed skipped
    if ! tail -n 5 "${log}" | grep -qE '[0-9]+ (passed|failed)'; then
        echo "no pytest summary"
        return
    fi
    passed="$(count "${log}" passed)"
    xfailed="$(count "${log}" xfailed)"
    failed="$(( $(count "${log}" failed) + $(count "${log}" errors?) ))"
    skipped="$(count "${log}" skipped)"
    echo "${passed} passed, ${xfailed} xfailed, ${failed} failed ($(( passed + xfailed + failed + skipped )) outcomes)"
}

# run_mode LABEL [SWITCH]: `make decide-test` with SWITCH, the mode's one switch, set.
run_mode() {
    local label="$1" switch="${2:-}" log="${LOG_DIR}/$1.log" rc=0
    env ${switch:+"${switch}"} make decide-test >"${log}" 2>&1 || rc=$?
    report "${label}" "${rc}" "${log}" "$(pytest_counts "${log}")"
}

run_mode auto
run_mode off DECIDB_TEST_DIRECT_SOLVE=off
run_mode serializer DECIDB_VERIFY_SERIALIZER=1
run_mode highs DECIDB_FORCE_SOLVER=highs

cpp_log="${LOG_DIR}/cpp.log"
cpp_rc=0
build/release/test/unittest "[decidb]" >"${cpp_log}" 2>&1 || cpp_rc=$?
cpp_summary="$(grep -oE '[0-9]+ assertions? in [0-9]+ test cases?' "${cpp_log}" | tail -n 1)"
report C++ "${cpp_rc}" "${cpp_log}" "${cpp_summary:-no Catch summary}"

if [[ -n "${failed_checks}" ]]; then
    echo "failed:${failed_checks}"
else
    echo "failed: none"
fi
echo "logs: ${LOG_DIR}"
[[ -z "${failed_checks}" ]]
