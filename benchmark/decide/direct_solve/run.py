#!/usr/bin/env python3
"""Times direct solve against the forced solver (Gurobi, HiGHS) on one query per direct-solve class, over TPC-H.

Each file in `queries/` is one DECIDE query over the TPC-H tables, with two header comments the runner reads:
`-- decision: <column>` and `-- objective: <expression over the output>`. The parent query reads
`COUNT(*), SUM(<decision>), <objective>`, so the time is the plan, not the transfer of every row, and every timed run
also returns the objective it reached.

Modes. `direct` pins `decide_direct_solve='require'`, so a query the rule does not take fails instead of using the
solver. `gurobi` and `highs` pin `'off'` and choose the backend with `DECIDB_FORCE_SOLVER`.

Each measured run is a fresh `decidb` process that only opens the stored database, under `/usr/bin/time -l`, so its
wall time and peak memory are the query's own. Every mode runs `--repeats` times per cell; the table reports the
median wall time with the min and max, and the largest peak memory. A run past `--timeout` is killed with its process
group and reported as `timeout`; a failed run is reported as `error`. Neither is ever reported as a time.

Agreement. At every size, every finished run's objective must match direct solve's within 1e-6 relative. A mismatch
is printed loudly, written to the CSV, and stops the sweep with a non-zero exit, so no wrong answer is timed further.

Machine-heat rule: sizes run smallest first, and a solver runs a size only when its median at the previous size of
that query was within `--solver-budget` seconds.

Beside the CSV, a JSON manifest records the commit, the binary, the solver versions, the machine and the arguments.

    python3 benchmark/decide/direct_solve/run.py                           # every query, every size
    python3 benchmark/decide/direct_solve/run.py --sf 0.00125 0.0125 --queries r1
"""

import argparse
import csv
import json
import os
import platform
import re
import signal
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
BINARY = REPO / "build" / "release" / "decidb"
QUERIES = HERE / "queries"
RESULTS = HERE.parent / "results" / "direct_solve"  # own folder: view_results.py opens the newest JSON in results/
DB_DIR = HERE.parent / "databases" / "direct_solve"

# TPC-H scale factors; partsupp holds one decision per row: 800, 9.6K, 100K and 1.6M rows. dbgen drops the fraction of a
# scale factor above 1 (sf=1.25 generates sf=1), so the largest is a whole number.
SCALE_FACTORS = (0.00125, 0.0125, 0.125, 2)
MODES = ("direct", "gurobi", "highs")
TOLERANCE = 1e-6  # relative, on the objective


def load_query(name):
    """Returns (DECIDE query, decision column, objective expression) from queries/<name>.sql."""
    text = (QUERIES / f"{name}.sql").read_text()
    header = dict(re.findall(r"^-- (decision|objective): (.+)$", text, re.MULTILINE))
    body = "\n".join(line for line in text.splitlines() if not line.startswith("--")).strip().rstrip(";")
    return body, header["decision"], header["objective"]


def prepare(db, sf):
    """Generates the TPC-H tables at this scale factor once; a stored database is reused."""
    if db.exists():
        return
    db.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(BINARY), str(db), "-c", f"CALL dbgen(sf={sf}); CHECKPOINT;"], check=True, capture_output=True)


def run(db, mode, sql, timeout):
    """One fresh process. Returns (wall seconds, peak MiB, status, stdout); status is None when the run finished."""
    setting = "require" if mode == "direct" else "off"
    env = {k: v for k, v in os.environ.items() if k != "DECIDB_FORCE_SOLVER"}
    if mode != "direct":
        env["DECIDB_FORCE_SOLVER"] = mode
    command = [
        "/usr/bin/time",
        "-l",
        str(BINARY),
        str(db),
        "-readonly",
        "-noheader",
        "-csv",
        "-c",
        f"SET decide_direct_solve='{setting}'; {sql}",
    ]
    start = time.perf_counter()
    proc = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env, start_new_session=True
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)  # the whole group: `time` and the decidb it started
        proc.communicate()
        return None, None, "timeout", ""
    wall = time.perf_counter() - start
    peak = re.search(r"(\d+)\s+maximum resident set size", stderr)
    peak_mib = int(peak.group(1)) / (1024 * 1024) if peak else None
    before_stats = stderr.split("real")[0]
    if proc.returncode != 0 or "Error" in before_stats:
        detail = before_stats.strip().splitlines()[0][:160] if before_stats.strip() else "failed"
        print(f"  [{mode}] error: {detail}", file=sys.stderr)
        return wall, peak_mib, "error", stdout
    return wall, peak_mib, None, stdout


