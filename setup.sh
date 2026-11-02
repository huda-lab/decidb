#!/usr/bin/env bash
# Set up a fresh clone: check the prerequisites, build DeciDB, create the Python
# test environment, and generate the TPC-H database config.txt asks for.
# Safe to rerun.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

CONFIG_FILE="config.txt"
# Flag to track the current section
current_section=""
while IFS='=' read -r key value; do
  # Trim whitespace around 'key' and 'value'
  key=$(echo "$key" | xargs)
  value=$(echo "$value" | xargs)

  # Skip empty lines
  [[ -z "$key" ]] && continue

  # Detect section headers
  if [[ "$key" =~ ^\[.*\]$ ]]; then
    current_section="${key//[\[\]]/}" # Extract section name without brackets
    continue
  fi

  # Process keys based on the current section
  case "$current_section" in
    decidb)
      case "$key" in
        build_mode) BUILD_MODE=$value ;;
        db_file) DB_FILE=$value ;;
      esac
      ;;
    tpch)
      case "$key" in
        scale_factor) SCALE_FACTOR=$value ;;
      esac
      ;;
  esac
done < "$CONFIG_FILE"

missing=()
for tool in cmake make python3; do
  command -v "$tool" > /dev/null || missing+=("$tool")
done
command -v c++ > /dev/null || command -v g++ > /dev/null || command -v clang++ > /dev/null || missing+=("a C++ compiler")
python3 -c 'import venv, ensurepip' 2> /dev/null || missing+=("the python3 venv module (python3-venv)")
if [[ ${#missing[@]} -gt 0 ]]; then
  echo "Error: missing prerequisites:" >&2
  printf '  - %s\n' "${missing[@]}" >&2
  exit 1
fi

export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN)}"
if command -v ninja > /dev/null; then
  GEN=ninja make "$BUILD_MODE"
else
  make "$BUILD_MODE"
fi

# The CLI runs without Gurobi (HiGHS is built in); only the test suite's oracle
# needs a license, so a failure here must not stop the rest of the setup.
tests_ready=1
make decide-setup || tests_ready=0

DB_PATH="build/$BUILD_MODE/$DB_FILE"
if [[ -f "$DB_PATH" ]]; then
  echo "TPC-H database already at $DB_PATH"
elif ! "build/$BUILD_MODE/decidb" "$DB_PATH" -c "LOAD tpch; CALL dbgen(sf = $SCALE_FACTOR);"; then
  echo "Error: Failed to set up TPC-H." >&2
  rm -f "$DB_PATH" "$DB_PATH.wal"
  exit 1
fi

echo
echo "Done. CLI: build/$BUILD_MODE/decidb"
if [[ $tests_ready -eq 1 ]]; then
  echo "Run the tests with: make decide-test"
else
  echo "The tests can't run yet: install a Gurobi license (see README.md), then rerun this script." >&2
fi
