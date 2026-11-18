"""A/B benchmark of two decidb binaries on the ANR scaling workloads.

Unlike benchmark/decide/anr_scaling.py, this measures the whole process's wall time as well as the profile's
phases, because one evaluator does its row pass inside Sink (outside `model_construction_ms`) and the other in
Finalize (inside it). `nonsolver_ms` = wall - solver_ms is the comparable number. Runs are interleaved A, B, A, B
so drift on the host hits both alike.

Usage: ab_bench.py OUT.csv --sizes 10000,100000 --runs 5 [--only name] [--threads N] [--source range|table]
"""

import argparse
import csv
import importlib.util
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
# Label -> binary. Edit to the two builds being compared; the second is the OPA parent of the merge (f56845f560).
BINARIES = {
    "tip": REPO / "build" / "release" / "decidb",
    "opa": REPO / ".claude" / "worktrees" / "sami-opa" / "build" / "release" / "decidb",
}

# The workloads are the repository's own.
_spec = importlib.util.spec_from_file_location("anr_scaling", REPO / "benchmark" / "decide" / "anr_scaling.py")
_scaling = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_scaling)
WORKLOADS, TABLE = _scaling.WORKLOADS, _scaling.TABLE

COUNTERS = ("model.variables", "model.linear_rows", "model.linear_nonzeros", "anr.helpers", "anr.inlined_helpers")
MEASURES = ("wall_ms", "nonsolver_ms", "optimizer_ms", "model_ms", "solver_ms", "peak_rss_mb")


def run_once(binary: Path, name: str, n: int, threads: int, source: str) -> dict:
    decisions, constraints, objective = WORKLOADS[name]
    setup = ".output /dev/null\nSET decide_frame_limit = 1000000000;\nSET decide_direct_solve = 'off';\n"
    if threads:
        setup += f"SET threads = {threads};\n"
    if source == "table":
        # A stored table, so the scan is parallel and rows reach the sink from several threads.
        inner = TABLE.format(n=n).rsplit(") t", 1)[0].split("(", 1)[1]
        setup += f"CREATE TABLE src AS {inner};\n"
        relation = "src t"
    else:
        relation = TABLE.format(n=n)
    sql = f"SELECT id, x FROM {relation} DECIDE {decisions} SUCH THAT {constraints.format(n=n)} {objective}"
    script = setup + ".timer on\n" + f"CREATE TEMP TABLE result AS {sql};\n"
    env = {key: value for key, value in os.environ.items() if not key.startswith("DECIDB_")}
    env.update(DECIDB_PROFILE="1", DECIDB_TIME_LIMIT="300")
    with tempfile.TemporaryFile("w+") as stderr, tempfile.TemporaryFile("w+") as stdout:
        start = time.monotonic()
        process = subprocess.Popen(
            [str(binary), ":memory:"], stdin=subprocess.PIPE, stdout=stdout, stderr=stderr, text=True, env=env
        )
        process.stdin.write(script)
        process.stdin.close()
        _, status, usage = os.wait4(process.pid, 0)
        wall = (time.monotonic() - start) * 1000
        process.returncode = os.waitstatus_to_exitcode(status)
        stderr.seek(0)
        err = stderr.read()
    row = {"workload": name, "rows": n, "status": "ok" if process.returncode == 0 else "error"}
    if process.returncode != 0:
        row["error"] = err.strip().splitlines()[-1][:200] if err.strip() else "?"
        return row
    bench = _scaling.parse_stage_output(err)
    row["wall_ms"] = wall
    row["optimizer_ms"] = bench.get("optimizer_ms", float("nan"))
    row["model_ms"] = bench.get("model_construction_ms", float("nan"))
    row["solver_ms"] = bench.get("solver_ms", float("nan"))
    row["nonsolver_ms"] = wall - row["solver_ms"]
    row["peak_rss_mb"] = usage.ru_maxrss / 2**20
    for line in err.splitlines():
        if line.startswith("DECIDB_PROFILE: ") and '"counter"' in line:
            event = json.loads(line.split("DECIDB_PROFILE: ", 1)[1])
            if event["name"] in COUNTERS:
                row[event["name"]] = int(event["value"])
    return row


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("--sizes", default="10000,100000")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--only", default="")
    parser.add_argument("--threads", type=int, default=0)
    parser.add_argument("--source", default="range", choices=("range", "table"))
    args = parser.parse_args()
    sizes = [int(size) for size in args.sizes.split(",")]
    names = [name for name in WORKLOADS if args.only in name]
    fields = ["binary", "workload", "rows", "status", "runs", *MEASURES, *COUNTERS, "error"]
    out = []
    for name in names:
        for n in sizes:
            samples = {label: [] for label in BINARIES}
            failed = {}
            for _ in range(args.runs):
                for label, binary in BINARIES.items():
                    if label in failed:
                        continue
                    row = run_once(binary, name, n, args.threads, args.source)
                    if row["status"] != "ok":
                        failed[label] = row
                    else:
                        samples[label].append(row)
            line = f"{name:15} {n:>9}"
            for label in BINARIES:
                if label in failed or not samples[label]:
                    row = {"binary": label, **failed.get(label, {"workload": name, "rows": n, "status": "error"})}
                    line += f" | {label} ERROR {row.get('error', '')[:60]}"
                else:
                    rows = samples[label]
                    row = {"binary": label, **rows[0], "runs": len(rows)}
                    for field in MEASURES:
                        row[field] = round(statistics.median(r[field] for r in rows), 2)
                    line += (
                        f" | {label} nonsolver {row['nonsolver_ms']:8.1f} model {row['model_ms']:8.1f}"
                        f" rss {row['peak_rss_mb']:7.1f}"
                    )
                out.append(row)
            print(line, flush=True)
    with open(args.out, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(out)


if __name__ == "__main__":
    main()
