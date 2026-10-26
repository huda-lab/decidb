#!/usr/bin/env python3
"""Measures the least R1's plan could cost, and how its time scales (R1-12).

It runs on the databases of `profile_direct_r1.py` (same `src` table, same stored budgets) and times, each in a fresh
`decidb` process under `/usr/bin/time -l`:

  floor   a hand-written DuckDB query of the narrow shape for `le_positive`: (id, t = v / w, room = w * cap) from
          `src`; one sort by t descending; a running sum of rooms; the room each row uses is
          LEAST(room, GREATEST(B - (run - room), 0)), which is the break-item rule in one expression (rooms before the
          break item in full, the remainder at it, nothing after); a join back to `src` on id gives every row its x.
          B is the `spend_30` budget that `prepare` stores. The parent reads `COUNT(*), SUM(x)` like le_positive.
  scan    a bare scan of `src` that returns one DOUBLE per row (w_pos * cap), read as `COUNT(*), SUM(x)`: the cost of
          reading and returning the rows.
  direct  the direct plan (`decide_direct_solve='require'`) for each shape, once per `SET threads` setting.
  gurobi  the solver path (`DECIDB_FORCE_SOLVER=gurobi`, `decide_direct_solve='off'`, `DECIDB_BENCH=1`), split into
          model construction, solve (both from the `DECIDB_BENCH:` lines on stderr) and the rest of the wall time.

Per run it records wall time, user and system CPU time (CPU over wall shows how many cores the run kept busy), peak
memory, page faults and swaps. The repeats of all methods and cells at one size are interleaved round-robin, so
thermal drift lands on all of them alike. The table reports the minimum, median and maximum wall time over the
finished runs; the JSON file keeps every raw record.

Before anything is timed, the floor and the direct plan must give the same `le_positive` objective within 1e-6
relative at every size; a mismatch or a failed check exits non-zero. A run over `--timeout` is killed with its whole
process group and recorded as `timeout`; a failed run is recorded as `error`. Neither is ever reported as a time.

    python3 benchmark/decide/direct_solve/archive/profile_r1_floor.py --rows 100000 --db-dir build/r1db_smoke
"""

import argparse
import json
import os
import re
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path

from profile_direct_r1 import BINARY, SHAPES, budgets_path, prepare, query_sql

DEFAULT_SHAPES = ["le_positive", "per_groups", "when_budget", "two_decisions"]
DEFAULT_THREADS = ["1", "2", "4", "all"]
METHODS = ("floor", "scan", "direct", "gurobi")
READ = SHAPES["le_positive"][1]  # what the parent reads from le_positive: COUNT(*), SUM(x)
AGREE_READ = "SUM(v*x), SUM(x)"


def floor_sql(db, read):
    """The hand-written narrow query for le_positive; `read` is evaluated over the columns v and x of every row."""
    budget = json.loads(budgets_path(db).read_text())["spend_30"]
    return f"""
        WITH n AS (SELECT id, v / w_pos AS t, w_pos * cap AS room FROM src),
        r AS (
            SELECT id, room,
                SUM(room) OVER (ORDER BY t DESC ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS run
            FROM n
        ),
        u AS (SELECT id, LEAST(room, GREATEST({budget!r} - (run - room), 0.0)) AS used FROM r)
        SELECT {read}
        FROM (SELECT s.v AS v, u.used / s.w_pos AS x FROM u JOIN src s USING (id)) q;
    """


def scan_sql():
    return "SELECT COUNT(*), SUM(x) FROM (SELECT (w_pos * cap)::DOUBLE AS x FROM src) q;"