def measure(db, mode, sql, repeats, timeout):
    """Runs one cell. Returns a dict with the median, min and max wall time, the max peak MiB, the decision count and
    every run's objective, or {"status": ...} at the first failure."""
    walls, peaks, objectives = [], [], []
    for _ in range(repeats):
        wall, peak, status, stdout = run(db, mode, sql, timeout)
        if status:
            return {"status": status}
        count, _, objective = stdout.strip().splitlines()[-1].split(",")
        walls.append(wall)
        peaks.append(peak)
        objectives.append(float(objective))
    return {
        "median": statistics.median(walls),
        "min": min(walls),
        "max": max(walls),
        "peak": max(p for p in peaks if p is not None),
        "decisions": int(count),
        "objectives": objectives,
    }


def disagreements(cells):
    """Every finished run's objective against direct solve's first; returns a description of each mismatch."""
    reference = cells["direct"]["objectives"][0]
    bad = []
    for mode, cell in cells.items():
        for value in cell.get("objectives", []):
            if abs(value - reference) > TOLERANCE * max(1.0, abs(value), abs(reference)):
                bad.append(f"direct {reference!r} vs {mode} {value!r}")
    return bad


def command_output(command, cwd=None):
    try:
        return subprocess.run(command, capture_output=True, text=True, cwd=cwd, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def manifest(args, stamp, output):
    """The setup a reader needs to reproduce or compare the run."""
    highs = dict(
        re.findall(
            r"#define HIGHS_VERSION_(MAJOR|MINOR|PATCH) (\d+)", (REPO / "third_party/highs/HConfig.h").read_text()
        )
    )
    gurobi = command_output(["gurobi_cl", "--version"])
    duckdb = command_output([str(BINARY), "-noheader", "-csv", "-c", "SELECT version(), current_setting('threads');"])
    return {
        "command": "benchmark/decide/direct_solve/run.py",
        "generated_at_utc": stamp,
        "output": str(output),
        "arguments": {k: (str(v) if isinstance(v, Path) else v) for k, v in vars(args).items()},
        "commit": command_output(["git", "rev-parse", "HEAD"], cwd=REPO),
        # what decides the numbers: the engine, the bundled solvers, and this benchmark's own code and queries
        "working_tree_dirty": bool(
            command_output(["git", "status", "--porcelain", "--", "src", "third_party", str(HERE)], cwd=REPO)
        ),
        "binary": str(BINARY),
        "binary_built": datetime.fromtimestamp(BINARY.stat().st_mtime, timezone.utc).isoformat(),
        "decidb_version_and_threads": duckdb,
        "gurobi": gurobi.splitlines()[0] if gurobi else None,
        "gurobi_home": os.environ.get("GUROBI_HOME"),
        "highs": f"{highs['MAJOR']}.{highs['MINOR']}.{highs['PATCH']}",
        "solver_threads": "solver default (DeciDB sets no thread count)",
        "platform": platform.platform(),
        "cpu": command_output(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "cores": os.cpu_count(),
        "memory_bytes": int(command_output(["sysctl", "-n", "hw.memsize"]) or 0) or None,
        "python": platform.python_version(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    names = sorted(p.stem for p in QUERIES.glob("*.sql"))
    parser.add_argument("--sf", nargs="+", type=float, default=list(SCALE_FACTORS), help="TPC-H scale factors")
    parser.add_argument("--queries", nargs="+", choices=names, default=names)
    parser.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES))
    parser.add_argument("--repeats", type=int, default=3, help="runs of every mode per cell (median is reported)")
    parser.add_argument("--timeout", type=int, default=600, help="seconds before a run is killed and reported")
    parser.add_argument(
        "--solver-budget",
        type=float,
        default=300.0,
        help="run a solver at a size only if its median at the previous (smaller) size took at most this long",
    )
    parser.add_argument("--db-dir", type=Path, default=DB_DIR)
    args = parser.parse_args()
    if "direct" not in args.modes:
        parser.error("--modes must include direct, the baseline every speedup and objective is measured against")
    if not BINARY.exists():
        parser.error(f"build the release CLI first: {BINARY}")

    sizes = sorted(args.sf)
    dbs = {sf: args.db_dir / f"tpch_sf{sf}.db" for sf in sizes}
    for sf in sizes:
        prepare(dbs[sf], sf)

    solvers = [m for m in args.modes if m != "direct"]
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    RESULTS.mkdir(parents=True, exist_ok=True)
    output = RESULTS / f"direct_solve_{stamp}.csv"
    output.with_suffix(".json").write_text(json.dumps(manifest(args, stamp, output), indent=2) + "\n")

    columns = ["query", "sf", "decisions", "objective", "agree"]
    for mode in args.modes:
        columns += [f"{mode} median s", f"{mode} min s", f"{mode} max s", f"{mode} peak MiB"]
    columns += [f"speedup vs {solver}" for solver in solvers]
    shown = ["query", "decisions", "objective"] + [f"{m} s (min-max)" for m in args.modes] + columns[-len(solvers) :]
    print("| " + " | ".join(shown) + " |")
    print("|---" + "|---:" * (len(shown) - 1) + "|")

    def status_of(cell):
        return cell.get("status")

    last = {}  # (solver, query) -> last median or status, for the machine-heat rule
    failed = False
    with output.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(columns)
        for sf in sizes:
            for name in args.queries:
                body, decision, objective = load_query(name)
                sql = f"SELECT COUNT(*), SUM({decision}), {objective} FROM ({body}) q;"
                cells = {"direct": measure(dbs[sf], "direct", sql, args.repeats, args.timeout)}
                for solver in solvers:
                    previous = last.get((solver, name), 0.0)
                    if not isinstance(previous, float) or previous > args.solver_budget:
                        reason = previous if isinstance(previous, str) else "previous size too slow"
                        cells[solver] = {"status": f"skipped ({reason})"}
                        continue
                    cells[solver] = measure(dbs[sf], solver, sql, args.repeats, args.timeout)
                    last[(solver, name)] = status_of(cells[solver]) or cells[solver]["median"]

                direct = cells["direct"]
                bad = [] if status_of(direct) else disagreements(cells)
                agree = "-" if status_of(direct) else ("NO" if bad else "yes")
                row = [
                    name,
                    sf,
                    direct.get("decisions", "-"),
                    direct["objectives"][0] if "objectives" in direct else "-",
                ]
                row.append(agree)
                for mode in args.modes:
                    cell = cells[mode]
                    if status_of(cell):
                        row += [status_of(cell), "-", "-", "-"]
                    else:
                        row += [
                            f"{cell['median']:.4f}",
                            f"{cell['min']:.4f}",
                            f"{cell['max']:.4f}",
                            f"{cell['peak']:.1f}",
                        ]
                speeds = []
                for solver in solvers:
                    finished = not status_of(direct) and not status_of(cells[solver])
                    speeds.append(f"{cells[solver]['median'] / direct['median']:.1f}x" if finished else "-")
                writer.writerow(row + speeds)
                handle.flush()

                def show(cell):
                    if status_of(cell):
                        return status_of(cell)
                    return f"{cell['median']:.3f} ({cell['min']:.3f}-{cell['max']:.3f})"

                print(
                    "| "
                    + " | ".join([name, str(row[2]), str(row[3])] + [show(cells[m]) for m in args.modes] + speeds)
                    + " |"
                )
                sys.stdout.flush()
                if bad:
                    for why in bad:
                        print(f"!!!!!! MISMATCH {name} at sf={sf}: {why} !!!!!!", file=sys.stderr)
                    failed = True
                    break
            if failed:
                break
    print(f"\nwrote {output} and {output.with_suffix('.json')}", file=sys.stderr)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
