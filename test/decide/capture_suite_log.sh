#!/usr/bin/env bash
# capture_suite_log.sh -- store what every query of the suite prints, in each mode, and compare two stored logs.
#
# A later change to the evaluator is measured against a log taken before it: the same queries must print the same
# stdout and stderr. Usage (from anywhere; run `make release` first so the binary is current):
#
#   test/decide/capture_suite_log.sh capture [--modes auto,off] [--out DIR] [pytest arguments]
#   test/decide/capture_suite_log.sh compare OLD NEW [--limit N]
#
# capture runs the suite once per mode, the four pytest modes of run_direct_matrix.sh:
#   auto        no switch
#   off         DECIDB_TEST_DIRECT_SOLVE=off
#   serializer  DECIDB_VERIFY_SERIALIZER=1
#   highs       DECIDB_FORCE_SOLVER=highs
# into test/decide/results/suite_log_<short hash>/ (or --out, for a second log of one commit). Each mode is a folder:
# gw<N>.jsonl, one line per query from each xdist worker (`_log_query` in decidb_cli.py); junit.xml, each test's
# outcome; pytest.log; manifest.txt, which commit and binary it was. A mode captured again replaces that mode alone.
# The folder holds a .gitignore of its own: test/decide/.gitignore names only results/matrix_*/. Arguments after the
# options go to pytest, so `-k pattern` captures a part of the suite. Any other DECIDB_ variable in the caller's
# environment stays on.
# The exit code is 0 only when every mode ran and passed.
#
# compare is suite_log.py, whose docstring holds its rules: it pairs each query by its SQL and the environment the
# wrapper gave it, prints every query that differs, and exits 0 with none, 1 with some (a query or a mode only one log
# holds counts), 2 when a log holds no mode.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# mode_switch MODE: the assignment that makes a run that mode.
mode_switch() {
    case "$1" in
        auto) echo "" ;;
        off) echo "DECIDB_TEST_DIRECT_SOLVE=off" ;;
        serializer) echo "DECIDB_VERIFY_SERIALIZER=1" ;;
        highs) echo "DECIDB_FORCE_SOLVER=highs" ;;
        *) echo "unknown mode '$1' -- expected auto, off, serializer or highs" >&2; return 2 ;;
    esac
}

write_manifest() {
    local folder="$1" binary="${DECIDB_EXE_PATH:-${REPO_ROOT}/build/release/decidb}"
    shift
    {
        echo "commit $(git -C "${REPO_ROOT}" rev-parse --short HEAD)"
        echo "uncommitted source files $(git -C "${REPO_ROOT}" status --porcelain -- src | wc -l | tr -d ' ')"
        echo "binary ${binary} cksum $(cksum < "${binary}" | cut -d' ' -f1)"
        echo "taken $(date +%Y-%m-%dT%H:%M:%S)"
        echo "pytest arguments: $*"
        env | grep '^DECIDB_' | grep -v '^DECIDB_SUITE_LOG=' | sed 's/^/caller: /'
    } > "${folder}/manifest.txt"
}

# capture_mode OUT MODE PYTEST_ARGUMENT...: one run; prints one summary line; returns nonzero unless it ran and passed.
capture_mode() {
    local out="$1" mode="$2" switch folder rc=0
    shift 2
    switch="$(mode_switch "${mode}")" || return 2
    folder="${out}/${mode}"
    rm -rf "${folder}"
    mkdir -p "${folder}"
    write_manifest "${folder}" ${@+"$@"}
    # The caller's switches are dropped: a mode's name has to say what ran.
    env -u DECIDB_TEST_DIRECT_SOLVE -u DECIDB_VERIFY_SERIALIZER -u DECIDB_FORCE_SOLVER \
        ${switch:+"${switch}"} DECIDB_SUITE_LOG="${folder}" \
        "${SCRIPT_DIR}/run_tests.sh" --junitxml="${folder}/junit.xml" ${@+"$@"} > "${folder}/pytest.log" 2>&1 || rc=$?
    local queries summary
    queries="$(cat "${folder}"/*.jsonl 2>/dev/null | wc -l | tr -d ' ')"
    summary="$(grep -E '[0-9]+ (passed|failed|error)' "${folder}/pytest.log" | tail -n 1 | tr -d '=')"
    echo "${mode}: ${queries} queries;${summary}"
    if [[ "${queries}" -eq 0 ]]; then
        echo "${mode}: FAILED TO RUN (see ${folder}/pytest.log)"
        return 1
    fi
    return "${rc}"
}

capture() {
    local modes="auto,off,serializer,highs" out="" status=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --modes) modes="$2"; shift 2 ;;
            --out) out="$2"; shift 2 ;;
            *) break ;;
        esac
    done
    out="${out:-${SCRIPT_DIR}/results/suite_log_$(git -C "${REPO_ROOT}" rev-parse --short HEAD)}"
    mkdir -p "${out}"
    out="$(cd "${out}" && pwd)"  # pytest runs from test/decide
    [[ -e "${out}/.gitignore" ]] || echo '*' > "${out}/.gitignore"  # never over one of the repository's own
    local mode
    for mode in ${modes//,/ }; do
        capture_mode "${out}" "${mode}" ${@+"$@"} || status=1
    done
    echo "log: ${out}"
    return "${status}"
}

case "${1:-}" in
    capture) shift; capture ${@+"$@"} ;;
    compare) shift; exec "${SCRIPT_DIR}/.venv/bin/python3" "${SCRIPT_DIR}/suite_log.py" ${@+"$@"} ;;
    *) sed -n '2,9p' "${BASH_SOURCE[0]}"; exit 2 ;;
esac
