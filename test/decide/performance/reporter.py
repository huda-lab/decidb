"""CLI table printer for performance results."""

from __future__ import annotations

from .tracker import PerfTracker


def print_perf_table(tracker: PerfTracker) -> None:
    """Print a summary table of performance records to stdout."""
    records = tracker.records
    if not records:
        print("\n  No performance records collected.\n")
        return

    # Column widths
    name_w = max(len(r.test_name) for r in records)
    name_w = max(name_w, 9)  # "Test Name"

    header = (
        f"  {'Test Name':<{name_w}}  {'Rows':>7}  {'Vars':>7}  "
        f"{'DecidB(s)':>10}  {'Oracle(s)':>10}  {'Obj Value':>14}  "
        f"{'Match':>10}  {'Solver':<8}"
    )
    sep = "  " + "-" * (len(header) - 2)

    lines = ["\n  Performance Summary", sep, header, sep]
    for r in records:
        obj_str = f"{r.objective_value:.2f}" if r.objective_value is not None else "N/A"
        oracle_total = r.oracle_build_time_s + r.oracle_solve_time_s
        cmp_str = r.comparison_status or "-"
        lines.append(
            f"  {r.test_name:<{name_w}}  {r.num_input_rows:>7}  "
            f"{r.num_solver_variables:>7}  {r.decidb_wall_time_s:>10.4f}  "
            f"{oracle_total:>10.4f}  {obj_str:>14}  "
            f"{cmp_str:>10}  {r.solver_backend:<8}"
        )
    lines.append(sep)
    lines.append("")

    print("\n".join(lines))
