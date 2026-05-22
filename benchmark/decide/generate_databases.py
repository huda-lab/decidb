#!/usr/bin/env python3
"""Generate TPC-H databases at multiple scale factors for benchmarking.

Usage:
    python3 benchmark/decide/generate_databases.py
    make decide-bench-setup
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
DATABASES_DIR = SCRIPT_DIR / "databases"
DECIDB_EXE = REPO_ROOT / "build" / "release" / "decidb"

DB_SIZES: dict[str, tuple[float, int]] = {
    "medium": (0.085, 500_000),
    "large": (0.17, 1_000_000),
}

GENERATE_TIMEOUT_SECONDS = 1_200


def run_decidb(db_path: Path, sql: str, *, readonly: bool = False,
               timeout: int = GENERATE_TIMEOUT_SECONDS) -> subprocess.CompletedProcess[str]:
    cmd = [str(DECIDB_EXE), str(db_path)]
    if readonly:
        cmd.append("-readonly")
    cmd.extend(["-c", sql])
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def get_lineitem_count(db_path: Path) -> int | None:
    result = subprocess.run(
        [
            str(DECIDB_EXE),
            str(db_path),
            "-readonly",
            "-csv",
            "-noheader",
            "-c",
            "SELECT COUNT(*) FROM lineitem;",
        ],
        capture_output=True,
        text=True,
        timeout=60,
    )
    if result.returncode != 0:
        return None

    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if not lines:
        return None
    try:
        return int(lines[-1].strip('"'))
    except ValueError:
        return None


def remove_database(db_path: Path) -> None:
    for path in (db_path, Path(f"{db_path}.wal")):
        if path.exists():
            path.unlink()


def generate_database(name: str, sf: float, lineitem_rows: int, db_path: Path) -> None:
    sql = f"""
LOAD tpch;
CALL dbgen(sf={sf});
CREATE TABLE lineitem_trimmed AS
    SELECT *
    FROM lineitem
    ORDER BY l_orderkey, l_linenumber
    LIMIT {lineitem_rows};
DROP TABLE lineitem;
ALTER TABLE lineitem_trimmed RENAME TO lineitem;
CHECKPOINT;
"""
    print(f"  Generating {name}.db (sf={sf}, lineitem_rows={lineitem_rows})...",
          end="", flush=True)
    result = run_decidb(db_path, sql)
    if result.returncode != 0:
        print(" FAILED")
        print(f"  Error: {result.stderr.strip()}", file=sys.stderr)
        sys.exit(1)

    actual_count = get_lineitem_count(db_path)
    if actual_count != lineitem_rows:
        print(" FAILED")
        print(
            f"  Error: expected {lineitem_rows} lineitem rows, got {actual_count}",
            file=sys.stderr,
        )
        sys.exit(1)
    print(f" OK ({db_path.stat().st_size / 1024 / 1024:.1f} MB)")


def main() -> None:
    if not DECIDB_EXE.exists():
        print(f"ERROR: decidb executable not found at {DECIDB_EXE}", file=sys.stderr)
        print("Run 'make release' first.", file=sys.stderr)
        sys.exit(1)

    DATABASES_DIR.mkdir(parents=True, exist_ok=True)

    for name, (sf, lineitem_rows) in DB_SIZES.items():
        db_path = DATABASES_DIR / f"{name}.db"
        if db_path.exists():
            actual_count = get_lineitem_count(db_path)
            if actual_count == lineitem_rows:
                print(f"  {name}.db already exists with {lineitem_rows} lineitem rows, skipping")
                continue
            print(
                f"  {name}.db has {actual_count} lineitem rows; "
                f"expected {lineitem_rows}, regenerating"
            )
            remove_database(db_path)

        generate_database(name, sf, lineitem_rows, db_path)

    print()
    print("Database sizes:")
    for name in DB_SIZES:
        db_path = DATABASES_DIR / f"{name}.db"
        if db_path.exists():
            size_mb = db_path.stat().st_size / 1024 / 1024
            print(f"  {name:>8}.db  {size_mb:>8.1f} MB")


if __name__ == "__main__":
    main()