def run_once(db, sql, mode, threads, timeout):
    """One fresh process. Returns a record: status None when the run finished, else 'timeout' or 'error'."""
    env = {k: v for k, v in os.environ.items() if k not in ("DECIDB_FORCE_SOLVER", "DECIDB_BENCH")}
    prefix = ""
    if mode == "direct":
        prefix = "SET decide_direct_solve='require'; "
    elif mode == "gurobi":
        prefix = "SET decide_direct_solve='off'; "
        env["DECIDB_FORCE_SOLVER"] = "gurobi"
        env["DECIDB_BENCH"] = "1"
    if threads != "all":
        prefix = f"SET threads={int(threads)}; " + prefix
    command = ["/usr/bin/time", "-l", str(BINARY), str(db), "-readonly", "-noheader", "-csv", "-c", prefix + sql]
    start = time.perf_counter()
    proc = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env, start_new_session=True
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)  # the whole group: `time` and the decidb it started
        proc.communicate()
        return {"status": "timeout"}
    wall = time.perf_counter() - start
    record = {"status": None, "wall": wall, "stdout": stdout.strip()}
    timing = re.search(r"^\s*([\d.]+)\s+real\s+([\d.]+)\s+user\s+([\d.]+)\s+sys", stderr, re.M)
    if proc.returncode != 0 or timing is None or "Error" in stderr[: timing.start()]:
        head = stderr[: timing.start()] if timing else stderr
        detail = head.strip().splitlines()[0][:160] if head.strip() else "failed"
        print(f"  [{mode} threads={threads}] error: {detail}", file=sys.stderr)
        return {"status": "error", "detail": detail}
    record["user"], record["sys"] = float(timing.group(2)), float(timing.group(3))
    record["cpu"] = record["user"] + record["sys"]
    for key, label, scale in (
        ("peak_mib", "maximum resident set size", 1024 * 1024),
        ("faults", "page faults", 1),
        ("swaps", "swaps", 1),
    ):
        found = re.search(rf"(\d+)\s+{label}", stderr)
        record[key] = int(found.group(1)) / scale if found else None
    if mode == "gurobi":
        for key, label in (("model_ms", "model_construction_ms"), ("solver_ms", "solver_ms")):
            found = re.search(rf"DECIDB_BENCH: {label}=([\d.]+)", stderr)
            if found is None:
                print(f"  [gurobi] DECIDB_BENCH line {label} missing or unparseable", file=sys.stderr)
                return {"status": "error", "detail": f"DECIDB_BENCH {label} missing"}
            record[key] = float(found.group(1))
        record["rest_ms"] = record["wall"] * 1000.0 - record["model_ms"] - record["solver_ms"]
    return record


def check_agreement(db):
    """The floor and the direct plan must reach the same le_positive objective. Returns an error text or None."""
    floor = run_once(db, floor_sql(db, AGREE_READ), "floor", "all", 3600)
    direct = run_once(db, query_sql(db, "le_positive", AGREE_READ), "direct", "all", 3600)
    try:
        (fo, fx), (do, dx) = (tuple(float(v) for v in r["stdout"].splitlines()[-1].split(",")) for r in (floor, direct))
    except (KeyError, ValueError, IndexError):
        return f"floor {floor['status'] or 'ok'}, direct {direct['status'] or 'ok'}, no objective to compare"
    if abs(fo - do) > 1e-6 * max(1.0, abs(fo), abs(do)):
        return f"floor objective {fo!r} vs direct {do!r}"
    print(f"agree {db.name}: objective {do!r}; SUM(x) floor {fx!r} direct {dx!r}", file=sys.stderr)
    return None


def cells_for(args):
    """The (method, shape, threads) cells of one size, in the fixed order each repeat runs them."""
    cells = []
    if "floor" in args.methods:
        cells += [("floor", "le_positive", t) for t in args.floor_threads]
    if "scan" in args.methods:
        cells += [("scan", "-", t) for t in args.floor_threads]
    if "direct" in args.methods:
        cells += [("direct", shape, t) for shape in args.shapes for t in args.threads]
    if "gurobi" in args.methods:
        cells += [("gurobi", shape, "all") for shape in args.shapes]
    return cells


def sql_for(db, cell):
    method, shape, _ = cell
    if method == "floor":
        return floor_sql(db, READ)
    if method == "scan":
        return scan_sql()
    return query_sql(db, shape, SHAPES[shape][1])


def summarize(records, key):
    values = [r[key] for r in records if r.get(key) is not None]
    return (min(values), statistics.median(values), max(values)) if values else (None, None, None)


