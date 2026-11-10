#!/usr/bin/env python3
"""Times direct solve against the forced solver (Gurobi, HiGHS) on one query per direct-solve class, over TPC-H.

Each file in `queries/` is one DECIDE query over the TPC-H tables, with two header comments the runner reads:
`-- decision: <column>` and `-- objective: <expression over the output>`. The parent query reads
`COUNT(*), SUM(<decision>), <objective>`, so the time is the plan, not the transfer of every row, and every timed run
also returns the objective it reached.

Modes. `direct` pins `decide_direct_solve='require'`, so a query the rule does not take fails instead of using the
solver. `gurobi` and `highs` pin `'off'`, choose the backend with `DECIDB_FORCE_SOLVER`, and report model-build and
solve time. Every mode runs DuckDB on 32 threads, the most Gurobi uses by default, so both sides have the same cores.

Each measured run is a fresh `decidb` process that only opens the stored database, under `/usr/bin/time`, so its
wall time and peak memory are the query's own. A size's database is read once first, so no mode pays for the first
read from disk. Every mode runs `--repeats` times per cell, stopping early after a run past `--repeat-budget`; the
table reports the median wall time with the min and max, and the largest peak memory. A run past `--timeout` is killed
with its process group and reported as `timeout`; a failed run is reported as `error`. Neither is ever reported as a
time.

Agreement. At every size, every finished run's objective must match direct solve's within 1e-6 relative. A mismatch
is printed loudly, written to the CSV, and stops the sweep with a non-zero exit, so no wrong answer is timed further.
A size no solver finished reads `n/a`: there was nothing to agree with.

Machine-heat rule: sizes run smallest first, a size's database is generated when the sweep reaches it, and a solver
runs a size only when its median at the previous size of that query was within `--solver-budget` seconds.

Beside the CSV, a JSON manifest records the commit, the binary, the solver versions, the machine and the arguments.

    python3 benchmark/decide/direct_solve/run.py                           # every query, every size
    python3 benchmark/decide/direct_solve/run.py --sf 0.00125 0.0125 --queries r1

Compare mode (`--compare`). Runs, instead of the sweep above, each query's direct plan beside its comparator in `hand/`:
the plan an expert writes by hand for the same answer, which carries what the rule owes (its verdict before the first row
is released, its clamps and checks, R1's proportional ties). A plan that skips an obligation measures the obligation,
not the plan. Both sides run in one process under `PRAGMA enable_profiling='json'`, best of `--repeats`, once on all
threads and once on one. The table reports, for each side, the summed operator CPU (the measure: the sum of every
operator's time, whatever the thread count) and the latency; the planning DECIDE adds (the `optimizer.direct.*` spans,
read from `DECIDB_PROFILE`) is reported apart, since it is in the direct side's latency and in no operator's time. The
two sides' answers (`COUNT(*)`, `SUM(<decision>)` and the objective) are compared on one thread, where a sum is
deterministic: a difference is shown, and a relative difference beyond 1e-9 stops the run.

    python3 benchmark/decide/direct_solve/run.py --compare --sf 1 --queries a1 n1 r1
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

sys.path.insert(0, str(HERE.parent))
import run_benchmarks as bench  # noqa: E402  (the parent folder's /usr/bin/time flag and parser)

# TPC-H scale factors; partsupp holds one decision per row: 100K rows, doubling up to 409.6M. dbgen drops the fraction
# of a scale factor above 1 (sf=1.25 generates sf=1), so the larger ones are whole numbers.
SCALE_FACTORS = (0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512)
SOLVERS = ("gurobi", "highs")
MODES = ("direct",) + SOLVERS
DEFAULT_MODES = ("direct", "gurobi")
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
    partial = db.with_name(db.name + ".partial")  # a killed generation must not pass for a stored database
    partial.unlink(missing_ok=True)
    subprocess.run(
        [str(BINARY), str(partial), "-c", f"CALL dbgen(sf={sf}); CHECKPOINT;"], check=True, capture_output=True
    )
    partial.rename(db)


def warm(db):
    """Reads the database once, so no mode pays for the first read from disk."""
    with db.open("rb") as handle:
        while handle.read(1 << 24):
            pass


HAND = HERE / "hand"
BENCH_ERROR = re.compile(r"(Error|error:)")


def load_hand(name):
    """The comparator's SQL, without its comment lines."""
    text = (HAND / f"{name}.sql").read_text()
    return "\n".join(line for line in text.splitlines() if not line.startswith("--")).strip().rstrip(";")


