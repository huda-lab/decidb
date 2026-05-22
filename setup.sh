#!/usr/bin/env bash
set -o pipefail

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


GEN=ninja make $BUILD_MODE
cd build/$BUILD_MODE

SQL_SCRIPT=$(mktemp)
cat << EOF > "$SQL_SCRIPT"
LOAD tpch;
CALL dbgen(sf = $SCALE_FACTOR);
EOF
./duckdb "$DB_FILE" < "$SQL_SCRIPT"
if [ $? -ne 0 ]; then
    echo "Error: Failed to set up TPC-H."
    rm -f "$SQL_SCRIPT"
    exit 1
fi
rm -f "$SQL_SCRIPT"
