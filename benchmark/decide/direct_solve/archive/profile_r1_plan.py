#!/usr/bin/env python3
"""Profiles the direct plan of R1 operator by operator, and how its time grows with the width of the source (R1-13).

It runs the direct plan (`decide_direct_solve='require'`) of each shape in a fresh `decidb` process under
`/usr/bin/time -l` (the run pattern of `profile_r1_floor.py`, imported), with DuckDB's JSON profiler on in detailed
mode:

    PRAGMA enable_profiling='json'; PRAGMA profiling_output='<file>'; PRAGMA profiling_mode='detailed';

and reads the profile back. Per (shape, rows, source variant, threads) it records:

  query     latency, cpu_time (CPU seconds summed over threads), CPU / latency, blocked thread time, rows returned,
            rows scanned, the process's wall time, user and system time and peak resident set size (`/usr/bin/time`)
  phases    the profiler's planner (which holds the binder), physical planner and optimizer timings, each optimizer pass
            and the DECIDE pass (`optimizer_decide`); "plan + optimize" is planner + physical planner + all optimizers.
            Parsing is not recorded by the profiler. "execution" is latency minus plan + optimize (derived).
  spill     the largest size seen in the run's temporary directory, sampled every 20 ms (a fresh directory per run,
            set with `SET temp_directory`). A file that lives less than one sample can be missed.
  operators the plan tree in order: type, CPU seconds (`operator_timing`, summed over threads), share of the operators'
            CPU, rows out, `result_set_size` (the operator's output in MiB, rows times row width), the columns it
            outputs and, for a window, how many of them are source columns passed through
  plan      counts of WINDOW, ORDER BY, windows that sort, hash aggregates, unnest, projections, joins and CTEs, the
            number of blocking operators, and the columns each window outputs

The profiler holds no per-operator memory and no per-operator thread count in this build (`SYSTEM_PEAK_BUFFER_MEMORY`
and `SYSTEM_PEAK_TEMP_DIR_SIZE` are rejected as profiler settings). What stands in for them: `result_set_size` per
operator (size of its output), peak resident set size and sampled temp-directory size per query, and CPU / latency for
the cores kept busy. Window outputs are derived from the profile: `#k` entries of a projection point at the child's
column k, a table scan lists the source columns it reads, and a window outputs its child's columns plus one per window
expression. A column below an operator the script cannot see through (join, unnest, aggregate) shows as `?`.

Source variants (`le_positive` only; other shapes always use `stored`), all named `src` so the query is unchanged:

  stored  the table of `--source-dir/r1_<rows>.db` as it is (13 columns); the file is opened read-only, never copied
  narrow  only the columns the `le_positive` DECIDE clause names (`w_pos`, `v`, `cap`)
  wide    stored plus 20 DOUBLE columns `pad_0` .. `pad_19` with the deterministic values ((id * (k + 1)) % 101)

narrow and wide are built once into `--db-dir/r1_<rows>_<variant>.db` from the stored tables (attached read-only), with
the stored budgets copied beside them. The source databases are never modified; a missing one is an error.

A run over `--timeout` is killed with its whole process group and recorded as `timeout`; a failed run is recorded as
`error`. Neither is ever reported as a time. `--repeats` runs a cell more than once; the operator profile is the first
run's, latency / wall / peak memory are reported as min / median / max over all finished runs.

    python3 benchmark/decide/direct_solve/archive/profile_r1_plan.py --rows 100000 --source-dir build/r1db_smoke --db-dir build/r1db_plan_smoke
"""

import argparse
import json
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from profile_direct_r1 import BINARY, SHAPES, budgets_path, query_sql
from profile_r1_floor import run_once

DEFAULT_SHAPES = ["le_positive", "per_groups", "when_budget", "count_only", "two_decisions"]
VARIANTS = ("stored", "narrow", "wide")
WIDE_PADS = 20
SAMPLE_SECONDS = 0.02
MIB = 1024 * 1024
BLOCKING = ("WINDOW", "ORDER_BY", "HASH_GROUP_BY", "PERFECT_HASH_GROUP_BY", "UNGROUPED_AGGREGATE")
PHASES = ("planner", "planner_binding", "physical_planner", "all_optimizers", "optimizer_decide")


