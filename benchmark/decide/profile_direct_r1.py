#!/usr/bin/env python3
"""Times direct solve against Gurobi and HiGHS on R1 shapes (continuous knapsack: one budget over REAL decisions).

Each measured run is a fresh `decidb` process that only opens the stored database, so its peak memory
(`maximum resident set size`, from `/usr/bin/time -l`) is the query's own. `direct` pins
`decide_direct_solve='require'`, so a query the rule does not apply to fails instead of using the solver; the solver
runs pin `'off'` and choose the backend with the environment variable `DECIDB_FORCE_SOLVER` (`gurobi` or `highs`),
the only switch the repo has for it. Results are read back as one aggregate row, so the time is the plan, not the
transfer of millions of rows.

A run that exceeds `--timeout` is killed (with its whole process group) and reported as `timeout`; a run that fails
is reported as `error`. Neither is ever reported as a time.

Before any timing, each shape is run at the smallest size on direct and on Gurobi, and the two objectives must agree
within 1e-6 relative; a mismatch is printed loudly and the script exits non-zero.

Machine-heat rule: shapes run one at a time, smallest size first, and a size runs on a solver only when that solver
finished the previous size of the shape within `--solver-budget` seconds.

    python3 benchmark/decide/profile_direct_r1.py --rows 100000 1000000 --db-dir /tmp/r1db
"""

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BINARY = HERE.parent.parent / "build" / "release" / "decidb"

GROUPS = 1000
FRACTION = 0.3  # a budget binds at this share of the full spend

# name -> (DECIDE clause template, what the parent reads, objective expression the agreement check sums).
# `{name}` placeholders are filled from the budgets that `prepare` stores beside the database.
BOX = "x BETWEEN 0 AND cap"
SHAPES = {
    "le_positive": (
        f"x(REAL) SUCH THAT {BOX} AND SUM(w_pos*x) <= {{spend_30}} MAXIMIZE SUM(v*x)",
        "COUNT(*), SUM(x)",
        "SUM(v*x)",
    ),
    "le_mixed_signs": (
        f"x(REAL) SUCH THAT {BOX} AND SUM(w_mix*x) <= {{spend_mix_30}} MAXIMIZE SUM(v_mix*x)",
        "COUNT(*), SUM(x)",
        "SUM(v_mix*x)",
    ),
    "ge_cover": (
        f"x(REAL) SUCH THAT {BOX} AND SUM(w_pos*x) >= {{spend_30}} MINIMIZE SUM(v*x)",
        "COUNT(*), SUM(x)",
        "SUM(v*x)",
    ),
    "eq_fill": (
        f"x(REAL) SUCH THAT {BOX} AND SUM(w_pos*x) = {{spend_30}} MAXIMIZE SUM(v*x)",
        "COUNT(*), SUM(x)",
        "SUM(v*x)",
    ),
    "between": (
        f"x(REAL) SUCH THAT {BOX} AND SUM(w_pos*x) BETWEEN {{spend_30}} AND {{spend_60}} MAXIMIZE SUM(v*x)",
        "COUNT(*), SUM(x)",
        "SUM(v*x)",
    ),
    "per_groups": (
        f"x(REAL) SUCH THAT {BOX} AND SUM(w_pos*x) <= {{spend_group_30}} PER grp MAXIMIZE SUM(v*x)",
        "COUNT(*), SUM(x)",
        "SUM(v*x)",
    ),
    "when_budget": (
        f"x(REAL) SUCH THAT {BOX} AND SUM(w_pos*x) <= {{spend_when_30}} WHEN f MAXIMIZE SUM(v*x)",
        "COUNT(*), SUM(x)",
        "SUM(v*x)",
    ),
    "two_decisions": (
        "x(REAL), y(REAL) SUCH THAT x BETWEEN 0 AND cap AND y BETWEEN 0 AND cap_y "
        "AND SUM(w_pos*x + w_y*y) <= {spend_two_30} MAXIMIZE SUM(v*x + v_y*y)",
        "COUNT(*), SUM(x), SUM(y)",
        "SUM(v*x + v_y*y)",
    ),
    "column_budget": (
        f"x(REAL) SUCH THAT {BOX} AND SUM(w_pos*x) <= budget_col PER grp MAXIMIZE SUM(v*x)",
        "COUNT(*), SUM(x)",
        "SUM(v*x)",
    ),
    "aggregate_budget": (
        f"x(REAL) SUCH THAT {BOX} AND SUM(w_pos*x) <= 0.3*SUM(w_pos*cap) MAXIMIZE SUM(v*x)",
        "COUNT(*), SUM(x)",
        "SUM(v*x)",
    ),
    "count_only": (
        f"x(REAL) SUCH THAT {BOX} AND SUM(w_pos*x) <= {{spend_30}} MAXIMIZE SUM(v*x)",
        "COUNT(*)",
        "SUM(v*x)",
    ),
}
SOLVERS = ("gurobi", "highs")


