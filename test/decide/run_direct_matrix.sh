#!/usr/bin/env bash
# run_direct_matrix.sh -- run the five direct-solve checks and print one summary line each.
#
# Usage (from anywhere; run `make release` first so the binary is current):
#   test/decide/run_direct_matrix.sh
#
# The checks are the ones architecture/rules.md lists:
#   auto        make decide-test
#   off         DECIDB_TEST_DIRECT_SOLVE=off make decide-test
#   serializer  DECIDB_VERIFY_SERIALIZER=1 make decide-test
#   highs       DECIDB_FORCE_SOLVER=highs make decide-test
#   C++         build/release/test/unittest "[decidb]"
#
# Each pytest line reads `auto: 2322 passed, 0 xfailed, 0 failed (2322 outcomes)`; the outcome totals of the
# modes should agree. A golden-model difference or a run that prints no pytest summary shows as `FAILED TO RUN`
# with the log path. Full logs go to $MATRIX_LOG_DIR (default: a new test/decide/results/matrix_<time> folder,
# which git ignores); the last line prints the path. The exit code is 0 only when no pytest check has a failure and the
# C++ run passes.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LOG_DIR="${MATRIX_LOG_DIR:-${SCRIPT_DIR}/results/matrix_$(date +%Y%m%dT%H%M%S)}"
mkdir -p "${LOG_DIR}"
cd "${REPO_ROOT}"

status=0

# count LABEL WORD: the number before WORD on pytest's final summary line (0 when absent).
count() {
    local n
    n="$(tail -n 5 "$1" | grep -E '(passed|failed|xfailed)' | tail -n 1 | grep -oE "[0-9]+ $2" | head -n 1 | cut -d' ' -f1)"
    echo "${n:-0}"
}

run_pytest() {
    local label="$1"
    shift
    local log="${LOG_DIR}/${label}.log"
    env "$@" make decide-test >"${log}" 2>&1
    if ! tail -n 5 "${log}" | grep -qE '[0-9]+ (passed|failed)'; then
        echo "${label}: FAILED TO RUN (see ${log})"
        status=1
        return
    fi
    local passed xfailed failed skipped
    passed="$(count "${log}" passed)"
    xfailed="$(count "${log}" xfailed)"
    failed="$(( $(count "${log}" failed) + $(count "${log}" errors?) ))"
    skipped="$(count "${log}" skipped)"
    echo "${label}: ${passed} passed, ${xfailed} xfailed, ${failed} failed ($(( passed + xfailed + failed + skipped )) outcomes)"
    if [[ "${failed}" -ne 0 ]]; then
        status=1
    fi
}

run_pytest auto
run_pytest off DECIDB_TEST_DIRECT_SOLVE=off
run_pytest serializer DECIDB_VERIFY_SERIALIZER=1
run_pytest highs DECIDB_FORCE_SOLVER=highs

cpp_log="${LOG_DIR}/cpp.log"
build/release/test/unittest "[decidb]" >"${cpp_log}" 2>&1
cpp_rc=$?
cpp_line="$(grep -E 'assertions? in [0-9]+ test case' "${cpp_log}" | tail -n 1)"
if [[ -n "${cpp_line}" ]]; then
    echo "C++: $(echo "${cpp_line}" | grep -oE '[0-9]+ assertions? in [0-9]+ test cases?')$([[ ${cpp_rc} -eq 0 ]] || echo ' (FAILED)')"
else
    echo "C++: FAILED TO RUN (see ${cpp_log})"
fi
if [[ ${cpp_rc} -ne 0 ]]; then
    status=1
fi

echo "logs: ${LOG_DIR}"
exit ${status}