def cli(db, sql):
    """Runs SQL in a fresh decidb process on `db` (created if missing) and returns stdout; raises on failure."""
    return subprocess.run(
        [str(BINARY), str(db), "-noheader", "-csv", "-c", sql], text=True, check=True, capture_output=True
    ).stdout


def source_columns(db):
    out = cli(
        db, "SELECT column_name FROM information_schema.columns WHERE table_name='src' ORDER BY ordinal_position;"
    )
    return out.split()


def narrow_columns(source_db, columns):
    """The source columns the le_positive DECIDE clause names, in table order."""
    clause = SHAPES["le_positive"][0].format(**json.loads(budgets_path(source_db).read_text()))
    return [c for c in columns if re.search(rf"\b{c}\b", clause)]


def variant_select(source_db, variant, columns):
    if variant == "narrow":
        return ", ".join(narrow_columns(source_db, columns))
    pads = ", ".join(f"((id * {k + 1}) % 101)::DOUBLE AS pad_{k}" for k in range(WIDE_PADS))
    return "*, " + pads


def variant_db(source_db, db_dir, rows, variant):
    """The database of a variant: the stored one itself, or a copy built once under `db_dir`."""
    if variant == "stored":
        return source_db
    db = db_dir / f"r1_{rows}_{variant}.db"
    if db.exists() and budgets_path(db).exists():
        return db
    db.unlink(missing_ok=True)
    building = db.with_name(db.name + ".building")
    building.unlink(missing_ok=True)
    select = variant_select(source_db, variant, source_columns(source_db))
    cli(building, f"ATTACH '{source_db}' AS s (READ_ONLY); CREATE TABLE src AS SELECT {select} FROM s.src; CHECKPOINT;")
    building.rename(db)
    shutil.copyfile(budgets_path(source_db), budgets_path(db))
    return db


def default_threads():
    return int(cli(":memory:", "SELECT current_setting('threads');").strip())


class DirSampler(threading.Thread):
    """Polls a directory tree and keeps the largest total size of its files."""

    def __init__(self, path):
        super().__init__(daemon=True)
        self.path, self.peak, self.files, self.stop_flag = path, 0, 0, threading.Event()

    def sample(self):
        total = count = 0
        for p in self.path.rglob("*"):
            try:
                if p.is_file():
                    total += p.stat().st_size
                    count += 1
            except OSError:
                pass  # the file went away between listing and stat
        self.peak, self.files = max(self.peak, total), max(self.files, count)

    def run(self):
        while not self.stop_flag.is_set():
            self.sample()
            self.stop_flag.wait(SAMPLE_SECONDS)

    def finish(self):
        self.stop_flag.set()
        self.join()
        self.sample()


def listify(value):
    return [] if value is None else (value if isinstance(value, list) else [value])


def as_columns(node):
    """The output columns a node can name from its `Projections`; None when the profile does not list them."""
    info = node.get("extra_info", {})
    if node["operator_type"] in ("PROJECTION", "TABLE_SCAN") and "Projections" in info:
        return listify(info["Projections"])
    return None


def label_columns(node, source_names, widths):
    """Annotates `node` and its children with `labels`, the column names the node outputs (`?` when unknown)."""
    for child in node.get("children", []):
        label_columns(child, source_names, widths)
    kind = node["operator_type"]
    below = node["children"][0]["labels"] if len(node.get("children", [])) == 1 else None
    entries = as_columns(node)
    if kind == "TABLE_SCAN":
        labels = entries if entries is not None else None
    elif kind == "PROJECTION" and entries is not None:
        labels = []
        for entry in entries:
            pointer = re.fullmatch(r"#(\d+)", str(entry))
            if pointer:
                k = int(pointer.group(1))
                labels.append(below[k] if below is not None and k < len(below) else "?")
            else:
                labels.append(entry if entry in source_names else "expr")
    elif kind in ("WINDOW", "STREAMING_WINDOW") and below is not None:
        labels = below + ["window"] * len(listify(node.get("extra_info", {}).get("Projections")))
    elif kind in ("ORDER_BY", "FILTER", "LIMIT") and below is not None:
        labels = below
    else:
        labels = None
    node["labels"] = labels


