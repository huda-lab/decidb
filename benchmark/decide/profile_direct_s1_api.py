#!/usr/bin/env python3
"""Compare materialized and streamed S1 API delivery on stored inputs."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import platform
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path

import profile_pipeline as pipeline
import run_benchmarks as bench
from profile_direct_s1 import PHASES


HERE = Path(__file__).resolve().parent
DEFAULT_BINARY = HERE / "results" / "profile_direct_s1_api"
MODES = ("direct", "highs", "gurobi")
DELIVERIES = ("materialized", "stream")
CARDINALITIES = ("upper", "lower", "exact", "interval")
SCOPES = ("global", "grouped", "grouped_local_when")
PINS = ("free", "fixed")
BOUND_KINDS = ("constant", "source", "source_double", "source_pair", "source_coalesce")
FIELDS = (
    "rows", "width", "source_kind", "output_kind", "mode", "delivery", "cardinality", "scope", "pins",
    "bound_kind",
    "repeat", "status",
    "setup_s", "submit_s",
    "fetch_s", "query_collect_s", "api_readback_s", "process_s", "peak_rss_mib", "setup_rss_mib",
    "process_peak_through_setup_mib", "process_peak_through_submit_mib", "post_submit_rss_mib",
    "process_peak_through_fetch_mib", "post_fetch_rss_mib", "process_peak_through_query_mib", "post_query_rss_mib",
    "result_rows", "output_rows", "chosen", "active_chosen", "objective", "payload_bytes", "window_input_rows",
    "window_sort_buffer_mib", *PHASES, "error",
)


def run_one(binary: Path, rows: int, width: int, source_kind: str, output_kind: str, mode: str,
            delivery: str, cardinality: str, scope: str, pins: str, bound_kind: str,
            repeat: int, timeout: int, raw_dir: Path) -> dict:
    environment = {key: value for key, value in os.environ.items() if not key.startswith("DECIDB_")}
    environment["DECIDB_PROFILE"] = "1"
    if mode != "direct":
        environment["DECIDB_FORCE_SOLVER"] = mode
    name = (f"n{rows}_w{width}_{source_kind}_{output_kind}_{mode}_{delivery}_{cardinality}_{scope}_"
            f"{pins}_{bound_kind}_r{repeat}")
    status = "ok"
    started = time.monotonic()
    try:
        completed = subprocess.run(
            ["/usr/bin/time", "-l", str(binary), str(rows), str(width), source_kind, mode, output_kind, delivery,
             cardinality, scope, pins, bound_kind],
            capture_output=True, text=True, env=environment, timeout=timeout,
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
    (raw_dir / f"{name}.stdout").write_text(stdout)
    (raw_dir / f"{name}.stderr").write_text(stderr)
    if status == "ok" and code != 0:
        status = pipeline.classify(stderr)
    try:
        result = json.loads(stdout.strip().splitlines()[-1]) if stdout.strip() else None
    except (json.JSONDecodeError, IndexError):
        result = None
    if status == "ok" and not result:
        status = "missing_measurement"
    group_size = rows // 100 if scope != "global" else rows
    group_count = 100 if scope != "global" else 1
    lower = (group_size // 10) * 6 * group_count
    upper = (group_size // 10) * 7 * group_count
    checked_chosen = result["active_chosen"] if result and scope == "grouped_local_when" else result["chosen"] if result else 0
    valid_cardinality = result and (
        checked_chosen <= (group_size // 10) * group_count if cardinality == "upper" else
        lower <= checked_chosen <= upper if cardinality == "interval" else
        checked_chosen == lower if cardinality == "exact" else checked_chosen >= lower
    )
    if status == "ok" and (
        result["result_rows"] != rows or result["output_rows"] != (rows if output_kind == "full" else 1)
        or not valid_cardinality or not math.isfinite(result["objective"])
        or result.get("delivery") != delivery or result.get("cardinality") != cardinality
        or result.get("scope") != scope or result.get("pins") != pins or result.get("bound_kind") != bound_kind
    ):
        status = "incorrect_result"
    profile = pipeline.parse_profile(stderr)
    resources = bench.parse_time_output(stderr)
    row = {
        "rows": rows, "width": width, "source_kind": source_kind, "output_kind": output_kind,
        "mode": mode, "delivery": delivery, "cardinality": cardinality, "scope": scope, "pins": pins,
        "bound_kind": bound_kind,
        "repeat": repeat,
        "status": status, "setup_s": result.get("setup_s") if result else None,
        "submit_s": result.get("submit_s") if result else None,
        "fetch_s": result.get("fetch_s") if result else None,
        "query_collect_s": result.get("query_collect_s") if result else None,
        "api_readback_s": result.get("api_readback_s") if result else None,
        "process_s": process_s,
        "peak_rss_mib": round(resources.get("peak_rss_kb", 0) / 1024, 3) or None,
        "setup_rss_mib": result.get("setup_rss_mib") if result else None,
        "process_peak_through_setup_mib": result.get("process_peak_through_setup_mib") if result else None,
        "process_peak_through_submit_mib": result.get("process_peak_through_submit_mib") if result else None,
        "post_submit_rss_mib": result.get("post_submit_rss_mib") if result else None,
        "process_peak_through_fetch_mib": result.get("process_peak_through_fetch_mib") if result else None,
        "post_fetch_rss_mib": result.get("post_fetch_rss_mib") if result else None,
        "process_peak_through_query_mib": result.get("process_peak_through_query_mib") if result else None,
        "post_query_rss_mib": result.get("post_query_rss_mib") if result else None,
        "result_rows": result.get("result_rows") if result else None,
        "output_rows": result.get("output_rows") if result else None,
        "chosen": result.get("chosen") if result else None,
        "active_chosen": result.get("active_chosen") if result else None,
        "objective": result.get("objective") if result else None,
        "payload_bytes": result.get("payload_bytes") if result else None,
        "window_input_rows": round(profile["counter_totals"].get("execution.window.input_rows", 0)),
        "window_sort_buffer_mib": round(profile["counter_totals"].get("execution.window.sort_buffer_bytes", 0)
                                        / (1024 * 1024), 3),
        "error": (stderr[-500:] if status != "ok" else ""),
    }
    for field, names in PHASES.items():
        row[field] = round(sum(profile["phases"].get(name, 0.0) for name in names), 6)
    return row


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", nargs="+", type=int, default=[1000000])
    parser.add_argument("--widths", nargs="+", type=int, default=[0])
    parser.add_argument("--source-kinds", nargs="+", choices=["stored", "stored_join"], default=["stored"])
    parser.add_argument("--output-kinds", nargs="+", choices=["full", "aggregate"], default=["full"])
    parser.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES))
    parser.add_argument("--deliveries", nargs="+", choices=DELIVERIES, default=["materialized"])
    parser.add_argument("--cardinalities", nargs="+", choices=CARDINALITIES, default=["upper"])
    parser.add_argument("--scopes", nargs="+", choices=SCOPES, default=["global"])
    parser.add_argument("--pins", nargs="+", choices=PINS, default=["free"])
    parser.add_argument("--bound-kinds", nargs="+", choices=BOUND_KINDS, default=["constant"])
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if any(n <= 0 for n in args.rows) or any(width < 0 for width in args.widths) or args.repeats <= 0:
        parser.error("rows and repeats must be positive; widths nonnegative")
    if any(scope != "global" for scope in args.scopes) and any(n < 100 for n in args.rows):
        parser.error("grouped benchmarks need at least 100 rows")
    if any(kind != "constant" for kind in args.bound_kinds) and args.cardinalities != ["interval"]:
        parser.error("source-valued bounds need only --cardinalities interval")
    if not args.binary.exists():
        parser.error(f"build the API benchmark first: {args.binary}")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.output or HERE / "results" / f"direct_s1_api_{stamp}.csv"
    output.parent.mkdir(parents=True, exist_ok=True)
    raw_dir = output.with_suffix("")
    manifest = {
        "command": "profile_direct_s1_api.py", "arguments": vars(args), "binary": str(args.binary),
        "platform": platform.platform(), "python": platform.python_version(), "generated_at_utc": stamp,
    }
    manifest["arguments"]["binary"] = str(args.binary)
    manifest["arguments"]["output"] = str(output)
    (output.parent / f"{output.stem}.json").write_text(json.dumps(manifest, indent=2) + "\n")
    with output.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        for repeat in range(args.repeats):
            for rows in args.rows:
                for width in args.widths:
                    for source_kind in args.source_kinds:
                        for output_kind in args.output_kinds:
                            for mode in args.modes:
                                for delivery in args.deliveries:
                                    for cardinality in args.cardinalities:
                                        for scope in args.scopes:
                                            for pins in args.pins:
                                                for bound_kind in args.bound_kinds:
                                                    row = run_one(args.binary, rows, width, source_kind, output_kind,
                                                                  mode, delivery, cardinality, scope, pins, bound_kind,
                                                                  repeat, args.timeout, raw_dir)
                                                    writer.writerow(row)
                                                    handle.flush()
                                                    print(f"{rows:>7} w={width:<4} {source_kind:<11} {output_kind:<9} "
                                                          f"{mode:<6} {delivery:<12} {cardinality:<8} {scope:<7} "
                                                          f"{pins:<5} {bound_kind:<8} {row['status']:<18} "
                                                          f"submit={row['submit_s']}s fetch={row['fetch_s']}s",
                                                          flush=True)
    print(output)


if __name__ == "__main__":
    main()
