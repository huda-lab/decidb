"""Same query at 1 and 8 threads over a stored table spanning several row groups, on both binaries.

The objective value at the optimum is unique even when the optimal assignment is not, so it is what is compared.
A crash, an INTERNAL error or a different objective between thread counts is a failure of the merge.

Usage: threads_test.py [rows]
"""

import os
import re
import subprocess
import sys
import time
from pathlib import Path

WORKTREES = Path("/Users/hatim/Code/decidb/.claude/worktrees")
BINARIES = {"anr": WORKTREES / "sami-anr/build/release/decidb", "opa": WORKTREES / "sami-opa/build/release/decidb"}
N = int(sys.argv[1]) if len(sys.argv) > 1 else 500_000

SRC = f"""CREATE TABLE src AS SELECT i AS id, i % 100 AS part, i // 100 AS p, (i * 7919) % 13 AS w,
    i % 10 AS g1, (i // 10) % 10 AS g2, (i // 100) % 10 AS g3, i % 7 AS k FROM range({N}) r(i);"""

# name -> (decisions, constraints, objective, SQL giving the objective's value from the result r joined to src)
QUERIES = {
    "flat-sum": ("x(REAL)", "x <= 10 AND per t.g1: sum(x) by (t.g1) <= 1000", "MAXIMIZE sum(t.w * x)", "sum(w * x)"),
    "nested-max-in-sum": (
        "x(REAL)",
        "x <= 10 AND per t.g1: sum(max(x) by (t.g1, t.g2)) by (t.g1) <= 50",
        "MAXIMIZE sum(t.w * x)",
        "sum(w * x)",
    ),
    "abs-per-class": (
        "x(REAL)",
        "x <= 10 AND x >= -10 AND abs(sum(x) by (t.g1)) <= 5",
        "MAXIMIZE sum(t.w * x)",
        "sum(w * x)",
    ),
    "keyed-decision": (
        "x(REAL), per t.g1: y(REAL)",
        "x <= y AND y <= 7 AND per t.g2: sum(x) by (t.g2) <= 100000",
        "MAXIMIZE sum(t.w * x)",
        "sum(w * x)",
    ),
    "frame-point": (
        "x(REAL)",
        "x <= 10 AND x <= at(previous else 0: x) over (t.p within t.part) + t.w",
        "MAXIMIZE sum(x)",
        "sum(x)",
    ),
    "frame-range": (
        "x(REAL)",
        "x <= 10 AND x <= avg(from 5 previous to previous else 0: x) over (t.p within t.part) + t.w",
        "MAXIMIZE sum(x)",
        "sum(x)",
    ),
    # The two shapes the execution-model review singled out as merge risks.
    "abs-of-keyed-in-class": (
        "per t.k: z(REAL)",
        "z <= 10 AND z >= -10 AND per t.g1: sum(abs(z - 2)) by (t.g1) <= 50",
        "MAXIMIZE sum(t.w * z)",
        "sum(w * z)",
    ),
    "abs-under-two-reducers": (
        "x(REAL), per t.g1: y(REAL)",
        "x <= 3 AND y <= 10 AND y >= -10 AND sum(x + abs(y - 2)) by (t.g2) <= 200000 "
        "AND sum(x + abs(y - 2)) by (t.g3) <= 300000",
        "MAXIMIZE sum(t.w * x)",
        "sum(w * x)",
    ),
}


def run(binary: Path, name: str, threads: int) -> str:
    decisions, constraints, objective, value = QUERIES[name]
    cols = "t.id, " + ", ".join(d.split("(")[0].split(":")[-1].strip() for d in decisions.split(", "))
    sql = f"SELECT {cols} FROM src t DECIDE {decisions} SUCH THAT {constraints} {objective}"
    script = (
        f"SET decide_frame_limit = 1000000000;\nSET decide_direct_solve = 'off';\n{SRC}\nSET threads = {threads};\n"
        f"CREATE TEMP TABLE r AS {sql};\n"
        f".mode list\nSELECT 'RESULT', count(*), round({value}, 4) FROM r JOIN src USING (id);\n"
    )
    env = {key: value for key, value in os.environ.items() if not key.startswith("DECIDB_")}
    env.update(DECIDB_PROFILE="1", DECIDB_TIME_LIMIT="300")
    start = time.monotonic()
    try:
        done = subprocess.run(
            [str(binary), ":memory:"], input=script, capture_output=True, text=True, env=env, timeout=900
        )
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    took = time.monotonic() - start
    result = re.search(r"RESULT\|(.*)", done.stdout)
    if done.returncode != 0 or not result:
        errors = [line for line in (done.stderr + done.stdout).splitlines() if "Error" in line or "INTERNAL" in line]
        message = errors[0][:150] if errors else f"exit {done.returncode}, no result"
        return f"FAIL({done.returncode}) {message}"
    merge = "merge" if "descent_merge" in done.stderr else "-"
    return f"{result.group(1)} [{took:.1f}s {merge}]"


def main() -> None:
    only = os.environ.get("ONLY", "")
    for name in QUERIES:
        if only and only not in name:
            continue
        for label, binary in BINARIES.items():
            one = run(binary, name, 1)
            eight = [run(binary, name, 8) for _ in range(2)]
            values = {text.split(" [")[0] for text in (one, *eight)}
            verdict = "same" if len(values) == 1 and not one.startswith(("FAIL", "TIMEOUT")) else "DIFFERENT/FAIL"
            print(f"{name:24} {label}  {verdict}\n    t=1: {one}\n    t=8: {eight[0]}\n    t=8: {eight[1]}", flush=True)


if __name__ == "__main__":
    main()
