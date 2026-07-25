#!/usr/bin/env python3
"""DecidB DECIDE performance benchmark runner.

Runs a set of DECIDE queries against pre-generated TPC-H databases of different
sizes (medium/large), measuring wall-clock time, peak memory (RSS), and
per-stage breakdowns.

Usage:
    python3 benchmark/decide/run_benchmarks.py                    # run all
    python3 benchmark/decide/run_benchmarks.py --queries Q1,Q3    # subset
    python3 benchmark/decide/run_benchmarks.py --sizes medium     # single size
    python3 benchmark/decide/run_benchmarks.py --manual           # manual query
    python3 benchmark/decide/run_benchmarks.py --compare          # auto compare

Databases must be generated first: make decide-bench-setup
When DECIDB_BENCH=1 is set, also parses per-stage timers from decidb stderr.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
QUERIES_DIR = SCRIPT_DIR / "queries"
RESULTS_DIR = SCRIPT_DIR / "results"
DATABASES_DIR = SCRIPT_DIR / "databases"

DECIDB_EXE = REPO_ROOT / "build" / "release" / "decidb"

DB_SIZES = ["medium", "large"]
DEFAULT_ITERATIONS = 3

COEFFICIENTS: dict[str, dict[str, int]] = {
    "medium": {
        "Q1_QTY_CAP": 3829286, "Q1_R_QTY_CAP": 1260000, "Q1_GRP_CAP": 62500,
        "Q1_LOCAL_CAP": 5420000000,
        "Q2_NE_SUM": 127500, "Q2_PRICE_CAP": 9070000000, "Q2_MOD_CAP": 1147500,
        "Q3_ROW_LIMIT": 20000, "Q3_ABS_CAP": 150000,
        "Q4_QTY_CAP": 2500000,
        "Q5_ROW_LIMIT": 50000, "Q5_SSE_CAP": 38700000,
        "Q6_ROW_LIMIT": 8192, "Q6_PICK_CAP": 4096, "Q6_BILIN_CAP": 204800,
        "Q6_GRP_CAP": 80000,
        "Q7_ROW_LIMIT": 1024, "Q7_BILIN_CAP": 40960, "Q7_PICK_CAP": 512,
        "Q9_ROW_LIMIT": 5000,
        "Q10_ROW_LIMIT": 50000, "Q10_SUM_CAP": 750000, "Q10_ABS_FLOOR": 125000,
        "Q11_BUDGET": 51000000000,
    },
    "large": {
        "Q1_QTY_CAP": 7660945, "Q1_R_QTY_CAP": 2520000, "Q1_GRP_CAP": 125000,
        "Q1_LOCAL_CAP": 10900000000,
        "Q2_NE_SUM": 255000, "Q2_PRICE_CAP": 18200000000, "Q2_MOD_CAP": 2295000,
        "Q3_ROW_LIMIT": 40000, "Q3_ABS_CAP": 300000,
        "Q4_QTY_CAP": 5000000,
        "Q5_ROW_LIMIT": 100000, "Q5_SSE_CAP": 77400000,
        "Q6_ROW_LIMIT": 16384, "Q6_PICK_CAP": 8192, "Q6_BILIN_CAP": 409600,
        "Q6_GRP_CAP": 160000,
        "Q7_ROW_LIMIT": 2048, "Q7_BILIN_CAP": 81920, "Q7_PICK_CAP": 1024,
        "Q9_ROW_LIMIT": 10000,
        "Q10_ROW_LIMIT": 100000, "Q10_SUM_CAP": 1500000, "Q10_ABS_FLOOR": 250000,
        "Q11_BUDGET": 102000000000,
    },
}

PLACEHOLDER_RE = re.compile(r"\$\{([^}]+)\}")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def get_git_commit() -> str:
    """Return short git commit hash of HEAD."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, cwd=REPO_ROOT,
        )
        return result.stdout.strip() if result.returncode == 0 else "unknown"
    except FileNotFoundError:
        return "unknown"