def planning_ms(stderr):
    """The DECIDE planning of each direct run, in milliseconds: the `optimizer.direct.*` spans that share a parent
    (DECIDB_PROFILE prints a begin and an end event for each span, with its parent's id)."""
    per_parent = {}
    for line in stderr.splitlines():
        if not line.startswith("DECIDB_PROFILE: "):
            continue
        event = json.loads(line[len("DECIDB_PROFILE: ") :])
        if event.get("event") == "end" and event["name"].startswith("optimizer.direct."):
            per_parent[event["parent"]] = per_parent.get(event["parent"], 0.0) + event["duration_ms"]
    return list(per_parent.values())


def profile_pair(db, direct_sql, hand_sql, threads, repeats, timeout, scratch):
    """Runs both sides in one process. Returns {side: {"cpu": ms, "latency": ms}}, "planning" (ms, the direct side's) and
    "answers" (the first run's CSV line of each side), or {"status": ...} when a run failed."""
    setting = f"SET threads={threads}; " if threads else ""
    lines = [
        setting + "SET decide_direct_solve='require';",
        direct_sql,  # the first run of each side is not timed: it reads the table in
        hand_sql,
    ]
    for side, sql in (("direct", direct_sql), ("hand", hand_sql)):
        for i in range(repeats):
            lines.append(f"PRAGMA enable_profiling='json'; PRAGMA profiling_output='{scratch}/{side}{i}.json';")
            lines.append(sql)
            lines.append("PRAGMA disable_profiling;")
    env = {k: v for k, v in os.environ.items() if k not in ("DECIDB_FORCE_SOLVER", "DECIDB_BENCH", "DECIDB_TIME_LIMIT")}
    env["DECIDB_PROFILE"] = "1"
    try:
        proc = subprocess.run(
            [str(BINARY), str(db), "-readonly", "-noheader", "-csv"],
            input="\n".join(lines),
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired:
        return {"status": "timeout"}
    quiet = "\n".join(line for line in proc.stderr.splitlines() if not line.startswith("DECIDB_PROFILE: "))
    if proc.returncode != 0 or BENCH_ERROR.search(quiet + proc.stdout):
        return {"status": "error", "detail": (quiet + proc.stdout).strip()[:300]}
    result = {}
    for side in ("direct", "hand"):
        profiles = [json.loads((Path(scratch) / f"{side}{i}.json").read_text()) for i in range(repeats)]
        result[side] = {
            "cpu": min(p["cpu_time"] for p in profiles) * 1000,
            "latency": min(p["latency"] for p in profiles) * 1000,
        }
    result["planning"] = min(planning_ms(proc.stderr)[1:] or [0.0])  # the first run is cold
    result["answers"] = proc.stdout.strip().splitlines()[:2]
    return result


def answers_differ(direct_line, hand_line):
    """(identical, relative difference) of two result lines `count,sum,objective`."""
    if direct_line == hand_line:
        return True, 0.0
    worst = 0.0
    for a, b in zip(direct_line.split(","), hand_line.split(",")):
        x, y = float(a), float(b)
        worst = max(worst, abs(x - y) / max(1.0, abs(x), abs(y)))
    return False, worst


def compare(args):
    """The sweep's replacement under `--compare`: both sides of each query, best of `args.repeats`."""
    import tempfile

    print(f"machine load at start: {os.getloadavg()[0]:.2f} (1 minute)", file=sys.stderr)
    rows, failed = [], False
    header = [
        "query",
        "sf",
        "direct CPU ms",
        "hand CPU ms",
        "CPU ratio",
        "direct latency ms",
        "hand latency ms",
        "DECIDE planning ms",
        "threads: direct 1t/all",
        "threads: hand 1t/all",
        "answers",
    ]
    print("| " + " | ".join(header) + " |")
    print("|---" + "|---:" * (len(header) - 1) + "|")
    for sf in sorted(args.sf):
        db = args.db_dir / f"tpch_sf{sf}.db"
        prepare(db, sf)
        warm(db)
        for name in args.queries:
            body, decision, objective = load_query(name)
            parent = f"SELECT COUNT(*), SUM({decision}), {objective} FROM (%s) q;"
            direct_sql, hand_sql = parent % body, parent % load_hand(name)
            with tempfile.TemporaryDirectory() as scratch:
                all_threads = profile_pair(db, direct_sql, hand_sql, None, args.repeats, args.timeout, scratch)
                one_thread = profile_pair(db, direct_sql, hand_sql, 1, args.repeats, args.timeout, scratch)
            if "status" in all_threads or "status" in one_thread:
                bad = all_threads if "status" in all_threads else one_thread
                print(f"| {name} | {sf} | {bad['status']}: {bad.get('detail', '')} |")
                failed = True
                continue
            identical, difference = answers_differ(*one_thread["answers"])
            answers = "identical" if identical else f"differ by {difference:.1e}"
            if difference > 1e-9:
                failed = True
            d, h = all_threads["direct"], all_threads["hand"]
            row = [
                name,
                sf,
                f"{d['cpu']:.1f}",
                f"{h['cpu']:.1f}",
                f"{d['cpu'] / h['cpu']:.2f}",
                f"{d['latency']:.1f}",
                f"{h['latency']:.1f}",
                f"{all_threads['planning']:.2f}",
                f"{one_thread['direct']['latency'] / d['latency']:.2f}",
                f"{one_thread['hand']['latency'] / h['latency']:.2f}",
                answers,
            ]
            rows.append(row)
            print("| " + " | ".join(str(c) for c in row) + " |")
            sys.stdout.flush()
    print(f"machine load at end: {os.getloadavg()[0]:.2f} (1 minute)", file=sys.stderr)
    sys.exit(1 if failed else 0)


def run(db, mode, sql, timeout):
    """One fresh process. Returns (wall seconds, peak MiB, status, stdout, stderr); status is None when the run
    finished."""
    setting = "off" if mode in SOLVERS else "require"
    env = {k: v for k, v in os.environ.items() if k not in ("DECIDB_FORCE_SOLVER", "DECIDB_BENCH", "DECIDB_TIME_LIMIT")}
    if mode in SOLVERS:
        env["DECIDB_FORCE_SOLVER"] = mode
        env["DECIDB_BENCH"] = "1"
        env["DECIDB_TIME_LIMIT"] = str(timeout * 2)  # `--timeout` decides, not DeciDB's default limit of 300 s
    command = [
        "/usr/bin/time",
        bench.get_time_flag(),
        str(BINARY),
        str(db),
        "-readonly",
        "-noheader",
        "-csv",
        "-c",
        f"SET decide_direct_solve='{setting}'; SET threads=32; {sql}",
    ]
    start = time.perf_counter()
    proc = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,  # a terminal would make a slow solve stop and ask whether to continue
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
        start_new_session=True,
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)  # the whole group: `time` and the decidb it started
        proc.communicate()
        return None, None, "timeout", "", ""
    wall = time.perf_counter() - start
    peak_kb = bench.parse_time_output(stderr).get("peak_rss_kb")
    peak_mib = peak_kb / 1024 if peak_kb is not None else None
    # What the process wrote comes first; `time` then starts its report with `real` (BSD) or the command line (GNU),
    # which holds the query's text.
    before_stats = re.split(r"real|^\s*Command ", stderr, flags=re.MULTILINE)[0]
    if proc.returncode != 0 or "Error" in before_stats:
        lines = before_stats.strip().splitlines()
        # The error's own line, not the first: `DECIDB_BENCH:` timing lines are written before it.
        detail = next((line for line in lines if "Error" in line), lines[0] if lines else "failed")[:160]
        print(f"  [{mode}] error: {detail}", file=sys.stderr)
        return wall, peak_mib, "error", stdout, stderr
    return wall, peak_mib, None, stdout, stderr


