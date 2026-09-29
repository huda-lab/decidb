#!/usr/bin/env python3
"""Run every DECIDE benchmark query on each backend and record where the time goes.

One row per (query, backend, repeat) in results/pipeline_profile.csv. No guards, no
caps, no early stopping: a run is measured and written down, whatever it does.
"""

from __future__ import annotations

import csv
import json
import os
import re
import subprocess
from collections import defaultdict
from pathlib import Path

import run_benchmarks as bench

# ---------------------------------------------------------------------------
# Every constant of this experiment.
DATABASE = "large"        # benchmark/decide/databases/{DATABASE}.db
BACKENDS = ["gurobi", "highs"]
REPEATS = 3
SOLVER_SECONDS = 300      # solver's own limit; reaching it is a result, not a failure
PROCESS_SECONDS = 420     # hard stop so one query cannot hang the sweep
# ---------------------------------------------------------------------------

HERE = Path(__file__).resolve().parent
RESULTS = HERE / "results"
CSV_PATH = RESULTS / f"pipeline_profile_{DATABASE}.csv"
RAW = RESULTS / f"pipeline_profile_raw_{DATABASE}"

# Reproduced twice: Q10/HiGHS at large OOM-kills the whole process from outside (no python
# exception to catch -- the OS/harness kills it), so it can't be handled in run_one. Recorded
# as a bug (06_issues/bugs/todo.md); skipped here so the rest of the sweep can finish.
SKIP = {("Q10", "highs")}

# Each stage names parent spans only, so no time is counted twice.
STAGES = {
    "parse_bind": ["frontend.parse", "frontend.bind_select"],
    "canonicalize": ["canonicalize.comparison", "canonicalize.objective"],
    "rewrite": ["optimizer.decide"],
    "plan": ["frontend.physical_decide"],
    "input": ["execution.assemble_input", "execution.sink_append", "execution.sink_combine"],
    "extraction": ["execution.evaluate_constraints", "execution.evaluate_objective",
                   "execution.evaluate_composed", "execution.entity_mapping", "execution.formulate"],
    "model_build": ["solver.build_neutral_model"],
    "backend_load": ["gurobi.load", "highs.load"],
    "solve": ["gurobi.optimize", "highs.optimize"],
    "readback": ["gurobi.status_and_readback", "highs.status_and_readback",
                 "execution.output_readback", "solver.postsolve", "solver.cleanup"],
}

# Gurobi and HiGHS spell these differently; take whichever the run emitted.
COUNTERS = {
    "variables": ["model.variables"],
    "linear_rows": ["model.linear_rows"],
    "linear_nonzeros": ["model.linear_nonzeros"],
    "quadratic_rows": ["model.quadratic_rows"],
    "objective": ["ObjVal", "highs.objective"],
    "iterations": ["IterCount", "highs.simplex_iterations"],
    "nodes": ["NodeCount", "highs.mip_nodes"],
    "mip_gap": ["MIPGap"],
}

COLUMNS = (["query", "backend", "repeat", "status", "rows", "query_s", "non_solver_ms"]
           + [f"{stage}_ms" for stage in STAGES] + list(COUNTERS) + ["peak_rss_mib"])  # solve_ms comes from STAGES


def discover() -> dict[str, Path]:
    """Every q*.sql and p*.sql in queries/, ordered Q1..Q11 then P1..P4."""
    found = {}
    for path in sorted((HERE / "queries").glob("[qp]*.sql")):
        match = re.match(r"([qp])(\d+)", path.stem)
        if match and not path.name.endswith(".example"):
            found[f"{match.group(1).upper()}{match.group(2)}"] = path
    return dict(sorted(found.items(), key=lambda item: ("QP".index(item[0][0]), int(item[0][1:]))))


def parse_profile(stderr: str) -> dict:
    """Collapse the nested spans into inclusive totals per span name."""
    if "DECIDB_PROFILE_QUERY_BEGIN\n" in stderr:
        stderr = stderr.split("DECIDB_PROFILE_QUERY_BEGIN\n", 1)[1].split("DECIDB_PROFILE_QUERY_END\n", 1)[0]
    events = [json.loads(line.split("DECIDB_PROFILE: ", 1)[1]) for line in stderr.splitlines()
              if line.startswith("DECIDB_PROFILE: ")]
    phases = defaultdict(float)
    for event in events:
        # "end" is one nested span; "aggregate" is a pre-summed total across many calls
        # (sink_append, output_readback) where the engine doesn't log every chunk. Both
        # carry a plain duration_ms for this name, so both add straight into the phase.
        if event["event"] in ("end", "aggregate"):
            phases[event["name"]] += event["duration_ms"]
    counters = {event["name"]: event["value"] for event in events if event["event"] == "counter"}
    return {"phases": dict(phases), "counters": counters}


def classify(stderr: str) -> str:
    """Why a statement failed: a backend that cannot express the model, or one that ran out of time."""
    if re.search(r"needs Gurobi|requires Gurobi|does not support|cannot solve", stderr, re.I):
        return "unsupported"
    if re.search(r"hit the time limit|time limit|timed out", stderr, re.I):
        return "solver_limit"
    return "error"