def get_system_info() -> dict:
    """Collect basic system info for reproducibility."""
    info: dict = {
        "platform": platform.platform(),
        "cpu": platform.processor() or "unknown",
        "python": platform.python_version(),
    }
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    kb = int(line.split()[1])
                    info["ram_gb"] = round(kb / 1024 / 1024, 1)
                    break
    except (OSError, ValueError):
        pass
    return info


def is_dirty() -> bool:
    """Check if the git working tree has uncommitted changes."""
    try:
        result = subprocess.run(
            ["git", "status", "--porcelain"],
            capture_output=True, text=True, cwd=REPO_ROOT,
        )
        return bool(result.stdout.strip())
    except FileNotFoundError:
        return False


def discover_queries() -> dict[str, Path]:
    """Discover query files by globbing queries/q*.sql."""
    queries: dict[str, Path] = {}
    for path in sorted(QUERIES_DIR.glob("q*.sql")):
        if path.suffix == ".sql" and not path.name.endswith(".example"):
            m = re.match(r"q(\d+)", path.stem)
            if m:
                queries[f"Q{m.group(1)}"] = path
    return queries


def get_query_description(filename: str) -> str:
    """Extract description from query filename.

    q1_linear_knapsack.sql -> linear_knapsack
    """
    stem = Path(filename).stem
    m = re.match(r"q\d+_(.*)", stem)
    return m.group(1) if m else stem


def resolve_query_sql(sql: str, size: str) -> tuple[str, dict[str, int]]:
    """Resolve ${NAME} placeholders for a benchmark size.

    Returns the resolved SQL and a dict containing only the coefficients
    actually referenced by this query.
    """
    coefficients = COEFFICIENTS.get(size)
    if coefficients is None:
        raise ValueError(
            f"no coefficient map configured for size '{size}'. "
            f"Available sizes: {', '.join(DB_SIZES)}"
        )

    used: dict[str, int] = {}

    def replace(match: re.Match[str]) -> str:
        name = match.group(1)
        if name in coefficients:
            used[name] = coefficients[name]
            return str(coefficients[name])
        return match.group(0)

    resolved_sql = PLACEHOLDER_RE.sub(replace, sql)
    unresolved = sorted(set(PLACEHOLDER_RE.findall(resolved_sql)))
    if unresolved:
        tokens = ", ".join(f"${{{name}}}" for name in unresolved)
        raise ValueError(f"unresolved placeholder(s) for size '{size}': {tokens}")
    return resolved_sql, used


def find_previous_result(current_commit: str) -> Path | None:
    """Walk git log to find the most recent commit with a result file."""
    try:
        result = subprocess.run(
            ["git", "log", "--format=%h", "-n", "50"],
            capture_output=True, text=True, cwd=REPO_ROOT,
        )
        if result.returncode != 0:
            return None
        for commit_hash in result.stdout.strip().splitlines():
            commit_hash = commit_hash.strip()
            if commit_hash == current_commit:
                continue
            result_path = RESULTS_DIR / f"{commit_hash}.json"
            if result_path.exists():
                return result_path
    except FileNotFoundError:
        pass
    return None


IS_MACOS = platform.system() == "Darwin"
_TIME_FLAG: str | None = None


def get_time_flag() -> str:
    """Return the best supported /usr/bin/time flag for this host."""
    global _TIME_FLAG
    if _TIME_FLAG is not None:
        return _TIME_FLAG

    if IS_MACOS:
        # `time -l` reports peak RSS on macOS, but sandboxed environments may
        # deny the sysctl it uses and make successful child commands exit as
        # failures. Probe once and fall back to POSIX timing when needed.
        try:
            result = subprocess.run(
                ["/usr/bin/time", "-l", "true"],
                capture_output=True,
                text=True,
                timeout=5,
            )
            _TIME_FLAG = "-l" if result.returncode == 0 else "-p"
        except (OSError, subprocess.TimeoutExpired):
            _TIME_FLAG = "-p"
    else:
        _TIME_FLAG = "-v"
    return _TIME_FLAG


