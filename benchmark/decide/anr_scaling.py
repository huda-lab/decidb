#!/usr/bin/env python3
"""How DeciDB's own time grows with the input for the ANR workloads (anr plan §6.10).

Each workload is one DECIDE query over a synthetic table of `n` rows, run at several `n`
with the key cardinalities, nesting depth, variant count and frame width held fixed, so
that every helper, window and member grows with `n` alone. DeciDB's own time is the
optimizer's and the model's construction (`DECIDB_BENCH`), the solve left out; the model's
size and the ANR counters (`DECIDB_PROFILE`: anr.helpers, anr.members, ...) are recorded
beside it. A workload grows linearly when its time's exponent -- the slope of log(time)
against log(n) between the two largest sizes -- stays near 1.

    python3 benchmark/decide/anr_scaling.py                  # 10^3 .. 10^5 rows
    python3 benchmark/decide/anr_scaling.py --max 1000000    # up to 10^6
    python3 benchmark/decide/anr_scaling.py --only range     # workloads whose name matches

Writes results/anr_scaling.csv (the results directory is not committed).
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import subprocess
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
    "nested-1": ("x(REAL)", "x <= 10 AND per t.g1: sum(max(x) by (t.g1, t.g2)) by (t.g1) <= 50",
                 "MAXIMIZE sum(x)"),
    "nested-2": ("x(REAL)", "x <= 10 AND per t.g1: sum(max(sum(x) by (t.g1, t.g2, t.g3)) by (t.g1, t.g2)) "
                 "by (t.g1) <= {n} / 50", "MAXIMIZE sum(x)"),
    "nested-3": ("x(REAL)", "x <= 10 AND per t.g1: sum(max(sum(max(x) by (t.g1, t.g2, t.g3, t.g4)) "
                 "by (t.g1, t.g2, t.g3)) by (t.g1, t.g2)) by (t.g1) <= {n} / 50", "MAXIMIZE sum(x)"),
    # `t.v * s` beside the reducer varies inside each class: ten variants per class (D64).
    "variants-10": ("x(REAL), per (): s(REAL)", "x <= 10 AND s <= 5 AND per t.g1: sum(x) by (t.g1) + t.v * s "
                    "<= {n} / 20", "MAXIMIZE sum(x) + sum(per (): s)"),
    "point": ("x(REAL)", f"x <= 10 AND x <= at(previous else 0: x) {TL} + t.w", "MAXIMIZE sum(x)"),
    "range-w1": ("x(REAL)", f"x <= 10 AND x <= sum(from previous to previous else 0: x) {TL} + t.w",
                 "MAXIMIZE sum(x)"),
    "range-w5": ("x(REAL)", f"x <= 10 AND x <= avg(from 5 previous to previous else 0: x) {TL} + t.w",
                 "MAXIMIZE sum(x)"),
    "range-w50": ("x(REAL)", f"x <= 10 AND x <= avg(from 50 previous to previous else 0: x) {TL} + t.w",
                  "MAXIMIZE sum(x)"),
    "range-step7": ("x(REAL)", f"x <= 10 AND x <= avg(from 49 previous to previous every 7 else 0: x) {TL} + t.w",
                    "MAXIMIZE sum(x)"),
    "range-max": ("x(REAL)", f"x <= 10 AND x >= max(from 5 previous to previous else 0: x) {TL} - t.w",
                  "MINIMIZE sum(x)"),
    "cyclic-repeats": ("x(REAL)", "x <= 10 AND x <= sum(from 4 previous to 4 next: x) "
                       "over (t.trio cyclic within t.q) - 1", "MAXIMIZE sum(x)"),
}

COUNTERS = ("model.variables", "model.linear_rows", "model.linear_nonzeros", "anr.frame_windows",
            "anr.frame_positions", "anr.helpers", "anr.helper_instances", "anr.members", "anr.member_terms",
            "anr.inlined_helpers")


def run(name: str, n: int) -> dict:
    decisions, constraints, objective = WORKLOADS[name]
    sql = (f"SELECT id, x FROM {TABLE.format(n=n)} DECIDE {decisions} "
           f"SUCH THAT {constraints.format(n=n)} {objective}")
    # A range of fifty over a million anchors reads fifty million values, which the default
    # decide_frame_limit (ten million) refuses: what is measured here is the growth.
    script = f".output /dev/null\nSET decide_frame_limit = 1000000000;\nCREATE TEMP TABLE result AS {sql};\n"
    env = {key: value for key, value in os.environ.items() if not key.startswith("DECIDB_")}
    env.update(DECIDB_BENCH="1", DECIDB_PROFILE="1", DECIDB_TIME_LIMIT="120")
    done = subprocess.run([str(DECIDB), ":memory:"], input=script, capture_output=True, text=True, env=env,
                          timeout=900)
    row = {"workload": name, "rows": n, "status": "ok" if done.returncode == 0 else "error"}
    if done.returncode != 0:
        row["error"] = done.stderr.strip().splitlines()[-1][:200] if done.stderr.strip() else "?"
    bench = dict(re.findall(r"DECIDB_BENCH: (\w+)=([\d.]+)", done.stderr))
    row["optimizer_ms"] = float(bench.get("optimizer_ms", "nan"))
    row["model_ms"] = float(bench.get("model_construction_ms", "nan"))
    row["solver_ms"] = float(bench.get("solver_ms", "nan"))
    row["own_ms"] = row["optimizer_ms"] + row["model_ms"]
    for line in done.stderr.splitlines():
        if line.startswith("DECIDB_PROFILE: ") and '"counter"' in line:
            event = json.loads(line.split("DECIDB_PROFILE: ", 1)[1])
            if event["name"] in COUNTERS:
                row[event["name"]] = int(event["value"])
    return row


def exponent(rows: list[dict]) -> float:
    """The slope of log(own time) against log(n) between the two largest sizes."""
    good = [r for r in rows if r["status"] == "ok" and r["own_ms"] > 0]
    if len(good) < 2:
        return float("nan")
    a, b = good[-2], good[-1]
    return math.log(b["own_ms"] / a["own_ms"]) / math.log(b["rows"] / a["rows"])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--max", type=int, default=100_000, help="largest input, a power of ten (10^5)")
    parser.add_argument("--only", default="", help="run the workloads whose name contains this")
    args = parser.parse_args()
    sizes = [10 ** k for k in range(3, int(round(math.log10(args.max))) + 1)]
    RESULTS.mkdir(exist_ok=True)
    out = []
    print(f"{'workload':15} {'rows':>9} {'own ms':>9} {'solve ms':>9} {'columns':>9} {'model rows':>10} "
          f"{'helpers':>7} {'inlined':>7}")
    for name in WORKLOADS:
        if args.only not in name:
            continue
        rows = []
        for n in sizes:
            row = run(name, n)
            rows.append(row)
            print(f"{name:15} {n:9d} {row['own_ms']:9.1f} {row['solver_ms']:9.1f} "
                  f"{row.get('model.variables', 0):9d} {row.get('model.linear_rows', 0):10d} "
                  f"{row.get('anr.helpers', 0):7d} {row.get('anr.inlined_helpers', 0):7d}"
                  + (f"  {row['error']}" if row["status"] != "ok" else ""), flush=True)
        slope = exponent(rows)
        print(f"{name:15} growth exponent {slope:.2f}\n", flush=True)
        for row in rows:
            row["exponent"] = round(slope, 3)
        out.extend(rows)
    fields = ["workload", "rows", "status", "own_ms", "optimizer_ms", "model_ms", "solver_ms", *COUNTERS,
              "exponent", "error"]
    with (RESULTS / "anr_scaling.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(out)
    print(f"-> {RESULTS / 'anr_scaling.csv'}")


if __name__ == "__main__":
    main()
