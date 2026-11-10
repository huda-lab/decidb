#!/usr/bin/env python3
"""How DeciDB's own time grows with the input for the ANR workloads (language/guide.md §5.5, §5.6, §7.1).

Each workload is one DECIDE query over a synthetic table of `n` rows, run at several `n`
with the key cardinalities, nesting depth, variant count and frame width held fixed, so
that every helper, window and member grows with `n` alone. DeciDB's own time is the
optimizer's and the model's construction (`DECIDB_BENCH`), the solve left out; the model's
size and the ANR counters (`DECIDB_PROFILE`: anr.helpers, anr.members, ...) are recorded
beside it, and the peak memory of the process that ran the query: its whole peak resident
set, so the solver's share is in it too. Each workload runs `--runs` times (3) at each size,
and every time and the peak are the median of those runs. A workload grows linearly when its
time's exponent -- the slope of log(time) against log(n) between the two largest sizes --
stays near 1.

    python3 benchmark/decide/anr_scaling.py                  # 10^3 .. 10^5 rows
    python3 benchmark/decide/anr_scaling.py --max 1000000    # up to 10^6
    python3 benchmark/decide/anr_scaling.py --only range     # workloads whose name matches
    python3 benchmark/decide/anr_scaling.py --phase P0       # store these numbers as P0's
    python3 benchmark/decide/anr_scaling.py --phase P2 --against P0
    DECIDB_EVALUATOR=tree python3 benchmark/decide/anr_scaling.py --phase T --against P2

Writes results/anr_scaling.csv, or with --phase results/anr_scaling_<phase>.csv, where a run
replaces the workloads and sizes it measured and keeps the rest, so `--only` can add to a
phase. --against prints each time and peak as a multiple of that phase's as stored before
this run, so `--phase P2 --against P2` compares a rerun with P2's last numbers (the results
directory is not committed).
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DECIDB = HERE.parent.parent / "build" / "release" / "decidb"
RESULTS = HERE / "results"

#: The synthetic input: `part` (100 timelines of n / 100 periods each), `p` its period,
#: `w` a small weight, keys `g1`..`g4` of ten values each nested in the one before, `v`,
#: ten values that vary inside every `g1` class, and `q`, timelines of three rows each
#: ordered by `trio`, so a cyclic range of nine positions visits each three times.
TABLE = """(SELECT i AS id, i % 100 AS part, i // 100 AS p, (i * 7919) % 13 AS w,
        i % 10 AS g1, (i // 10) % 10 AS g2, (i // 100) % 10 AS g3, (i // 1000) % 10 AS g4,
        (i // 10) % 10 AS v, i % 3 AS trio, i // 3 AS q
        FROM range({n}) r(i)) t"""

TL = "over (t.p within t.part)"

#: name -> (decisions, constraints, objective). Every decision is REAL, so the solve is an
#: LP whatever the workload: what is measured is the model, not a branch-and-bound.
WORKLOADS = {
    "flat-sum": ("x(REAL)", "x <= 10 AND per t.g1: sum(x) by (t.g1) <= {n} / 20", "MAXIMIZE sum(x)"),
    "nested-1": ("x(REAL)", "x <= 10 AND per t.g1: sum(max(x) by (t.g1, t.g2)) by (t.g1) <= 50", "MAXIMIZE sum(x)"),
    "nested-2": (
        "x(REAL)",
        "x <= 10 AND per t.g1: sum(max(sum(x) by (t.g1, t.g2, t.g3)) by (t.g1, t.g2)) " "by (t.g1) <= {n} / 50",
        "MAXIMIZE sum(x)",
    ),
    "nested-3": (
        "x(REAL)",
        "x <= 10 AND per t.g1: sum(max(sum(max(x) by (t.g1, t.g2, t.g3, t.g4)) "
        "by (t.g1, t.g2, t.g3)) by (t.g1, t.g2)) by (t.g1) <= {n} / 50",
        "MAXIMIZE sum(x)",
    ),
    # `t.v * s` beside the reducer varies inside each class: ten variants per class (D70).
    "variants-10": (
        "x(REAL), per (): s(REAL)",
        "x <= 10 AND s <= 5 AND per t.g1: sum(x) by (t.g1) + t.v * s " "<= {n} / 20",
        "MAXIMIZE sum(x) + sum(per (): s)",
    ),
    "point": ("x(REAL)", f"x <= 10 AND x <= at(previous else 0: x) {TL} + t.w", "MAXIMIZE sum(x)"),
    "range-w1": ("x(REAL)", f"x <= 10 AND x <= sum(from previous to previous else 0: x) {TL} + t.w", "MAXIMIZE sum(x)"),
    "range-w5": (
        "x(REAL)",
        f"x <= 10 AND x <= avg(from 5 previous to previous else 0: x) {TL} + t.w",
        "MAXIMIZE sum(x)",
    ),
    "range-w50": (
        "x(REAL)",
        f"x <= 10 AND x <= avg(from 50 previous to previous else 0: x) {TL} + t.w",
        "MAXIMIZE sum(x)",
    ),
    "range-step7": (
        "x(REAL)",
        f"x <= 10 AND x <= avg(from 49 previous to previous every 7 else 0: x) {TL} + t.w",
        "MAXIMIZE sum(x)",
    ),
    "range-max": (
        "x(REAL)",
        f"x <= 10 AND x >= max(from 5 previous to previous else 0: x) {TL} - t.w",
        "MINIMIZE sum(x)",
    ),
    "cyclic-repeats": (
        "x(REAL)",
        "x <= 10 AND x <= sum(from 4 previous to 4 next: x) " "over (t.trio cyclic within t.q) - 1",
        "MAXIMIZE sum(x)",
    ),
}

#: What a run measures, each stored as its median over the runs. The counters are the model's size, the same each run.
MEASURES = ("own_ms", "peak_rss_mb", "optimizer_ms", "model_ms", "solver_ms")

COUNTERS = (
    "model.variables",
    "model.linear_rows",
    "model.linear_nonzeros",
    "anr.frame_windows",
    "anr.frame_positions",
    "anr.helpers",
    "anr.helper_instances",
    "anr.members",
    "anr.member_terms",
    "anr.inlined_helpers",
)


def run_measured(argv: list[str], script: str, env: dict[str, str], timeout: float) -> tuple[int, str, int]:
    """Run `argv` on `script`: its exit code, its stderr and its peak resident set in bytes.

    The peak is what `wait4` reports for this child alone. `getrusage(RUSAGE_CHILDREN)` will not do: it holds the
    largest of every child waited for so far, so a workload after a larger one would report the larger one's.
    `ru_maxrss` counts bytes on macOS and KiB on Linux. Stderr goes to a file, since nothing reads a pipe here.
    """
    with tempfile.TemporaryFile("w+") as stderr:
        process = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=stderr, text=True, env=env
        )
        process.stdin.write(script)
        process.stdin.close()
        deadline = time.monotonic() + timeout
        while True:
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid:
                break
            if time.monotonic() > deadline:
                process.kill()
                process.wait()
                raise subprocess.TimeoutExpired(argv, timeout)
            time.sleep(0.02)
        process.returncode = os.waitstatus_to_exitcode(status)
        stderr.seek(0)
        return process.returncode, stderr.read(), usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024)


def run(name: str, n: int) -> dict:
    decisions, constraints, objective = WORKLOADS[name]
    sql = (
        f"SELECT id, x FROM {TABLE.format(n=n)} DECIDE {decisions} " f"SUCH THAT {constraints.format(n=n)} {objective}"
    )
    # A range of fifty over a million anchors reads fifty million values, which the default
    # decide_frame_limit (ten million) refuses: what is measured here is the growth. The model
    # is what is measured, so the direct solve, which answers a flat sum without one, is off.
    script = (
        f".output /dev/null\nSET decide_frame_limit = 1000000000;\nSET decide_direct_solve = 'off';\n"
        f"CREATE TEMP TABLE result AS {sql};\n"
    )
    # The caller's DECIDB_ switches are dropped, so that a stray dump or limit does not skew a
    # measurement, except the evaluator: `DECIDB_EVALUATOR=tree <this script>` measures the tree.
    env = {
        key: value for key, value in os.environ.items() if not key.startswith("DECIDB_") or key == "DECIDB_EVALUATOR"
    }
    env.update(DECIDB_BENCH="1", DECIDB_PROFILE="1", DECIDB_TIME_LIMIT="120")
    returncode, stderr, peak_bytes = run_measured([str(DECIDB), ":memory:"], script, env, timeout=900)
    row = {"workload": name, "rows": n, "status": "ok" if returncode == 0 else "error"}
    row["peak_rss_mb"] = round(peak_bytes / 2**20, 1)
    if returncode != 0:
        row["error"] = stderr.strip().splitlines()[-1][:200] if stderr.strip() else "?"
    bench = dict(re.findall(r"DECIDB_BENCH: (\w+)=([\d.]+)", stderr))
    row["optimizer_ms"] = float(bench.get("optimizer_ms", "nan"))
    row["model_ms"] = float(bench.get("model_construction_ms", "nan"))
    row["solver_ms"] = float(bench.get("solver_ms", "nan"))
    row["own_ms"] = row["optimizer_ms"] + row["model_ms"]
    for line in stderr.splitlines():
        if line.startswith("DECIDB_PROFILE: ") and '"counter"' in line:
            event = json.loads(line.split("DECIDB_PROFILE: ", 1)[1])
            if event["name"] in COUNTERS:
                row[event["name"]] = int(event["value"])
    return row


def measure(name: str, n: int, runs: int) -> dict:
    """`runs` runs of one workload at one size, each measure their median: a phase's numbers are read against gates of
    1.5x, and one run on a loaded host moves by more. A run that fails is the row, since the rest would fail too."""
    rows = []
    for _ in range(runs):
        row = run(name, n)
        if row["status"] != "ok":
            return row
        rows.append(row)
    return {**rows[0], **{field: statistics.median(row[field] for row in rows) for field in MEASURES}, "runs": runs}


def exponent(rows: list[dict]) -> float:
    """The slope of log(own time) against log(n) between the two largest sizes."""
    good = [r for r in rows if r["status"] == "ok" and r["own_ms"] > 0]
    if len(good) < 2:
        return float("nan")
    a, b = good[-2], good[-1]
    return math.log(b["own_ms"] / a["own_ms"]) / math.log(b["rows"] / a["rows"])


def stored(path: Path) -> list[dict]:
    """The rows of a results file, none when it does not exist."""
    if not path.exists():
        return []
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def phase_path(phase: str) -> Path:
    return RESULTS / (f"anr_scaling_{phase}.csv" if phase else "anr_scaling.csv")


def report_against(rows: list[dict], baseline: list[dict], phase: str) -> None:
    """Print each row's time and peak as a multiple of the same workload and size in `baseline`, `phase`'s rows."""
    base = {(row["workload"], int(row["rows"])): row for row in baseline}
    print(f"\nagainst {phase}\n{'workload':15} {'rows':>9} {'own ms':>9} {'peak MB':>9}")
    for row in rows:
        then = base.get((row["workload"], row["rows"]))
        if then is None:
            print(f"{row['workload']:15} {row['rows']:9d}   not in {phase}")
            continue
        own, peak = row["own_ms"] / float(then["own_ms"]), row["peak_rss_mb"] / float(then["peak_rss_mb"])
        print(f"{row['workload']:15} {row['rows']:9d} {own:8.2f}x {peak:8.2f}x")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--max", type=int, default=100_000, help="largest input, a power of ten (10^5)")
    parser.add_argument("--only", default="", help="run the workloads whose name contains this")
    parser.add_argument("--phase", default="", help="store the numbers under this label, as P0")
    parser.add_argument("--against", default="", help="print them as multiples of this stored phase's")
    parser.add_argument("--runs", type=int, default=3, help="runs per workload and size, of which the median is kept")
    args = parser.parse_args()
    # Read before this run writes, which may be to the same file.
    baseline = stored(phase_path(args.against)) if args.against else []
    sizes = [10**k for k in range(3, int(round(math.log10(args.max))) + 1)]
    RESULTS.mkdir(exist_ok=True)
    out = []
    print(
        f"{'workload':15} {'rows':>9} {'own ms':>9} {'peak MB':>9} {'solve ms':>9} {'columns':>9} {'model rows':>10} "
        f"{'helpers':>7} {'inlined':>7}"
    )
    for name in WORKLOADS:
        if args.only not in name:
            continue
        rows = []
        for n in sizes:
            row = measure(name, n, args.runs)
            rows.append(row)
            print(
                f"{name:15} {n:9d} {row['own_ms']:9.1f} {row['peak_rss_mb']:9.1f} {row['solver_ms']:9.1f} "
                f"{row.get('model.variables', 0):9d} {row.get('model.linear_rows', 0):10d} "
                f"{row.get('anr.helpers', 0):7d} {row.get('anr.inlined_helpers', 0):7d}"
                + (f"  {row['error']}" if row["status"] != "ok" else ""),
                flush=True,
            )
        slope = exponent(rows)
        print(f"{name:15} growth exponent {slope:.2f}\n", flush=True)
        for row in rows:
            row["exponent"] = round(slope, 3)
            row["phase"] = args.phase
        out.extend(rows)
    fields = [
        "phase",
        "workload",
        "rows",
        "status",
        "own_ms",
        "peak_rss_mb",
        "optimizer_ms",
        "model_ms",
        "solver_ms",
        "runs",
        *COUNTERS,
        "exponent",
        "error",
    ]
    path = phase_path(args.phase)
    measured = {(row["workload"], row["rows"]) for row in out}
    kept = [row for row in stored(path) if (row["workload"], int(row["rows"])) not in measured] if args.phase else []
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(kept + out)
    print(f"-> {path}")
    if args.against:
        report_against(out, baseline, args.against)


if __name__ == "__main__":
    main()
