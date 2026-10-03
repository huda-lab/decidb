#!/usr/bin/env python3
"""Reproducible end-to-end comparison for the admitted global S1 rule.

Every run uses a fresh process and fully materializes the DECIDE output. A row
with a timeout, solver limit, or error is recorded but never counted as a win.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import re
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path

import profile_pipeline as pipeline
import run_benchmarks as bench


HERE = Path(__file__).resolve().parent
EXE = HERE.parents[1] / "build/release/decidb"
MODES = ("direct", "highs", "gurobi")
PHASES = {
    "direct_analyze_ms": ("optimizer.direct.analyze",),
    "direct_construct_ms": ("optimizer.direct.construct",),
    "model_build_ms": ("solver.build_neutral_model",),
    "backend_load_ms": ("gurobi.load", "highs.load"),
    "solve_ms": ("gurobi.optimize", "highs.optimize"),
    "solver_readback_ms": ("gurobi.status_and_readback", "highs.status_and_readback", "execution.output_readback"),
    "result_collect_append_ms": ("execution.result_collect_append",),
    "result_collect_combine_ms": ("execution.result_collect_combine",),
    "result_collect_finalize_ms": ("execution.result_collect_finalize",),
}
FIELDS = (
    "rows", "capacity", "width", "pattern", "source_kind", "mode", "repeat", "status", "query_s", "process_s",
    "peak_rss_mib", "result_rows", "chosen", "objective", "window_input_rows", "window_sort_buffer_mib", *PHASES,
    "error",
)


def source_sql(rows: int, width: int, pattern: str) -> str:
    score = (
        "(((CAST(i AS BIGINT)*37)%10007)-5000)::DOUBLE / 10.0"
        if pattern == "mixed" else "((CAST(i AS BIGINT)%17)-8)::DOUBLE"
    )
    payload = f", rpad(i::VARCHAR, {width}, 'p') AS payload" if width else ""
    return f"SELECT i, {score} AS score{payload} FROM range({rows}) t(i)"


def s1_sql(rows: int, capacity: int, width: int, pattern: str, source_kind: str) -> tuple[str, str]:
    source = source_sql(rows, width, pattern)
    if source_kind == "stored_join":
        setup = (
            f"CREATE TEMP TABLE source AS {source};\n"
            "CREATE TEMP TABLE weights AS SELECT k, 1.0 + (k % 5)::DOUBLE / 20.0 AS weight "
            "FROM range(97) t(k);"
        )
        payload = ", s.payload" if width else ""
        relation = (
            "(SELECT s.i, s.score * w.weight AS score"
            f"{payload} FROM source s JOIN weights w ON s.i % 97 = w.k) j"
        )
    elif source_kind == "stored":
        setup = f"CREATE TEMP TABLE source AS {source};"
        relation = "source"
    else:
        setup = ""
        relation = f"({source}) s"
    columns = "i, score, payload, x" if width else "i, score, x"
    query = f"""
        CREATE TEMP TABLE result AS SELECT {columns} FROM (
            FROM {relation}
            DECIDE x(BOOL) SUCH THAT SUM(x)<={capacity} MAXIMIZE SUM(score*x)
        ) q;
    """
    return setup, query


def run_one(rows: int, capacity: int, width: int, pattern: str, mode: str, repeat: int,
            timeout: int, raw_dir: Path, source_kind: str) -> dict:
    setup, sql = s1_sql(rows, capacity, width, pattern, source_kind)
    script = "\n".join([
        ".bail on", ".output /dev/null", "SET threads=4;",
        f"SET decide_direct_solve='{'require' if mode == 'direct' else 'off'}';",
        setup,
        ".output stderr", ".print DECIDB_PROFILE_QUERY_BEGIN", ".output /dev/null",
        ".timer on", sql, ".timer off", ".output stderr", ".print DECIDB_PROFILE_QUERY_END",
        ".mode json", ".output stdout",
        "SELECT COUNT(*) AS n, SUM(x) AS chosen, SUM(score*x) AS objective FROM result;", "",
    ])
    environment = {key: value for key, value in os.environ.items() if not key.startswith("DECIDB_")}
    environment["DECIDB_PROFILE"] = "1"
    if mode != "direct":
        environment["DECIDB_FORCE_SOLVER"] = mode
    name = f"n{rows}_k{capacity}_w{width}_{pattern}_{source_kind}_{mode}_r{repeat}"
    started = time.monotonic()
    status = "ok"
    try:
        completed = subprocess.run(
            ["/usr/bin/time", "-l", str(EXE), "-batch", "-init", "/dev/null"],
            input=script, capture_output=True, text=True, env=environment, timeout=timeout,
        )
        stdout, stderr, code = completed.stdout, completed.stderr, completed.returncode
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or b""
        stderr = exc.stderr or b""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        code, status = -1, "process_timeout"
    process_s = round(time.monotonic() - started, 6)
    raw_dir.mkdir(parents=True, exist_ok=True)
    (raw_dir / f"{name}.sql").write_text(script)
    (raw_dir / f"{name}.stdout").write_text(stdout)
    (raw_dir / f"{name}.stderr").write_text(stderr)
    if status == "ok" and (code != 0 or "Error:" in stderr or "Error Error" in stderr):
        status = pipeline.classify(stderr)
    profile = pipeline.parse_profile(stderr)
    resources = bench.parse_time_output(stderr)
    timings = re.findall(r"Run Time \(s\): real ([\d.]+)", stdout + stderr)
    result = re.search(r'"n"\s*:\s*(\d+).*?"chosen"\s*:\s*(\d+).*?"objective"\s*:\s*([\d.eE+-]+)',
                       stdout, re.DOTALL)
    if status == "ok" and (not timings or not result):
        status = "missing_measurement"
    if status == "ok" and (int(result.group(1)) != rows or int(result.group(2)) > capacity):
        status = "incorrect_result"
    row = {
        "rows": rows, "capacity": capacity, "width": width, "pattern": pattern, "source_kind": source_kind,
        "mode": mode,
        "repeat": repeat, "status": status, "query_s": float(timings[0]) if timings else None,
        "process_s": process_s, "peak_rss_mib": round(resources.get("peak_rss_kb", 0) / 1024, 3) or None,
        "result_rows": int(result.group(1)) if result else None,
        "chosen": int(result.group(2)) if result else None,
        "objective": float(result.group(3)) if result else None,
        "window_input_rows": round(profile["counter_totals"].get("execution.window.input_rows", 0)),
        "window_sort_buffer_mib": round(profile["counter_totals"].get("execution.window.sort_buffer_bytes", 0)
                                        / (1024 * 1024), 3),
        "error": " | ".join(line for line in stderr.splitlines() if "Error:" in line)[:500],
    }
    for field, names in PHASES.items():
        row[field] = round(sum(profile["phases"].get(name, 0.0) for name in names), 6)
    return row


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", nargs="+", type=int, default=[1000, 10000])
    parser.add_argument("--widths", nargs="+", type=int, default=[0, 512])
    parser.add_argument("--fractions", nargs="+", type=float, default=[0, 0.1])
    parser.add_argument("--patterns", nargs="+", choices=["mixed", "tied"], default=["mixed", "tied"])
    parser.add_argument("--source-kind", choices=["range", "stored", "stored_join"], default="range")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if any(n <= 0 for n in args.rows) or any(width < 0 for width in args.widths) or any(
        fraction < 0 or fraction > 1 for fraction in args.fractions
    ) or args.repeats <= 0:
        parser.error("rows and repeats must be positive; widths nonnegative; fractions in [0, 1]")
    if not EXE.exists():
        parser.error(f"build the release CLI first: {EXE}")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.output or HERE / "results" / f"direct_s1_{stamp}.csv"
    output.parent.mkdir(parents=True, exist_ok=True)
    raw_dir = output.with_suffix("")
    manifest = {
        "command": "profile_direct_s1.py", "arguments": vars(args), "executable": str(EXE),
        "platform": platform.platform(), "python": platform.python_version(), "generated_at_utc": stamp,
    }
    manifest["arguments"]["output"] = str(output)
    (output.parent / f"{output.stem}.json").write_text(json.dumps(manifest, indent=2) + "\n")
    with output.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        for repeat in range(args.repeats):
            for rows in args.rows:
                for width in args.widths:
                    for fraction in args.fractions:
                        capacity = int(rows * fraction)
                        for pattern in args.patterns:
                            for mode in MODES:
                                row = run_one(rows, capacity, width, pattern, mode, repeat, args.timeout, raw_dir,
                                              args.source_kind)
                                writer.writerow(row)
                                handle.flush()
                                print(f"{rows:>7} {capacity:>7} w={width:<4} {pattern:<5} {mode:<6} "
                                      f"{row['status']:<18} query={row['query_s']}s rss={row['peak_rss_mib']}MiB",
                                      flush=True)
    print(output)


if __name__ == "__main__":
    main()