def flatten(node, depth=0, out=None):
    out = [] if out is None else out
    out.append((depth, node))
    for child in node.get("children", []):
        flatten(child, depth + 1, out)
    return out


def operator_rows(profile, source_names):
    root = {"operator_type": "QUERY", "children": profile.get("children", [])}
    label_columns(root, source_names, None)
    nodes = [(d, n) for d, n in flatten(root) if d > 0]
    total = sum(n.get("operator_timing", 0.0) for _, n in nodes) or 1.0
    rows = []
    for depth, n in nodes:
        labels = n["labels"]
        kind = n["operator_type"]
        source = [c for c in labels if c in source_names] if labels is not None else None
        sorts = kind in ("WINDOW", "STREAMING_WINDOW") and any(
            "ORDER BY" in str(p) for p in listify(n.get("extra_info", {}).get("Projections"))
        )
        rows.append(
            {
                "depth": depth - 1,
                "type": kind,
                "cpu_s": n.get("operator_timing", 0.0),
                "share": n.get("operator_timing", 0.0) / total,
                "rows": n.get("operator_cardinality"),
                "out_mib": n.get("result_set_size", 0) / MIB,
                "out_columns": None if labels is None else len(labels),
                "source_columns_out": None if source is None else len(source),
                "columns": labels,
                "sorts": sorts,
                "partitioned": kind == "WINDOW"
                and any("PARTITION BY" in str(p) for p in listify(n.get("extra_info", {}).get("Projections"))),
            }
        )
    return rows


def plan_summary(operators):
    by_type = {}
    for op in operators:
        by_type[op["type"]] = by_type.get(op["type"], 0) + 1
    windows = [op for op in operators if op["type"] in ("WINDOW", "STREAMING_WINDOW")]
    count = lambda *types: sum(by_type.get(t, 0) for t in types)
    return {
        "windows": len(windows),
        "windows_that_sort": sum(op["sorts"] for op in windows),
        "windows_partitioned": sum(op["partitioned"] for op in windows),
        "order_by": count("ORDER_BY", "TOP_N"),
        "hash_aggregates": count("HASH_GROUP_BY", "PERFECT_HASH_GROUP_BY"),
        "ungrouped_aggregates": count("UNGROUPED_AGGREGATE"),
        "unnest": count("UNNEST"),
        "projections": count("PROJECTION"),
        "joins": sum(v for k, v in by_type.items() if "JOIN" in k),
        "ctes": sum(v for k, v in by_type.items() if "CTE" in k),
        "blocking_operators": count(*BLOCKING),
        "by_type": by_type,
        "window_outputs": [
            {
                "out_columns": op["out_columns"],
                "source_columns_out": op["source_columns_out"],
                "columns": op["columns"],
                "sorts": op["sorts"],
                "partitioned": op["partitioned"],
            }
            for op in windows
        ],
    }


def query_fields(profile):
    keep = ("latency", "cpu_time", "blocked_thread_time", "rows_returned", "cumulative_rows_scanned", "result_set_size")
    fields = {k: profile.get(k) for k in keep}
    phases = {k: v for k, v in profile.items() if k in PHASES or k.startswith("optimizer_")}
    plan_optimize = sum(profile.get(k, 0.0) for k in ("planner", "physical_planner", "all_optimizers"))
    fields.update(
        phases=phases,
        plan_optimize_s=plan_optimize,
        execution_s=profile["latency"] - plan_optimize,
        cpu_over_latency=profile["cpu_time"] / profile["latency"] if profile["latency"] else None,
    )
    return fields


