#!/usr/bin/env python3
"""Big-M tightening experiment harness.

Runs Big-M-sensitive DECIDE queries against a TPC-H database, capturing per-stage
timing (via DECIDB_BENCH) and the realized objective value.

The objective value is the correctness guard: a VALID tightening of Big-M must
NOT change the optimum. If the objective differs between two builds for the same
query, either the tighter M was invalid (cut feasible points -> smaller optimum)
or the legacy M was invalid (capped the aggregate -> smaller optimum). The
harness recomputes the objective from the returned decision columns so it is
independent of solver-reported bounds.

Usage:
    python3 benchmark/decide/bigm_experiment.py [--db PATH] [--iters N]
            [--queries q10,q11] [--label NAME]
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
QUERIES_DIR = SCRIPT_DIR / "queries"
DECIDB_EXE = REPO_ROOT / "build" / "release" / "decidb"
DEFAULT_DB = REPO_ROOT / "decidb.db"

# query_name -> per-row objective contribution as a function of the CSV row dict.
OBJECTIVES = {
    "q10": lambda r: float(r["x"]) * float(r["l_extendedprice"]),
    "q11": lambda r: float(r["x"]) * float(r["l_extendedprice"]),
}

BENCH_RE = re.compile(r"DECIDB_BENCH:\s*(\w+)=([0-9.eE+-]+)")


def query_file(name: str) -> Path:
    matches = sorted(QUERIES_DIR.glob(f"{name}_*.sql"))
    if not matches:
        raise FileNotFoundError(f"no query file for {name} in {QUERIES_DIR}")
    return matches[0]


def run_once(db: Path, sql: str) -> tuple[str, dict]:
    env = dict(os.environ)
    env["DECIDB_BENCH"] = "1"
    proc = subprocess.run(
        [str(DECIDB_EXE), str(db), "-readonly", "-csv", "-c", sql],
        capture_output=True, text=True, env=env, timeout=1800,
    )
    if proc.returncode != 0:
        sys.exit(f"query failed:\n{proc.stderr}")
    stages = {k: float(v) for k, v in BENCH_RE.findall(proc.stderr)}
    return proc.stdout, stages


def objective(name: str, csv_text: str) -> float:
    fn = OBJECTIVES[name]
    reader = csv.DictReader(io.StringIO(csv_text))
    return sum(fn(row) for row in reader)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", type=Path, default=DEFAULT_DB)
    ap.add_argument("--iters", type=int, default=3)
    ap.add_argument("--queries", type=str, default="q10,q11")
    ap.add_argument("--label", type=str, default="run")
    args = ap.parse_args()

    names = [q.strip() for q in args.queries.split(",") if q.strip()]
    results = []
    print(f"label={args.label}  db={args.db}  iters={args.iters}\n")
    header = f"{'query':6} {'solver_ms':>10} {'model_ms':>9} {'vars':>9} {'rows':>9} {'objective':>18}"
    print(header)
    print("-" * len(header))
    for name in names:
        sql = query_file(name).read_text()
        solver_ms, model_ms, vars_, cons, specs, obj = [], [], None, None, None, None
        for _ in range(args.iters):
            out, stages = run_once(args.db, sql)
            solver_ms.append(stages.get("solver_ms", float("nan")))
            model_ms.append(stages.get("model_construction_ms", float("nan")))
            vars_ = int(stages.get("total_variables", 0))
            # Matrix rows as built, not the clause-spec count: a Big-M reformulation is
            # judged on the rows it adds, and the spec count cannot see them.
            cons = int(stages.get("total_constraint_rows", 0))
            specs = int(stages.get("total_constraint_specs", 0))
            if obj is None:
                obj = objective(name, out)
        entry = {
            "query": name,
            "median_solver_ms": statistics.median(solver_ms),
            "median_model_ms": statistics.median(model_ms),
            "total_variables": vars_,
            "total_constraint_rows": cons,
            "total_constraint_specs": specs,
            "objective": obj,
            "solver_ms_runs": solver_ms,
        }
        results.append(entry)
        print(f"{name:6} {entry['median_solver_ms']:>10.1f} "
              f"{entry['median_model_ms']:>9.1f} {vars_:>9} {cons:>9} {obj:>18.2f}")

    out_path = SCRIPT_DIR / "results" / f"bigm_{args.label}.json"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(
        {"label": args.label, "db": str(args.db), "iters": args.iters, "results": results},
        indent=2))
    print(f"\nwrote {out_path}")


if __name__ == "__main__":
    main()
