#!/usr/bin/env python3
"""Visual benchmark results viewer with ANSI colored stage bars.

Usage:
    python3 view_results.py              # latest result (most recent file)
    python3 view_results.py {hash}       # specific commit
    python3 view_results.py dirty        # dirty result
    python3 view_results.py manual       # manual result
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
RESULTS_DIR = SCRIPT_DIR / "results"

# ANSI color codes
BLUE = "\033[34m"
YELLOW = "\033[33m"
RED = "\033[31m"
GREY = "\033[90m"
BOLD = "\033[1m"
DIM = "\033[2m"
RESET = "\033[0m"

BAR_WIDTH = 60


def find_result_file(identifier: str | None) -> Path:
    """Resolve identifier to a JSON result file path."""
    if identifier:
        path = RESULTS_DIR / f"{identifier}.json"
        if path.exists():
            return path
        path = Path(identifier)
        if path.exists():
            return path
        print(f"ERROR: No results found for '{identifier}'", file=sys.stderr)
        sys.exit(1)

    # No identifier — find most recent file by modification time
    results = sorted(RESULTS_DIR.glob("*.json"), key=lambda p: p.stat().st_mtime)
    if not results:
        print("ERROR: No result files found in results/", file=sys.stderr)
        sys.exit(1)
    return results[-1]


def format_count(n: int | float) -> str:
    """Format a count with K/M suffixes for readability."""
    n = int(n)
    if n >= 1_000_000:
        return f"{n / 1_000_000:.1f}M"
    if n >= 1_000:
        return f"{n / 1_000:.0f}K"
    return str(n)


def render_bar(stages: dict, wall_time_s: float) -> str:
    """Render a proportional colored Unicode bar from stage timers.

    Stage colors:
      Optimizer      -> blue   (ANSI 34)
      Model building -> yellow (ANSI 33)
      Solver         -> red    (ANSI 31)
      Overhead       -> grey   (ANSI 90)
    """
    if not stages or wall_time_s <= 0:
        return ""

    wall_ms = wall_time_s * 1000
    opt_ms = stages.get("optimizer_ms", 0)
    model_ms = stages.get("model_construction_ms", 0)
    solver_ms = stages.get("solver_ms", 0)
    other_ms = max(0, wall_ms - opt_ms - model_ms - solver_ms)

    total = opt_ms + model_ms + solver_ms + other_ms
    if total <= 0:
        return ""

    # Compute proportional character widths
    segments = [
        ("opt", opt_ms, BLUE),
        ("model", model_ms, YELLOW),
        ("solver", solver_ms, RED),
        ("other", other_ms, GREY),
    ]

    widths: list[int] = []
    for _, ms, _ in segments:
        widths.append(max(1, round(ms / total * BAR_WIDTH)) if ms > 0 else 0)

    # Adjust to exactly BAR_WIDTH
    diff = sum(widths) - BAR_WIDTH
    if diff != 0:
        max_idx = widths.index(max(widths))
        widths[max_idx] -= diff

    # Build bar line
    bar = "  "
    for i, (_, _, color) in enumerate(segments):
        w = widths[i]
        if w > 0:
            bar += color + "\u2588" * w + RESET

    # Build legend line — center each label under its segment
    legend = "  "
    for i, (label, _, color) in enumerate(segments):
        w = widths[i]
        if w > 0:
            if w >= len(label):
                pad_left = (w - len(label)) // 2
                pad_right = w - len(label) - pad_left
                legend += " " * pad_left + f"{color}{label}{RESET}" + " " * pad_right
            else:
                legend += f"{color}{label[:w]}{RESET}"

    # Stage timing summary (always show actual numbers)
    def fmt_ms(ms: float) -> str:
        if ms >= 1000:
            return f"{ms / 1000:.1f}s"
        return f"{ms:.0f}ms" if ms >= 1 else f"{ms:.2f}ms"

    parts = []
    for label, ms, color in segments:
        if ms > 0:
            pct = ms / total * 100
            parts.append(f"{color}{label}{RESET} {fmt_ms(ms)} ({pct:.0f}%)")
    timing = "  " + "  ".join(parts)

    return f"{bar}\n{legend}\n{timing}"


def render_query_group(query_name: str, entries: list[dict]) -> str:
    """Render a query group: header + SQL + per-size metrics + bars."""
    lines: list[str] = []

    sql = entries[0].get("sql", "")
    desc = entries[0].get("description", query_name)
    display_name = f"{query_name}: {desc}" if desc != query_name else query_name

    lines.append(BOLD + "\u2501" * 60 + RESET)
    lines.append(f"{BOLD}{display_name}{RESET}")

    if sql:
        for sql_line in sql.strip().splitlines():
            lines.append(f"  {DIM}{sql_line}{RESET}")
    lines.append("")

    for entry in entries:
        size = entry["size"]
        stats = entry.get("stats", {})
        stages = stats.get("stages", {})

        median = stats.get("median_wall_time_s")
        if median is None:
            lines.append(f"  {size:<8}\u2502 FAILED")
            lines.append("")
            continue

        # Build metrics line
        metric_parts: list[str] = []

        num_rows = stages.get("num_rows")
        if num_rows:
            metric_parts.append(f"{format_count(num_rows)} rows")

        total_vars = stages.get("total_variables")
        if total_vars:
            metric_parts.append(f"{format_count(total_vars)} vars")

        # Model size in two units, never summed. `total_constraint_rows` is what the solver
        # actually received (a per-row clause expands to one row per data row, a PER clause
        # to one row per group), so it is the figure to read for matrix pressure;
        # `total_constraint_specs` is the clause-level count that points at the DECIDE
        # clause responsible. Runs recorded before the split carry only the old
        # `total_constraints`, which summed the two units and reported neither — show it
        # flagged rather than silently comparing it against a row count.
        total_rows = stages.get("total_constraint_rows")
        total_specs = stages.get("total_constraint_specs")
        legacy_cons = stages.get("total_constraints")
        if total_rows is not None:
            part = f"{format_count(total_rows)} constraint rows"
            if total_specs is not None:
                part += f" ({int(total_specs)} specs)"
            metric_parts.append(part)
        elif legacy_cons is not None:
            metric_parts.append(f"{int(legacy_cons)} constraints (pre-split, mixed unit)")

        rss_kb = stats.get("median_peak_rss_kb")
        if rss_kb:
            metric_parts.append(f"{rss_kb / 1024:.0f} MB")

        time_str = f"{median:.2f}s"
        metrics_str = " \u2502 ".join(metric_parts) if metric_parts else ""

        if metrics_str:
            lines.append(f"  {size:<8}\u2502 {metrics_str} \u2502 {time_str}")
        else:
            lines.append(f"  {size:<8}\u2502 {time_str}")

        # Stage bar
        bar = render_bar(stages, median)
        if bar:
            lines.append(bar)
        lines.append("")

    return "\n".join(lines)


def render_results(data: dict) -> str:
    """Render full results with header and all query groups."""
    lines: list[str] = []

    commit = data.get("commit", "unknown")
    timestamp = data.get("timestamp", "")
    sizes = data.get("sizes", [])

    lines.append("")
    lines.append(f"{BOLD}DecidB Benchmark Results{RESET}")
    lines.append(f"  Commit: {commit}  |  {timestamp[:19]}  |  Sizes: {', '.join(sizes)}")
    lines.append("")

    # Group entries by query name, preserving order
    groups: dict[str, list[dict]] = {}
    for entry in data.get("queries", []):
        q = entry["query"]
        groups.setdefault(q, []).append(entry)

    for query_name in groups:
        lines.append(render_query_group(query_name, groups[query_name]))

    return "\n".join(lines)


def main() -> None:
    identifier = sys.argv[1] if len(sys.argv) > 1 else None
    result_path = find_result_file(identifier)

    try:
        with open(result_path) as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"ERROR: Could not load {result_path}: {e}", file=sys.stderr)
        sys.exit(1)

    print(render_results(data))


if __name__ == "__main__":
    main()