def profile_cell(db, shape, threads, timeout, profile_file, scratch):
    """One profiled process. Returns a record with `status` None and the parsed profile, or a failure status."""
    spill_dir = scratch / "tmp"
    spill_dir.mkdir(parents=True)
    sql = (
        f"SET temp_directory='{spill_dir}'; PRAGMA enable_profiling='json'; PRAGMA profiling_output='{profile_file}'; "
        f"PRAGMA profiling_mode='detailed'; {query_sql(db, shape, SHAPES[shape][1])}"
    )
    sampler = DirSampler(spill_dir)
    sampler.start()
    record = run_once(db, sql, "direct", threads, timeout)
    sampler.finish()
    record.update(spill_peak_bytes=sampler.peak, spill_files=sampler.files)
    if record["status"] is None:
        try:
            record["profile"] = json.loads(profile_file.read_text())
        except (OSError, ValueError):
            return {"status": "error", "detail": "no profile written"}
        record["profile_file"] = str(profile_file)
    return record


def f(v, places=2):
    return "-" if v is None else f"{v:.{places}f}"


def ms(v):
    return "-" if v is None else f"{v * 1000:.1f}"


def stats(values):
    return (min(values), statistics.median(values), max(values)) if values else (None, None, None)


def cell_report(cell, runs):
    """The record of one cell: first run's profile, statistics over all finished runs."""
    done = [r for r in runs if r["status"] is None]
    record = dict(cell, runs=len(runs), finished=len(done), failed=sorted({r["status"] for r in runs if r["status"]}))
    if not done:
        record["detail"] = runs[0].get("detail")
        return record
    first = done[0]
    operators = operator_rows(first["profile"], cell["source_names"])
    record.update(
        query=query_fields(first["profile"]),
        stdout=first["stdout"],
        operators=operators,
        plan=plan_summary(operators),
        spill_peak_bytes=max(r["spill_peak_bytes"] for r in done),
        spill_files=max(r["spill_files"] for r in done),
        user_s=first["user"],
        sys_s=first["sys"],
        faults=first["faults"],
        swaps=first["swaps"],
        latency=dict(zip(("min", "median", "max"), stats([r["profile"]["latency"] for r in done]))),
        wall=dict(zip(("min", "median", "max"), stats([r["wall"] for r in done]))),
        peak_mib=dict(zip(("min", "median", "max"), stats([r["peak_mib"] for r in done if r["peak_mib"]]))),
        profile_file=first["profile_file"],
    )
    return record


def head(record):
    return f"{record['shape']}, {record['rows']:,} rows, source {record['variant']}, threads {record['threads']}"


def operator_table(record):
    lines = [
        "| # | operator | CPU s | share | rows out | out MiB | columns out | source cols out |",
        "|--:|---|---:|---:|---:|---:|---:|---:|",
    ]
    for i, op in enumerate(record["operators"]):
        name = "&nbsp;&nbsp;" * op["depth"] + op["type"] + (" (sorts)" if op["sorts"] else "")
        lines.append(
            f"| {i} | {name} | {f(op['cpu_s'], 3)} | {op['share'] * 100:.1f}% | {op['rows']} | {f(op['out_mib'], 1)} "
            f"| {'?' if op['out_columns'] is None else op['out_columns']} "
            f"| {'?' if op['source_columns_out'] is None else op['source_columns_out']} |"
        )
    return lines