def parse_time_output(stderr: str) -> dict:
    """Parse /usr/bin/time output from stderr (handles both GNU -v and BSD -l formats).

    Returns dict with wall_time_s and peak_rss_kb.
    """
    result: dict = {}

    # POSIX format: "real 0.45" (used as the macOS fallback when `time -l`
    # cannot report resource usage in restricted environments).
    m = re.search(r"^real\s+([\d.]+)", stderr, re.MULTILINE)
    if m:
        result["wall_time_s"] = round(float(m.group(1)), 4)

    if IS_MACOS:
        # BSD format: "        0.45 real         0.00 user         0.00 sys"
        m = re.search(r"^\s*([\d.]+)\s+real", stderr, re.MULTILINE)
        if m:
            result["wall_time_s"] = round(float(m.group(1)), 4)
        # BSD format: "             1212416  maximum resident set size" (bytes)
        m = re.search(r"(\d+)\s+maximum resident set size", stderr)
        if m:
            result["peak_rss_kb"] = int(m.group(1)) // 1024
    else:
        # GNU format: "Elapsed (wall clock) time (h:mm:ss or m:ss): 0:00.45"
        m = re.search(
            r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*"
            r"(?:(\d+):)?(\d+):(\d+(?:\.\d+)?)",
            stderr,
        )
        if m:
            hours = int(m.group(1) or 0)
            minutes = int(m.group(2))
            seconds = float(m.group(3))
            result["wall_time_s"] = round(hours * 3600 + minutes * 60 + seconds, 4)
        # GNU format: "Maximum resident set size (kbytes): 12345"
        m = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", stderr)
        if m:
            result["peak_rss_kb"] = int(m.group(1))

    return result


def parse_bench_output(stderr: str) -> dict:
    """Parse DECIDB_BENCH stage timer lines from stderr.

    Lines look like: DECIDB_BENCH: key=value
    """
    stages: dict = {}
    for m in re.finditer(r"DECIDB_BENCH:\s*(\w+)=([\d.]+)", stderr):
        key = m.group(1)
        val = m.group(2)
        try:
            stages[key] = float(val) if "." in val else int(val)
        except ValueError:
            stages[key] = val
    return stages


def run_single(query_sql: str, db_path: Path, timeout: int = 600) -> dict:
    """Run a single benchmark query, returning timing and memory metrics."""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".sql", delete=False) as f:
        f.write(query_sql)
        sql_path = f.name

    try:
        env = os.environ.copy()
        time_flag = get_time_flag()
        cmd = [
            "/usr/bin/time", time_flag,
            str(DECIDB_EXE), str(db_path), "-readonly",
        ]

        with open(sql_path) as sql_file:
            result = subprocess.run(
                cmd,
                stdin=sql_file,
                capture_output=True,
                text=True,
                timeout=timeout,
                env=env,
            )

        metrics = {"exit_code": result.returncode}

        # Parse /usr/bin/time output (goes to stderr)
        time_metrics = parse_time_output(result.stderr)
        metrics.update(time_metrics)

        # Parse DECIDB_BENCH stage timers if present
        bench_metrics = parse_bench_output(result.stderr)
        if bench_metrics:
            metrics["stages"] = bench_metrics

        # Capture any errors
        if result.returncode != 0:
            if IS_MACOS:
                # BSD time output: lines with leading whitespace + number + label
                time_line_re = re.compile(r"^\s+[\d.]+\s+(real|user|sys)")
                posix_time_line_re = re.compile(r"^(real|user|sys)\s+[\d.]+")
                resource_line_re = re.compile(r"^\s+\d+\s+\w")
                error_lines = [
                    line for line in result.stderr.splitlines()
                    if line.strip()
                    and not time_line_re.match(line)
                    and not posix_time_line_re.match(line.strip())
                    and not resource_line_re.match(line)
                ]
            else:
                error_lines = [
                    line for line in result.stderr.splitlines()
                    if not line.strip().startswith(("Command", "Elapsed", "Maximum",
                        "Average", "Major", "Minor", "Voluntary", "Involuntary",
                        "Swaps", "File", "Socket", "Signals", "Page", "Exit",
                        "Percent", "System", "User"))
                    and line.strip()
                ]
            if error_lines:
                metrics["error"] = "\n".join(error_lines[:5])

        return metrics

    except subprocess.TimeoutExpired:
        return {"exit_code": -1, "error": f"Timeout after {timeout}s"}
    finally:
        os.unlink(sql_path)


