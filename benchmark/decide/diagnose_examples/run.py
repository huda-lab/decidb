#!/usr/bin/env python3
"""Records what a SQL user reads for each example query, plain and under DIAGNOSE, on each backend, as one Markdown
report.

Each file in `queries/` is one example: `--` comments saying what it shows, optional `SET` statements, then one DECIDE
query. A `-- database: <name>` comment picks the TPC-H database (default tpch_sf0.0125). Statements are split on `;`,
so a string literal in an example must not hold one.

For every backend the runner runs the query as written (its rows, or the message the user reads) and prefixed with
DIAGNOSE (the relation), each in a fresh `decidb` process that only opens the stored database, with
`DECIDB_FORCE_SOLVER` naming the backend. Settings are otherwise the defaults a user gets, direct solve included. Where
the backends' DIAGNOSE relations differ (numbers compared to six significant digits), the report says so. A run past
`--timeout` is killed with its process group and reported as a timeout.

Beside the report, a JSON manifest records the commit, whether the working tree was dirty, the binary and the solver
versions; the report opens with the same facts.

    python3 benchmark/decide/diagnose_examples/run.py
    python3 benchmark/decide/diagnose_examples/run.py --queries budget remove_only --backends highs
"""

import argparse
import json
import os
import platform
import re
import signal
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
BINARY = REPO / "build" / "release" / "decidb"
QUERIES = HERE / "queries"
RESULTS = HERE.parent / "results" / "diagnose_examples"
DB_DIR = HERE.parent / "databases" / "direct_solve"
DEFAULT_DATABASE = "tpch_sf0.0125"
BACKENDS = ("gurobi", "highs")
SHOWN_ROWS = 5  # rows of a plain run's result printed in the report
ERROR = re.compile(r"^(?:[A-Z][A-Za-z ]*)?Error: ")


def load_example(name):
    """Returns (description, database name, setup statements, DECIDE query) from queries/<name>.sql."""
    lines = (QUERIES / f"{name}.sql").read_text().splitlines()
    comments = [line[2:].strip() for line in lines if line.startswith("--")]
    database = next((c.split(":", 1)[1].strip() for c in comments if c.startswith("database:")), DEFAULT_DATABASE)
    description = " ".join(c for c in comments if not c.startswith("database:"))
    body = "\n".join(line for line in lines if not line.startswith("--"))
    *setup, query = [statement.strip() for statement in body.split(";") if statement.strip()]
    return description, database, setup, query


def prepare(db):
    """Generates the TPC-H tables of `tpch_sf<sf>.db` once; a stored database is reused."""
    if db.exists():
        return
    sf = float(db.stem.removeprefix("tpch_sf"))
    db.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(BINARY), str(db), "-c", f"CALL dbgen(sf={sf}); CHECKPOINT;"], check=True, capture_output=True)


def run(db, backend, sql, timeout):
    """One fresh process. Returns {"rows": [...]} for a relation, else {"error": message}; either may carry
    "warnings", the other lines the run printed to stderr."""
    env = {**os.environ, "DECIDB_FORCE_SOLVER": backend}
    command = [str(BINARY), str(db), "-readonly", "-json", "-c", sql]
    proc = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env, start_new_session=True
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.communicate()
        return {"error": f"timeout: killed after {timeout}s"}
    lines = [line for line in stderr.splitlines() if line.strip()]
    errors = [line for line in lines if ERROR.match(line)]
    outcome = {"warnings": [line for line in lines if not ERROR.match(line)]}
    if errors or proc.returncode != 0:
        # The first line is the message; DuckDB follows an internal error with a stack trace.
        outcome["error"] = errors[0] if errors else f"exit status {proc.returncode}"
    else:
        outcome["rows"] = json.loads(stdout) if stdout.strip() else []
    return outcome


def normalized(outcome):
    """The outcome as compared across backends: rows in a fixed order, numbers to six significant digits."""
    if "rows" not in outcome:
        return outcome["error"]

    def cell(value):
        return f"{value:.6g}" if isinstance(value, float) else value

    return sorted((tuple((k, cell(v)) for k, v in row.items()) for row in outcome["rows"]), key=repr)


def table(rows):
    """A Markdown table of JSON rows."""
    if not rows:
        return "_(no rows)_"

    def cell(value):
        return "NULL" if value is None else str(value).replace("|", "\\|")

    columns = list(rows[0])
    out = ["| " + " | ".join(columns) + " |", "|" + "---|" * len(columns)]
    out += ["| " + " | ".join(cell(row[c]) for c in columns) + " |" for row in rows]
    return "\n".join(out)