def cell_markdown(record):
    lines = [f"### {head(record)}", ""]
    if record["finished"] == 0:
        return lines + [f"No finished run: {record['failed']} {record.get('detail') or ''}", ""]
    q, plan = record["query"], record["plan"]
    lat, wall, peak = record["latency"], record["wall"], record["peak_mib"]
    lines += [
        f"Runs {record['finished']}/{record['runs']}. Latency min / median / max "
        f"{f(lat['min'])} / {f(lat['median'])} / {f(lat['max'])} s; wall {f(wall['min'])} / {f(wall['median'])} / "
        f"{f(wall['max'])} s; peak RSS {f(peak['min'], 0)} / {f(peak['median'], 0)} / {f(peak['max'], 0)} MiB. "
        f"First run: cpu_time {f(q['cpu_time'], 3)} s, CPU / latency {f(q['cpu_over_latency'])}, blocked thread time "
        f"{f(q['blocked_thread_time'], 3)} s, rows scanned {q['cumulative_rows_scanned']:,}, faults "
        f"{record['faults']:.0f}, swaps {record['swaps']:.0f}.",
        "",
        f"Plan + optimize {ms(q['plan_optimize_s'])} ms = planner {ms(q['phases'].get('planner'))} (binder "
        f"{ms(q['phases'].get('planner_binding'))}) + physical planner {ms(q['phases'].get('physical_planner'))} + "
        f"optimizers {ms(q['phases'].get('all_optimizers'))} (DECIDE pass {ms(q['phases'].get('optimizer_decide'))}); "
        f"execution (derived) {f(q['execution_s'], 3)} s.",
        "",
        f"Temp directory: {'WRITTEN, ' + format(record['spill_peak_bytes'] / MIB, '.1f') + ' MiB in ' + str(record['spill_files']) + ' files' if record['spill_files'] else 'no file seen (nothing spilled)'}.",
        "",
        f"Plan: {plan['windows']} windows ({plan['windows_that_sort']} sort, {plan['windows_partitioned']} partitioned), "
        f"{plan['order_by']} ORDER BY, {plan['hash_aggregates']} hash aggregates, {plan['unnest']} unnest, "
        f"{plan['projections']} projections, {plan['joins']} joins, {plan['ctes']} CTEs, "
        f"{plan['blocking_operators']} blocking operators (windows, ORDER BY, aggregates). "
        f"Operators by type: {json.dumps(plan['by_type'], sort_keys=True)}.",
        "",
        "Window outputs (columns out / of them source columns), in plan order from the root:",
    ]
    for i, w in enumerate(plan["window_outputs"]):
        cols = w["columns"]
        if cols is None:
            kept = "column names unknown"
        else:
            names = sorted({c for c in cols if c in record["source_names"]})
            other = sum(1 for c in cols if c not in record["source_names"] and c != "?")
            kept = f"source columns {names or 'none'} ({w['source_columns_out']} counting repeats), {other} computed"
        lines.append(
            f"- window {i}{' (sorts)' if w['sorts'] else ''}{' (partitioned)' if w['partitioned'] else ''}: "
            f"{w['out_columns']} columns out; {kept}"
        )
    return lines + ["", *operator_table(record), ""]


SUMMARY_HEADER = [
    "shape", "rows", "variant", "threads", "ok", "latency med s", "wall med s", "cpu_time s", "CPU/latency",
    "peak RSS MiB", "spill MiB", "plan+opt ms", "execution s", "windows", "sorts", "blocking ops", "window src cols",
]  # fmt: skip


def summary_row(record):
    if record["finished"] == 0:
        return (
            f"| {record['shape']} | {record['rows']:,} | {record['variant']} | {record['threads']} | 0/{record['runs']} |"
            + " - |" * 12
        )
    q, plan = record["query"], record["plan"]
    sources = "/".join(
        "?" if w["source_columns_out"] is None else str(w["source_columns_out"]) for w in plan["window_outputs"]
    )
    cells = [
        record["shape"], f"{record['rows']:,}", record["variant"], record["threads"],
        f"{record['finished']}/{record['runs']}", f(record["latency"]["median"]), f(record["wall"]["median"]),
        f(q["cpu_time"]), f(q["cpu_over_latency"]), f(record["peak_mib"]["median"], 0),
        f(record["spill_peak_bytes"] / MIB, 1), ms(q["plan_optimize_s"]), f(q["execution_s"], 3),
        plan["windows"], plan["windows_that_sort"] + plan["order_by"], plan["blocking_operators"], sources or "-",
    ]  # fmt: skip
    return "| " + " | ".join(str(c) for c in cells) + " |"