def compute_stats(runs: list[dict]) -> dict:
    """Compute median and stddev from a list of run results."""
    successful = [r for r in runs if r.get("exit_code") == 0]
    stats: dict = {"total_runs": len(runs), "successful_runs": len(successful)}

    if not successful:
        return stats

    wall_times = [r["wall_time_s"] for r in successful if "wall_time_s" in r]
    if wall_times:
        stats["median_wall_time_s"] = round(statistics.median(wall_times), 4)
        if len(wall_times) > 1:
            stats["stddev_wall_time_s"] = round(statistics.stdev(wall_times), 4)
        else:
            stats["stddev_wall_time_s"] = 0.0

    rss_values = [r["peak_rss_kb"] for r in successful if "peak_rss_kb" in r]
    if rss_values:
        stats["median_peak_rss_kb"] = int(statistics.median(rss_values))

    # Aggregate stage timers if present (median of each stage)
    stage_keys: set[str] = set()
    for r in successful:
        if "stages" in r:
            stage_keys.update(r["stages"].keys())
    if stage_keys:
        stats["stages"] = {}
        for key in sorted(stage_keys):
            values = [r["stages"][key] for r in successful
                      if "stages" in r and key in r["stages"]]
            if values:
                if isinstance(values[0], float):
                    stats["stages"][key] = round(statistics.median(values), 2)
                else:
                    stats["stages"][key] = int(statistics.median(values))

    return stats


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------