def budgets_path(db):
    return db.with_suffix(".budgets.json")


def prepare(db, rows):
    """Builds the source table once per size and stores the budget constants that bind at this size."""
    sql = f"""
        DROP TABLE IF EXISTS src;
        CREATE TABLE src AS
        WITH base AS (
            SELECT i AS id,
                (1 + (i * 37) % 19 / 2.0)::DOUBLE AS w_pos,
                (((i * 13) % 21) - 8) / 4.0 AS w_mix,
                (1 + (i * 53) % 97 / 10.0)::DOUBLE AS v,
                ((((i * 53) % 97) - 30) / 10.0)::DOUBLE AS v_mix,
                (5 + (i % 23))::DOUBLE AS cap,
                0.0::DOUBLE AS lo,
                (i % {GROUPS})::INTEGER AS grp,
                (i % 3 != 0) AS f,
                (1 + (i * 29) % 17 / 2.0)::DOUBLE AS w_y,
                (1 + (i * 71) % 89 / 10.0)::DOUBLE AS v_y,
                (4 + (i % 19))::DOUBLE AS cap_y
            FROM range({rows}) r(i)
        )
        SELECT *, {FRACTION} * SUM(w_pos * cap) OVER (PARTITION BY grp) AS budget_col FROM base;
        CHECKPOINT;
    """
    subprocess.run([str(BINARY), str(db)], input=sql, text=True, check=True, capture_output=True)
    totals = f"""
        SELECT SUM(w_pos * cap), SUM(GREATEST(w_mix, 0) * cap), SUM(w_pos * cap) FILTER (f),
               SUM(w_pos * cap + w_y * cap_y)
        FROM src;
    """
    out = subprocess.run(
        [str(BINARY), str(db), "-noheader", "-csv", "-c", totals], text=True, check=True, capture_output=True
    ).stdout
    spend, spend_mix, spend_when, spend_two = (float(v) for v in out.strip().splitlines()[-1].split(","))
    budgets = {
        "spend_30": FRACTION * spend,
        "spend_60": 2 * FRACTION * spend,
        "spend_mix_30": FRACTION * spend_mix,
        "spend_group_30": FRACTION * spend / GROUPS,
        "spend_when_30": FRACTION * spend_when,
        "spend_two_30": FRACTION * spend_two,
    }
    budgets_path(db).write_text(json.dumps(budgets))


def query_sql(db, shape, read):
    decide = SHAPES[shape][0].format(**json.loads(budgets_path(db).read_text()))
    return f"SELECT {read} FROM (SELECT * FROM src DECIDE {decide}) q;"


def run(db, mode, solver, sql, timeout):
    """One fresh process. Returns (wall seconds, peak MiB, status, stdout); status is None when the run finished."""
    setting = "require" if mode == "direct" else "off"
    env = {k: v for k, v in os.environ.items() if k != "DECIDB_FORCE_SOLVER"}
    if mode == "solver":
        env["DECIDB_FORCE_SOLVER"] = solver
    command = [
        "/usr/bin/time",
        "-l",
        str(BINARY),
        str(db),
        "-readonly",
        "-noheader",
        "-csv",
        "-c",
        f"SET decide_direct_solve='{setting}'; {sql}",
    ]
    start = time.perf_counter()
    proc = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env, start_new_session=True
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)  # the whole group: `time` and the decidb it started
        proc.communicate()
        return None, None, "timeout", ""
    wall = time.perf_counter() - start
    peak = re.search(r"(\d+)\s+maximum resident set size", stderr)
    peak_mib = int(peak.group(1)) / (1024 * 1024) if peak else None
    status = None
    before_stats = stderr.split("real")[0]
    if proc.returncode != 0 or "Error" in before_stats:
        status = "error"
        detail = before_stats.strip().splitlines()[0][:160] if before_stats.strip() else "failed"
        print(f"  [{mode} {solver or ''}] error: {detail}", file=sys.stderr)
    return wall, peak_mib, status, stdout


