#!/usr/bin/env python3
"""Times direct solve against the solver on A1 shapes (separable box-constrained problem).

Each measured run is a fresh `decidb` process that only opens the stored database, so its peak memory
(`maximum resident set size`, from `/usr/bin/time -l`) is the query's own. `direct` pins
`decide_direct_solve='require'`, so a query the rule does not apply to fails instead of using the solver; `solver` pins
`'off'`. Results are read back as one aggregate row, so the time is the plan, not the transfer of millions of rows.

Machine-heat rule: shapes run one at a time, smallest size first, and a size runs on the solver only when the
previous size finished within `--solver-budget` seconds.

    python3 benchmark/decide/direct_solve/archive/profile_direct_a1.py --rows 100000 1000000 --db-dir /tmp/a1db
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BINARY = HERE.parents[3] / "build" / "release" / "decidb"

# name -> (source table, DECIDE clause, what the parent reads)
SHAPES = {
    "real_quadratic": (
        "src",
        "x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))",
        "COUNT(*), SUM(x)",
    ),
    "real_quadratic_when": (
        "src",
        "x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2)) WHEN (f)",
        "COUNT(*), SUM(x)",
    ),
    "int_linear": ("src", "x(INT) SUCH THAT x BETWEEN 0 AND cap MAXIMIZE SUM(score * x)", "COUNT(*), SUM(x)"),
    "bool_bounds": ("src", "x(BOOL) SUCH THAT x <= 1 MAXIMIZE SUM(score * x)", "COUNT(*), SUM(x)"),
    "constant_problem": ("src", "x(REAL) SUCH THAT x BETWEEN 0 AND 10 MAXIMIZE SUM(3 * x)", "COUNT(*), SUM(x)"),
    "wide_unused_payload": (
        "src_wide",
        "x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))",
        "COUNT(*), SUM(x)",
    ),
    "wide_used_payload": (
        "src_wide",
        "x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))",
        "COUNT(*), SUM(x), SUM(length(payload))",
    ),
    "count_only": ("src", "x(REAL) SUCH THAT x BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))", "COUNT(*)"),
}


def prepare(db, rows):
    """Builds the source tables once per size."""
    sql = f"""
        DROP TABLE IF EXISTS src; DROP TABLE IF EXISTS src_wide;
        CREATE TABLE src AS SELECT i AS id,
            (((i * 7919) % 4001) / 100.0 - 10.0)::DOUBLE AS t,
            (5 + (i % 23))::DOUBLE AS cap,
            (((i * 31) % 201) / 10.0 - 10.0)::DOUBLE AS score,
            (i % 3 = 0) AS f
        FROM range({rows}) r(i);
        CREATE TABLE src_wide AS SELECT *, repeat('p', 512) AS payload FROM src;
        CHECKPOINT;
    """
    subprocess.run([str(BINARY), str(db)], input=sql, text=True, check=True, capture_output=True)


def measure(db, mode, shape, timeout):
    table, decide, read = SHAPES[shape]
    setting = "require" if mode == "direct" else "off"
    sql = f"SET decide_direct_solve='{setting}'; SELECT {read} FROM (SELECT * FROM {table} DECIDE {decide}) q;"
    start = time.perf_counter()
    try:
        done = subprocess.run(
            ["/usr/bin/time", "-l", str(BINARY), str(db), "-readonly", "-noheader", "-csv", "-c", sql],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return None, None, f"timeout after {timeout}s"
    wall = time.perf_counter() - start
    peak = re.search(r"(\d+)\s+maximum resident set size", done.stderr)
    peak_mib = int(peak.group(1)) / (1024 * 1024) if peak else None
    error = None
    if done.returncode != 0 or "Error" in done.stderr.split("real")[0]:
        error = done.stderr.strip().splitlines()[0][:120] if done.stderr.strip() else "failed"
    return wall, peak_mib, error


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rows", nargs="+", type=int, default=[100000])
    parser.add_argument("--shapes", nargs="+", choices=sorted(SHAPES), default=sorted(SHAPES))
    parser.add_argument("--modes", nargs="+", choices=["direct", "solver"], default=["direct", "solver"])
    parser.add_argument("--repeats", type=int, default=2, help="runs of direct per cell; the solver runs once")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument(
        "--solver-budget",
        type=float,
        default=15.0,
        help="run the solver at a size only if its previous (smaller) run took at most this long",
    )
    parser.add_argument("--db-dir", type=Path, required=True)
    args = parser.parse_args()
    args.db_dir.mkdir(parents=True, exist_ok=True)
    solver_time = {}
    print("| shape | rows | direct s | direct peak MiB | solver s | solver peak MiB | speedup |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for rows in sorted(args.rows):
        db = args.db_dir / f"a1_{rows}.db"
        if not db.exists():
            prepare(db, rows)
        for shape in args.shapes:
            cells = {}
            for mode in args.modes:
                if mode == "solver" and solver_time.get(shape, 0.0) > args.solver_budget:
                    cells[mode] = ("skipped (previous size too slow)", None)
                    continue
                runs = [measure(db, mode, shape, args.timeout) for _ in range(args.repeats if mode == "direct" else 1)]
                good = [r for r in runs if r[2] is None]
                if not good:
                    cells[mode] = (runs[0][2], None)
                    continue
                wall = sorted(r[0] for r in good)[len(good) // 2]
                peak = max(r[1] for r in good if r[1] is not None)
                cells[mode] = (wall, peak)
                if mode == "solver":
                    solver_time[shape] = wall
            d, s = cells.get("direct", (None, None)), cells.get("solver", (None, None))
            fmt = lambda v: f"{v:.2f}" if isinstance(v, float) else (v or "-")
            speed = f"{s[0] / d[0]:.1f}x" if isinstance(s[0], float) and isinstance(d[0], float) else "-"
            print(f"| {shape} | {rows} | {fmt(d[0])} | {fmt(d[1])} | {fmt(s[0])} | {fmt(s[1])} | {speed} |")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