def print_comparison(current: list[dict], previous: dict) -> None:
    """Print a comparison table between current and previous results."""
    prev_lookup: dict[tuple[str, str], dict] = {}
    for entry in previous.get("queries", []):
        key = (entry["query"], entry.get("size", ""))
        prev_lookup[key] = entry.get("stats", {})

    print()
    print(f"Comparison: {previous.get('commit', '?')} -> current")
    print("=" * 90)
    header = (f"{'Query':<25} {'Size':>8} {'Prev(s)':>9} {'Curr(s)':>9} "
              f"{'Delta':>8} {'%':>7} {'RSS Delta':>10}")
    print(header)
    print("-" * 90)

    for entry in current:
        query = entry["query"]
        desc = entry.get("description", query)
        name = f"{query}_{desc}" if desc != query else query
        size = entry["size"]
        curr_stats = entry.get("stats", {})
        prev_stats = prev_lookup.get((query, size), {})

        curr_t = curr_stats.get("median_wall_time_s")
        prev_t = prev_stats.get("median_wall_time_s")
        curr_rss = curr_stats.get("median_peak_rss_kb")
        prev_rss = prev_stats.get("median_peak_rss_kb")

        if curr_t is not None and prev_t is not None:
            delta = curr_t - prev_t
            pct = (delta / prev_t * 100) if prev_t > 0 else 0
            delta_str = f"{delta:+.4f}"
            pct_str = f"{pct:+.1f}%"
        else:
            delta_str = "-"
            pct_str = "-"

        prev_str = f"{prev_t:.4f}" if prev_t is not None else "-"
        curr_str = f"{curr_t:.4f}" if curr_t is not None else "FAIL"

        rss_delta_str = "-"
        if curr_rss is not None and prev_rss is not None:
            rss_delta_mb = (curr_rss - prev_rss) / 1024
            rss_delta_str = f"{rss_delta_mb:+.1f}MB"

        print(f"{name:<25} {size:>8} {prev_str:>9} {curr_str:>9} "
              f"{delta_str:>8} {pct_str:>7} {rss_delta_str:>10}")

    print("=" * 90)
    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(description="DecidB DECIDE benchmark runner")
    parser.add_argument(
        "--queries", type=str, default=None,
        help="Comma-separated list of queries to run (e.g., Q1,Q3). Default: all",
    )
    parser.add_argument(
        "--sizes", type=str, default=None,
        help="Comma-separated DB sizes (e.g., medium,large). Default: all",
    )
    parser.add_argument(
        "--iterations", type=int, default=DEFAULT_ITERATIONS,
        help=f"Number of iterations per query/size (default: {DEFAULT_ITERATIONS})",
    )
    parser.add_argument(
        "--manual", action="store_true",
        help="Run queries/manual.sql instead of standard queries",
    )
    parser.add_argument(
        "--compare", nargs="?", const="auto", default=None,
        help="Compare with previous results. No arg = auto-find, or specify commit hash",
    )
    parser.add_argument(
        "--timeout", type=int, default=600,
        help="Timeout per query execution in seconds (default: 600)",
    )
    args = parser.parse_args()

    # Validate prerequisites
    if not DECIDB_EXE.exists():
        print(f"ERROR: decidb executable not found at {DECIDB_EXE}", file=sys.stderr)
        print("Run 'make release' first.", file=sys.stderr)
        sys.exit(1)

    # Determine sizes
    sizes = [s.strip() for s in args.sizes.split(",")] if args.sizes else DB_SIZES
    for s in sizes:
        if s not in DB_SIZES:
            print(f"ERROR: Unknown size '{s}'. Available: {', '.join(DB_SIZES)}",
                  file=sys.stderr)
            sys.exit(1)
        db_path = DATABASES_DIR / f"{s}.db"
        if not db_path.exists():
            print(f"ERROR: Database not found: {db_path}", file=sys.stderr)
            print("Run 'make decide-bench-setup' to generate databases.", file=sys.stderr)
            sys.exit(1)

    commit = get_git_commit()
    system_info = get_system_info()
    dirty = is_dirty()

    # --- Manual mode ---
    if args.manual:
        manual_path = QUERIES_DIR / "manual.sql"
        if not manual_path.exists():
            print(f"ERROR: {manual_path} not found.", file=sys.stderr)
            print(f"Copy {QUERIES_DIR / 'manual.sql.example'} to manual.sql and edit.",
                  file=sys.stderr)
            sys.exit(1)

        sql = manual_path.read_text()
        all_results: list[dict] = []

        print(f"DecidB Manual Benchmark (commit: {commit}{'*' if dirty else ''})")
        print(f"Sizes: {', '.join(sizes)}")
        print(f"Iterations: {args.iterations}")
        if os.environ.get("DECIDB_BENCH"):
            print("Stage timers: ENABLED (DECIDB_BENCH=1)")
        print()

        for size in sizes:
            db_path = DATABASES_DIR / f"{size}.db"
            print(f"[manual] size={size}", end="", flush=True)

            runs: list[dict] = []
            for i in range(args.iterations):
                metrics = run_single(sql, db_path, timeout=args.timeout)
                runs.append(metrics)
                print("." if metrics.get("exit_code") == 0 else "X", end="", flush=True)

            stats = compute_stats(runs)
            entry = {
                "query": "manual",
                "description": "manual",
                "size": size,
                "sql": sql.strip(),
                "runs": runs,
                "stats": stats,
            }
            all_results.append(entry)
            median = stats.get("median_wall_time_s")
            print(f" {median:.4f}s" if median is not None else " FAILED")

        output = {
            "commit": commit,
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "system": system_info,
            "iterations": args.iterations,
            "sizes": sizes,
            "queries": all_results,
        }

        RESULTS_DIR.mkdir(parents=True, exist_ok=True)
        output_path = RESULTS_DIR / "manual.json"
        with open(output_path, "w") as f:
            json.dump(output, f, indent=2)
        print(f"\nResults saved to: {output_path}")

        subprocess.run([sys.executable, str(SCRIPT_DIR / "view_results.py"), "manual"])
        return

    # --- Standard mode ---
    available_queries = discover_queries()
    if not available_queries:
        print("ERROR: No query files found in queries/", file=sys.stderr)
        sys.exit(1)

    if args.queries:
        query_names = [q.strip().upper() for q in args.queries.split(",")]
        for q in query_names:
            if q not in available_queries:
                print(f"ERROR: Unknown query '{q}'. "
                      f"Available: {', '.join(sorted(available_queries))}",
                      file=sys.stderr)
                sys.exit(1)
    else:
        query_names = sorted(available_queries.keys())

    print(f"DecidB DECIDE Benchmark (commit: {commit}{'*' if dirty else ''})")
    print(f"Queries: {', '.join(query_names)}")
    print(f"Sizes: {', '.join(sizes)}")
    print(f"Iterations: {args.iterations}")
    if os.environ.get("DECIDB_BENCH"):
        print("Stage timers: ENABLED (DECIDB_BENCH=1)")
    print()

    all_results: list[dict] = []
    total_combos = len(query_names) * len(sizes)
    combo_idx = 0

    for query_name in query_names:
        query_path = available_queries[query_name]
        sql = query_path.read_text()
        desc = get_query_description(query_path.name)

        for size in sizes:
            combo_idx += 1
            try:
                resolved_sql, coefficients = resolve_query_sql(sql, size)
            except ValueError as e:
                print(f"ERROR: {query_path.name}: {e}", file=sys.stderr)
                sys.exit(1)

            db_path = DATABASES_DIR / f"{size}.db"
            print(f"[{combo_idx}/{total_combos}] {query_name}_{desc} (size={size})",
                  end="", flush=True)

            runs: list[dict] = []
            for i in range(args.iterations):
                metrics = run_single(resolved_sql, db_path, timeout=args.timeout)
                runs.append(metrics)
                print("." if metrics.get("exit_code") == 0 else "X", end="", flush=True)

            stats = compute_stats(runs)
            entry = {
                "query": query_name,
                "description": desc,
                "size": size,
                "sql": resolved_sql.strip(),
                "coefficients": coefficients,
                "runs": runs,
                "stats": stats,
            }
            all_results.append(entry)
            median = stats.get("median_wall_time_s")
            print(f" {median:.4f}s" if median is not None else " FAILED")

    # Build output document
    output = {
        "commit": commit,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "system": system_info,
        "iterations": args.iterations,
        "sizes": sizes,
        "queries": all_results,
    }

    # Save JSON
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    identifier = "dirty" if dirty else commit
    output_path = RESULTS_DIR / f"{identifier}.json"
    with open(output_path, "w") as f:
        json.dump(output, f, indent=2)
    print(f"\nResults saved to: {output_path}")

    # Comparison
    if args.compare:
        if args.compare == "auto":
            prev_path = find_previous_result(commit)
        else:
            prev_path = RESULTS_DIR / f"{args.compare}.json"

        if prev_path and prev_path.exists():
            try:
                with open(prev_path) as f:
                    prev = json.load(f)
                print_comparison(all_results, prev)
            except (OSError, json.JSONDecodeError) as e:
                print(f"WARNING: Could not load comparison file: {e}", file=sys.stderr)
        elif args.compare != "auto":
            print(f"WARNING: No results found for '{args.compare}'", file=sys.stderr)
        else:
            print("No previous results found for comparison.")

    # Invoke viewer
    subprocess.run([sys.executable, str(SCRIPT_DIR / "view_results.py"), identifier])


if __name__ == "__main__":
    main()