def render(outcome, shown=None):
    """An outcome as Markdown: the message the user reads, or the rows (the first `shown` of them)."""
    out = [f"> {warning}" for warning in outcome.get("warnings", [])]
    if "error" in outcome:
        out.append(f"```\n{outcome['error']}\n```")
        return "\n\n".join(out)
    rows = outcome["rows"]
    if shown is not None and len(rows) > shown:
        out.append(f"{len(rows)} rows; the first {shown}:")
        rows = rows[:shown]
    out.append(table(rows))
    return "\n\n".join(out)


def command_output(command, cwd=None):
    try:
        return subprocess.run(command, capture_output=True, text=True, cwd=cwd, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def manifest(args, stamp, output):
    """The setup a reader needs to reproduce or compare the run."""
    highs = dict(
        re.findall(
            r"#define HIGHS_VERSION_(MAJOR|MINOR|PATCH) (\d+)", (REPO / "third_party/highs/HConfig.h").read_text()
        )
    )
    gurobi = command_output(["gurobi_cl", "--version"])
    return {
        "command": "benchmark/decide/diagnose_examples/run.py",
        "generated_at_utc": stamp,
        "output": str(output),
        "arguments": {k: (str(v) if isinstance(v, Path) else v) for k, v in vars(args).items()},
        "commit": command_output(["git", "rev-parse", "HEAD"], cwd=REPO),
        # what decides the output: the engine, the bundled solvers, and this runner's own code and examples
        "working_tree_dirty": bool(
            command_output(["git", "status", "--porcelain", "--", "src", "third_party", str(HERE)], cwd=REPO)
        ),
        "binary": str(BINARY),
        "binary_built": datetime.fromtimestamp(BINARY.stat().st_mtime, timezone.utc).isoformat(),
        "decidb_version": command_output([str(BINARY), "-noheader", "-csv", "-c", "SELECT version();"]),
        "gurobi": gurobi.splitlines()[0] if gurobi else None,
        "highs": f"{highs['MAJOR']}.{highs['MINOR']}.{highs['PATCH']}",
        "platform": platform.platform(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    names = sorted(p.stem for p in QUERIES.glob("*.sql"))
    parser.add_argument("--queries", nargs="+", choices=names, default=names)
    parser.add_argument("--backends", nargs="+", choices=BACKENDS, default=list(BACKENDS))
    parser.add_argument("--timeout", type=int, default=120, help="seconds before a run is killed and reported")
    parser.add_argument("--db-dir", type=Path, default=DB_DIR)
    args = parser.parse_args()
    if not BINARY.exists():
        parser.error(f"build the release CLI first: {BINARY}")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    RESULTS.mkdir(parents=True, exist_ok=True)
    output = RESULTS / f"diagnose_examples_{stamp}.md"
    facts = manifest(args, stamp, output)
    output.with_suffix(".json").write_text(json.dumps(facts, indent=2) + "\n")

    report = [
        "# DIAGNOSE examples",
        "",
        f"Commit `{facts['commit']}`{' (working tree dirty)' if facts['working_tree_dirty'] else ''}, "
        f"binary `{facts['binary']}` built {facts['binary_built']}, {facts['decidb_version']}, "
        f"{facts['gurobi']}, HiGHS {facts['highs']}. Generated {stamp}.",
    ]
    differing = []
    for name in args.queries:
        description, database, setup, query = load_example(name)
        db = args.db_dir / f"{database}.db"
        prepare(db)
        prefix = "".join(f"{statement};\n" for statement in setup)
        report += [
            "",
            f"## {name}",
            "",
            description,
            "",
            f"Database `{database}`.",
            "",
            f"```sql\n{prefix}{query};\n```",
        ]
        diagnosed = {}
        for backend in args.backends:
            print(f"{name} [{backend}]", file=sys.stderr)
            plain = run(db, backend, f"{prefix}{query};", args.timeout)
            diagnosed[backend] = run(db, backend, f"{prefix}DIAGNOSE {query};", args.timeout)
            report += [
                "",
                f"### {backend}",
                "",
                "Plain run:",
                "",
                render(plain, SHOWN_ROWS),
                "",
                "DIAGNOSE:",
                "",
                render(diagnosed[backend]),
            ]
        if len({json.dumps(normalized(o)) for o in diagnosed.values()}) > 1:
            differing.append(name)
            report += ["", f"**The DIAGNOSE relations differ between {' and '.join(diagnosed)}.**"]
    report[3:3] = [
        "",
        f"DIAGNOSE relations that differ between backends: {', '.join(differing) if differing else 'none'}.",
    ]
    output.write_text("\n".join(report) + "\n")
    print(f"wrote {output} and {output.with_suffix('.json')}", file=sys.stderr)


if __name__ == "__main__":
    main()