def check_agreement(db, shapes, timeout):
    """Direct and Gurobi must reach the same objective at the smallest size; returns the shapes that disagree."""
    bad = []
    for shape in shapes:
        sql = query_sql(db, shape, SHAPES[shape][2])
        d = run(db, "direct", None, sql, timeout)
        g = run(db, "solver", "gurobi", sql, timeout)
        try:
            dv, gv = (float(r[3].strip().splitlines()[-1]) for r in (d, g))
        except (ValueError, IndexError):
            bad.append((shape, f"direct {d[2] or 'ok'}, gurobi {g[2] or 'ok'}, no objective to compare"))
            continue
        if abs(dv - gv) > 1e-6 * max(1.0, abs(dv), abs(gv)):
            bad.append((shape, f"direct {dv!r} vs gurobi {gv!r}"))
        else:
            print(f"agree {shape}: {dv!r}", file=sys.stderr)
    return bad


def measure(db, mode, solver, shape, repeats, timeout):
    """Median wall time and max peak over finished repeats, or the status of the first failure."""
    sql = query_sql(db, shape, SHAPES[shape][1])
    runs = []
    for _ in range(repeats if mode == "direct" else 1):
        wall, peak, status, _ = run(db, mode, solver, sql, timeout)
        if status:
            return status, None
        runs.append((wall, peak))
    wall = sorted(r[0] for r in runs)[len(runs) // 2]
    return wall, max(r[1] for r in runs if r[1] is not None)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rows", nargs="+", type=int, default=[100000])
    parser.add_argument("--shapes", nargs="+", choices=sorted(SHAPES), default=list(SHAPES))
    parser.add_argument("--solver", nargs="+", choices=SOLVERS, default=list(SOLVERS))
    parser.add_argument("--repeats", type=int, default=2, help="runs of direct per cell; a solver runs once")
    parser.add_argument("--timeout", type=int, default=300, help="seconds before a run is killed and reported")
    parser.add_argument(
        "--solver-budget",
        type=float,
        default=15.0,
        help="run a solver at a size only if its previous (smaller) run took at most this long",
    )
    parser.add_argument("--db-dir", type=Path, required=True)
    args = parser.parse_args()
    args.db_dir.mkdir(parents=True, exist_ok=True)
    sizes = sorted(args.rows)
    dbs = {}
    for rows in sizes:
        dbs[rows] = args.db_dir / f"r1_{rows}.db"
        if not dbs[rows].exists() or not budgets_path(dbs[rows]).exists():
            dbs[rows].unlink(missing_ok=True)
            prepare(dbs[rows], rows)

    bad = check_agreement(dbs[sizes[0]], args.shapes, args.timeout)
    if bad:
        for shape, why in bad:
            print(f"!!!!!! MISMATCH {shape}: {why} !!!!!!", file=sys.stderr)
        sys.exit(1)

    columns = ["shape", "rows", "direct s", "direct peak MiB"]
    for solver in args.solver:
        columns += [f"{solver} s", f"{solver} peak MiB"]
    columns += [f"speedup vs {solver}" for solver in args.solver]
    print("| " + " | ".join(columns) + " |")
    print("|---|---:" + "|---:" * (len(columns) - 2) + "|")

    def fmt(v):
        return f"{v:.2f}" if isinstance(v, float) else (v or "-")

    last = {}  # (solver, shape) -> last status or time, for the machine-heat rule
    for rows in sizes:
        for shape in args.shapes:
            direct = measure(dbs[rows], "direct", None, shape, args.repeats, args.timeout)
            cells = [shape, str(rows), fmt(direct[0]), fmt(direct[1])]
            speeds = []
            for solver in args.solver:
                previous = last.get((solver, shape), 0.0)
                if not isinstance(previous, float) or previous > args.solver_budget:
                    reason = previous if isinstance(previous, str) else "previous size too slow"
                    result = (f"skipped ({reason})", None)
                else:
                    result = measure(dbs[rows], "solver", solver, shape, args.repeats, args.timeout)
                    last[(solver, shape)] = result[0]
                cells += [fmt(result[0]), fmt(result[1])]
                finished = isinstance(result[0], float) and isinstance(direct[0], float)
                speeds.append(f"{result[0] / direct[0]:.1f}x" if finished else "-")
            print("| " + " | ".join(cells + speeds) + " |")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