def table_rows(rows, cells, records):
    out = []
    for cell in cells:
        runs = [r for r in records if r["rows"] == rows and (r["method"], r["shape"], r["threads"]) == cell]
        done = [r for r in runs if r["status"] is None]
        failed = sorted({r["status"] for r in runs if r["status"]})
        wall = summarize(done, "wall")
        cpu = summarize(done, "cpu")[1]
        ratio = statistics.median(r["cpu"] / r["wall"] for r in done) if done else None
        f = lambda v, p=2: "-" if v is None else f"{v:.{p}f}"
        peak, faults, swaps = (summarize(done, k)[1] for k in ("peak_mib", "faults", "swaps"))
        gurobi = [f(summarize(done, k)[1], 0) for k in ("model_ms", "solver_ms", "rest_ms")]
        count = f"{len(done)}/{len(runs)}" + (f" ({', '.join(failed)})" if failed else "")
        out.append(
            "| "
            + " | ".join(
                [cell[0], cell[1], cell[2], f"{rows:,}", count]
                + [f(wall[0]), f(wall[1]), f(wall[2]), f(cpu), f(ratio), f(peak, 0), f(faults, 0), f(swaps, 0)]
                + (gurobi if cell[0] == "gurobi" else ["-"] * 3)
            )
            + " |"
        )
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rows", nargs="+", type=int, default=[100000])
    parser.add_argument("--shapes", nargs="+", choices=sorted(SHAPES), default=DEFAULT_SHAPES)
    parser.add_argument("--methods", nargs="+", choices=METHODS, default=list(METHODS))
    parser.add_argument("--threads", nargs="+", default=DEFAULT_THREADS, help="direct plan: integers or `all`")
    parser.add_argument("--floor-threads", nargs="+", default=["all"], help="floor and scan: integers or `all`")
    parser.add_argument("--repeats", type=int, default=3, help="runs per cell, interleaved round-robin")
    parser.add_argument("--timeout", type=int, default=300, help="seconds before a run is killed and recorded")
    parser.add_argument("--db-dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, help="JSON file of raw records (default: <db-dir>/r1_floor.json)")
    args = parser.parse_args()
    for t in args.threads + args.floor_threads:
        if t != "all" and not (t.isdigit() and int(t) > 0):
            parser.error(f"thread setting {t!r} must be a positive integer or `all`")
    out = args.out or args.db_dir / "r1_floor.json"
    args.db_dir.mkdir(parents=True, exist_ok=True)
    sizes = sorted(args.rows)
    dbs = {}
    for rows in sizes:
        dbs[rows] = args.db_dir / f"r1_{rows}.db"
        if not dbs[rows].exists() or not budgets_path(dbs[rows]).exists():
            dbs[rows].unlink(missing_ok=True)
            prepare(dbs[rows], rows)

    if "floor" in args.methods:
        for rows in sizes:
            problem = check_agreement(dbs[rows])
            if problem:
                print(f"!!!!!! MISMATCH at {rows} rows: {problem} !!!!!!", file=sys.stderr)
                sys.exit(1)

    cells = cells_for(args)
    header = [
        "method", "shape", "threads", "rows", "ok runs", "wall min s", "wall med s", "wall max s", "CPU med s",
        "CPU/wall", "peak MiB", "page faults", "swaps", "model ms", "solve ms", "rest ms",
    ]  # fmt: skip
    records = []
    lines = ["| " + " | ".join(header) + " |", "|---|---|---:" + "|---:" * (len(header) - 3) + "|"]
    for rows in sizes:
        for repeat in range(args.repeats):
            for cell in cells:
                record = run_once(dbs[rows], sql_for(dbs[rows], cell), cell[0], cell[2], args.timeout)
                record.update(rows=rows, method=cell[0], shape=cell[1], threads=cell[2], repeat=repeat)
                records.append(record)
        lines += table_rows(rows, cells, records)
        out.write_text(json.dumps({"args": {k: str(v) for k, v in vars(args).items()}, "records": records}, indent=1))
    table = "\n".join(lines)
    out.with_suffix(".md").write_text(table + "\n")
    print(table)


if __name__ == "__main__":
    main()