def write_outputs(out_dir, meta, records):
    (out_dir / "r1_plan_profile.json").write_text(json.dumps({"meta": meta, "records": records}, indent=1))
    lines = [
        "| " + " | ".join(SUMMARY_HEADER) + " |",
        "|---|---:|---|---|---|" + "---:|" * (len(SUMMARY_HEADER) - 5),
        *[summary_row(r) for r in records],
        "",
    ]
    for record in records:
        lines += cell_markdown(record)
    text = "\n".join(lines)
    (out_dir / "r1_plan_profile.md").write_text(text + "\n")
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rows", nargs="+", type=int, default=[100000])
    parser.add_argument("--shapes", nargs="+", choices=sorted(SHAPES), default=DEFAULT_SHAPES)
    parser.add_argument("--variants", nargs="+", choices=VARIANTS, default=["stored"], help="le_positive only")
    parser.add_argument("--threads", nargs="+", default=["1", "all"], help="integers or `all`")
    parser.add_argument("--repeats", type=int, default=1, help="runs per cell; the operator profile is the first run's")
    parser.add_argument("--timeout", type=int, default=600, help="seconds before a run is killed and recorded")
    parser.add_argument("--source-dir", type=Path, default=BINARY.parents[1] / "r1db")
    parser.add_argument("--db-dir", type=Path, required=True, help="where narrow and wide databases are built")
    parser.add_argument("--out-dir", type=Path, help="JSON, markdown and raw profiles (default: --db-dir)")
    args = parser.parse_args()
    for t in args.threads:
        if t != "all" and not (t.isdigit() and int(t) > 0):
            parser.error(f"thread setting {t!r} must be a positive integer or `all`")
    out_dir = args.out_dir or args.db_dir
    raw_dir = out_dir / "raw"
    for d in (args.db_dir, out_dir, raw_dir):
        d.mkdir(parents=True, exist_ok=True)

    sources = {}
    for rows in sorted(args.rows):
        sources[rows] = args.source_dir / f"r1_{rows}.db"
        if not sources[rows].exists() or not budgets_path(sources[rows]).exists():
            sys.exit(f"{sources[rows]} or its budgets file is missing; build it with profile_direct_r1.py first")
    cells = [
        (shape, rows, variant, threads)
        for rows in sorted(args.rows)
        for shape in args.shapes
        for variant in args.variants
        if variant == "stored" or shape == "le_positive"
        for threads in args.threads
    ]
    meta = {
        "args": {k: str(v) for k, v in vars(args).items()},
        "default_threads": default_threads(),
        "profiler": "json, detailed",
    }
    records, columns = [], {}
    for shape, rows, variant, threads in cells:
        db = variant_db(sources[rows], args.db_dir, rows, variant)
        columns.setdefault(db, set(source_columns(db)))
        cell = dict(shape=shape, rows=rows, variant=variant, threads=threads, source_names=columns[db])
        runs = []
        for repeat in range(args.repeats):
            with tempfile.TemporaryDirectory(prefix=f"{shape}_{rows}_{variant}_{threads}_", dir=out_dir) as scratch:
                started = time.strftime("%H:%M:%S")
                print(f"{started} {head(cell)} run {repeat + 1}/{args.repeats}", file=sys.stderr)
                raw = raw_dir / f"{shape}_{rows}_{variant}_t{threads}_r{repeat}.json"
                runs.append(profile_cell(db, shape, threads, args.timeout, raw, Path(scratch)))
        record = cell_report(cell, runs)
        record["source_names"] = sorted(record["source_names"])
        record["source_columns"] = len(record["source_names"])
        records.append(record)
        write_outputs(out_dir, meta, records)
    print(write_outputs(out_dir, meta, records))


if __name__ == "__main__":
    main()