def bench_seconds(stderr, name):
    """A `DECIDB_BENCH: <name>_ms=<value>` line, in seconds; None when the run printed none."""
    found = re.search(rf"^DECIDB_BENCH: {name}_ms=([\d.]+)$", stderr, re.MULTILINE)
    return float(found.group(1)) / 1000 if found else None


def measure(db, mode, sql, repeats, timeout, repeat_budget):
    """Runs one cell. Returns a dict with the runs made, the median, min and max wall time, the max peak MiB, the median
    model-build and solve time on a solver, the decision count and every run's objective, or {"status": ...} at the
    first failure. A run past `repeat_budget` seconds is the cell's last."""
    walls, peaks, objectives, models, solves = [], [], [], [], []
    for _ in range(repeats):
        wall, peak, status, stdout, stderr = run(db, mode, sql, timeout)
        if status:
            return {"status": status}
        count, _, objective = stdout.strip().splitlines()[-1].split(",")
        walls.append(wall)
        peaks.append(peak)
        objectives.append(float(objective))
        models.append(bench_seconds(stderr, "model_construction"))
        solves.append(bench_seconds(stderr, "solver"))
        if wall > repeat_budget:
            break
    return {
        "runs": len(walls),
        "median": statistics.median(walls),
        "min": min(walls),
        "max": max(walls),
        "peak": max(p for p in peaks if p is not None),
        "model": statistics.median(models) if None not in models else None,
        "solve": statistics.median(solves) if None not in solves else None,
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


def cpu_model(cpuinfo=Path("/proc/cpuinfo")):
    """The processor's name: Linux keeps it in /proc/cpuinfo, macOS in sysctl."""
    if cpuinfo.exists():
        model = re.search(r"^model name\s*:\s*(.+)$", cpuinfo.read_text(), re.MULTILINE)
        return model.group(1) if model else None
    return command_output(["sysctl", "-n", "machdep.cpu.brand_string"])


def manifest(args, stamp, output):
    """The setup a reader needs to reproduce or compare the run."""
    highs = dict(
        re.findall(
            r"#define HIGHS_VERSION_(MAJOR|MINOR|PATCH) (\d+)", (REPO / "third_party/highs/HConfig.h").read_text()
        )
    )
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
        "uncommitted_diff": command_output(["git", "diff", "HEAD", "--", "src", "third_party", str(HERE)], cwd=REPO),
        "queries": {path.stem: path.read_text() for path in sorted(QUERIES.glob("*.sql"))},
        "binary": str(BINARY),
        "binary_built": datetime.fromtimestamp(BINARY.stat().st_mtime, timezone.utc).isoformat(),
        "decidb_version_and_threads": duckdb,
        # the libraries the binary's dlopen search finds outside GUROBI_HOME; `gurobi_cl` needs a library path to run
        "gurobi": sorted(path.name for path in Path.home().glob("gurobi*/linux64/lib/libgurobi*.so*")),
        "gurobi_home": os.environ.get("GUROBI_HOME"),
        "highs": f"{highs['MAJOR']}.{highs['MINOR']}.{highs['PATCH']}",
        "solver_threads": "solver default (DeciDB sets no thread count)",
        "platform": platform.platform(),
        "cpu": cpu_model(),
        "cores": os.cpu_count(),
        "memory_bytes": os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES"),
        "python": platform.python_version(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    names = sorted(p.stem for p in QUERIES.glob("*.sql"))
    parser.add_argument("--sf", nargs="+", type=float, default=list(SCALE_FACTORS), help="TPC-H scale factors")
    parser.add_argument("--queries", nargs="+", choices=names, default=names)
    parser.add_argument("--modes", nargs="+", choices=MODES, default=list(DEFAULT_MODES))
    parser.add_argument(
        "--repeats",
        type=int,
        default=None,
        help="runs of every mode per cell (median is reported; 3), or of each side of --compare (best is reported; 5)",
    )
    parser.add_argument(
        "--repeat-budget", type=float, default=1800.0, help="a run past this many seconds is its cell's last"
    )
    parser.add_argument("--timeout", type=int, default=21600, help="seconds before a run is killed and reported")
    parser.add_argument(
        "--solver-budget",
        type=float,
        default=7200.0,
        help="run a solver at a size only if its median at the previous (smaller) size took at most this long",
    )
    parser.add_argument("--db-dir", type=Path, default=DB_DIR)
    parser.add_argument(
        "--compare",
        action="store_true",
        help="run each query's direct plan beside its comparator in hand/ (best of --repeats, one process) instead of "
        "the sweep; --modes is not used",
    )
    args = parser.parse_args()
    if args.repeats is None:
        args.repeats = 5 if args.compare else 3
    if args.compare:
        if not BINARY.exists():
            parser.error(f"build the release CLI first: {BINARY}")
        compare(args)
    if "direct" not in args.modes:
        parser.error("--modes must include direct, the baseline every speedup and objective is measured against")
    if not BINARY.exists():
        parser.error(f"build the release CLI first: {BINARY}")

    sizes = sorted(args.sf)
    dbs = {sf: args.db_dir / f"tpch_sf{sf}.db" for sf in sizes}

    solvers = [m for m in args.modes if m in SOLVERS]
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    RESULTS.mkdir(parents=True, exist_ok=True)
    output = RESULTS / f"direct_solve_{stamp}.csv"
    output.with_suffix(".json").write_text(json.dumps(manifest(args, stamp, output), indent=2) + "\n")

    columns = ["query", "sf", "decisions", "objective", "agree"]
    for mode in args.modes:
        columns += [f"{mode} runs", f"{mode} median s", f"{mode} min s", f"{mode} max s", f"{mode} peak MiB"]
        if mode in SOLVERS:
            columns += [f"{mode} model s", f"{mode} solve s"]
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
            try:
                prepare(dbs[sf], sf)
            except subprocess.CalledProcessError as error:
                print(f"!!!!!! generating sf={sf} failed, sweep stops: {error.stderr[:400]} !!!!!!", file=sys.stderr)
                failed = True
                break
            warm(dbs[sf])
            for name in args.queries:
                body, decision, objective = load_query(name)
                sql = f"SELECT COUNT(*), SUM({decision}), {objective} FROM ({body}) q;"
                cells = {}
                for mode in args.modes:
                    if mode in SOLVERS:
                        previous = last.get((mode, name), 0.0)
                        if not isinstance(previous, float) or previous > args.solver_budget:
                            reason = previous if isinstance(previous, str) else "previous size too slow"
                            cells[mode] = {"status": f"skipped ({reason})"}
                            continue
                    cells[mode] = measure(dbs[sf], mode, sql, args.repeats, args.timeout, args.repeat_budget)
                    if mode in SOLVERS:
                        last[(mode, name)] = status_of(cells[mode]) or cells[mode]["median"]

                direct = cells["direct"]
                bad = [] if status_of(direct) else disagreements(cells)
                compared = any("objectives" in cells[solver] for solver in solvers)
                agree = "-" if status_of(direct) else ("NO" if bad else ("yes" if compared else "n/a"))
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
                        row += [status_of(cell), "-", "-", "-", "-"]
                    else:
                        row += [
                            cell["runs"],
                            f"{cell['median']:.4f}",
                            f"{cell['min']:.4f}",
                            f"{cell['max']:.4f}",
                            f"{cell['peak']:.1f}",
                        ]
                    if mode in SOLVERS:
                        split = [cell.get("model"), cell.get("solve")]
                        row += [f"{value:.4f}" if value is not None else "-" for value in split]
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