def run_one(name: str, path: Path, backend: str, repeat: int) -> dict:
    sql, _ = bench.resolve_query_sql(path.read_text(), DATABASE)
    output = RAW / f"{name}_{backend}_{repeat}"
    output.mkdir(parents=True, exist_ok=True)
    count_file = output / "rows.json"
    script = "\n".join([
        ".bail on", ".output /dev/null",
        ".output stderr", ".print DECIDB_PROFILE_QUERY_BEGIN", ".output /dev/null",
        ".timer on", f"CREATE TEMP TABLE result AS {sql.strip().rstrip(';')};", ".timer off",
        ".output stderr", ".print DECIDB_PROFILE_QUERY_END", ".output /dev/null",
        ".mode json", f".output {count_file}", "SELECT count(*) AS n FROM result;", "",
    ])
    environment = {key: value for key, value in os.environ.items() if not key.startswith("DECIDB_")}
    environment.update(DECIDB_FORCE_SOLVER=backend, DECIDB_TIME_LIMIT=str(SOLVER_SECONDS),
                       DECIDB_PROFILE="1", DECIDB_BENCH="1")
    database = str(bench.DATABASES_DIR / f"{DATABASE}.db")
    status = "ok"
    try:
        done = subprocess.run(["/usr/bin/time", "-l", str(bench.DECIDB_EXE), database, "-readonly"],
                              input=script, capture_output=True, text=True,
                              env=environment, timeout=PROCESS_SECONDS)
        stdout, stderr, code = done.stdout, done.stderr, done.returncode
    except subprocess.TimeoutExpired as expired:
        # Each stream decodes independently -- one can be real bytes while the other is the
        # `None` -> "" fallback, so gating both on stdout's type left stderr undecoded.
        stdout = expired.stdout or b""
        stderr = expired.stderr or b""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        code, status = -1, "process_timeout"
    (output / "stderr.log").write_text(stderr)
    (output / "stdout.log").write_text(stdout)

    if status == "ok" and code != 0:
        # A capability refusal and a solve that ran out of time both surface as a failed
        # statement, so the message decides which; neither is an error in the run itself.
        status = classify(stderr)

    profile = parse_profile(stderr)
    (output / "profile.json").write_text(json.dumps(profile, indent=2) + "\n")
    timings = re.findall(r"Run Time \(s\): real ([\d.]+)", stdout)
    resources = bench.parse_time_output(stderr)
    rows = json.loads(count_file.read_text())[0]["n"] if count_file.exists() and count_file.stat().st_size else None

    row = {"query": name, "backend": backend, "repeat": repeat, "status": status, "rows": rows,
           "query_s": float(timings[0]) if timings else None,
           "peak_rss_mib": round(resources.get("peak_rss_kb", 0) / 1024, 1) or None}
    for stage, spans in STAGES.items():
        total = sum(profile["phases"].get(span, 0.0) for span in spans)
        row[f"{stage}_ms"] = round(total, 3) if total else None
    if row["query_s"] is not None and row["solve_ms"] is not None:
        row["non_solver_ms"] = round(row["query_s"] * 1000 - row["solve_ms"], 3)
    else:
        row["non_solver_ms"] = None
    for column, names in COUNTERS.items():
        values = [profile["counters"][key] for key in names if key in profile["counters"]]
        row[column] = values[0] if values else None
    return row


def main() -> None:
    RESULTS.mkdir(parents=True, exist_ok=True)
    queries = discover()
    print(f"{len(queries)} queries x {len(BACKENDS)} backends x {REPEATS} repeats on {DATABASE}.db")
    # Resume rather than repeat: a run already on disk (including a solved-but-crashed-writer
    # case) is loaded as-is instead of paying for it again, which matters once single runs cost
    # up to PROCESS_SECONDS.
    rows = list(csv.DictReader(CSV_PATH.open())) if CSV_PATH.exists() else []
    done = {(row["query"], row["backend"], int(row["repeat"])) for row in rows}
    for repeat in range(REPEATS):
        for name, path in queries.items():
            for backend in BACKENDS:
                if (name, backend, repeat) in done:
                    continue
                if (name, backend) in SKIP:
                    row = {"query": name, "backend": backend, "repeat": repeat, "status": "skipped_memory"}
                    print(f"  pass{repeat} {name:4} {backend:7} skipped_memory (see SKIP)")
                else:
                    print(f"  pass{repeat} {name:4} {backend:7}", end=" ", flush=True)
                    row = run_one(name, path, backend, repeat)
                    print(f"{row['status']:15} {row['query_s'] or 0:8.3f}s  solve={row['solve_ms'] or 0:10.1f}ms")
                rows.append(row)
                with CSV_PATH.open("w", newline="") as handle:
                    writer = csv.DictWriter(handle, fieldnames=COLUMNS)
                    writer.writeheader()
                    writer.writerows(rows)
    print(f"\n{len(rows)} runs -> {CSV_PATH}")


if __name__ == "__main__":
    main()
