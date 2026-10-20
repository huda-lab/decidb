#!/usr/bin/env python3
"""Per-test branch coverage report for the direct-solve code (H-30).

Two subcommands, standard library only:

  coverage_report.py slice [--s1 N] [--a1 N] [--conformance N]
      Print node ids spread evenly over the S1 oracle grid, the A1 file and the rule
      conformance file. This is the slice used to time the full run.

  coverage_report.py report COVERAGE_DIR [--tests] [--min-unique N]
      Read the per-test profiles that ``run_coverage.sh`` left in COVERAGE_DIR/profiles and
      print which branches each test reaches and which only that test (or only that
      category) reaches. Writes COVERAGE_DIR/report.json for later comparison.

Vocabulary
  arm       one outcome of a branch condition in the source: the true side or the false side
            of an ``if``, ``&&``, ``||``, ``?:``, loop test and so on. One condition gives
            two arms. Arms are identified by file, line and column of the condition plus the
            side, so they stay comparable between runs while the code does not change.
  category  one test function with its parameters stripped, e.g.
            ``test_direct_a1_separable_box.py::test_a1_oracle_solver_and_direct_agree``.
            (A parametrized grid is one category.)
  unique    an arm that exactly one test (or exactly one category) reaches.

The instrumented files are exactly the ones listed in ``SCOPE`` below. The instrumented binary
also carries a handful of header template functions that the toolchain instruments as a side
effect; those are ignored here because they are not in ``SCOPE``.
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BINARY = REPO / "build" / "coverage" / "decidb"
SCOPE_DIR = REPO / "src" / "optimizer" / "decide" / "direct"
SCOPE = sorted(SCOPE_DIR.glob("*.cpp")) + [REPO / "src" / "optimizer" / "decide" / "decide_rewrite_rhs_window.cpp"]

_KIND_BRANCH = 4  # llvm-cov branch region


def _tool(name: str) -> str:
    return subprocess.run(["xcrun", "-f", name], capture_output=True, text=True, check=True).stdout.strip()


# ---------------------------------------------------------------------------
# slice
# ---------------------------------------------------------------------------


def _evenly(items: list[str], n: int) -> list[str]:
    if n >= len(items):
        return items
    return [items[(2 * i + 1) * len(items) // (2 * n)] for i in range(n)]


def cmd_slice(args) -> int:
    out = subprocess.run(
        [
            sys.executable,
            "-m",
            "pytest",
            "--collect-only",
            "-q",
            "tests/test_direct_s1_uniform_matroid.py",
            "tests/test_direct_a1_separable_box.py",
            "tests/test_direct_rule_conformance.py",
        ],
        cwd=HERE,
        capture_output=True,
        text=True,
    ).stdout
    ids = [line for line in out.splitlines() if "::" in line]
    s1 = [i for i in ids if "test_direct_s1_uniform_matroid.py" in i and "oracle_solver_and_direct_agree" in i]
    a1 = [i for i in ids if "test_direct_a1_separable_box.py" in i]
    conf = [i for i in ids if "test_direct_rule_conformance.py" in i]
    for i in _evenly(s1, args.s1) + _evenly(a1, args.a1) + _evenly(conf, args.conformance):
        print(i)
    return 0


# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------


def _short(path: str) -> str:
    return path.split("/src/optimizer/decide/", 1)[-1]


def _export(profdata: Path) -> tuple[set[tuple], set[tuple]]:
    """(all arms, covered arms) of the in-scope files for one profile."""
    out = subprocess.run(
        [
            _tool("llvm-cov"),
            "export",
            str(BINARY),
            f"-instr-profile={profdata}",
            "-format=text",
            "-skip-expansions",
            *map(str, SCOPE),
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    arms, covered = set(), set()
    for f in json.loads(out)["data"][0]["files"]:
        name = _short(f["filename"])
        for l1, c1, l2, c2, true_n, false_n, _fid, _xid, kind in f["branches"]:
            if kind != _KIND_BRANCH:
                continue
            for side, n in (("T", true_n), ("F", false_n)):
                arm = (name, l1, c1, l2, c2, side)
                arms.add(arm)
                if n > 0:
                    covered.add(arm)
    return arms, covered


def _category(nodeid: str) -> str:
    path, _, rest = nodeid.partition("::")
    function = re.sub(r"\[.*$", "", rest)
    return f"{Path(path).name}::{function}"


def _label(arm: tuple, source_cache: dict) -> str:
    name, l1, c1, _l2, _c2, side = arm
    path = REPO / "src" / "optimizer" / "decide" / name
    lines = source_cache.setdefault(path, path.read_text().splitlines())
    text = lines[l1 - 1].strip()
    return f"{name}:{l1}:{c1} {side}  {text[:90]}"


def cmd_report(args) -> int:
    root = Path(args.dir)
    meta = {}
    for js in sorted((root / "profiles").glob("*.json")):
        m = json.loads(js.read_text())
        m["profdata"] = js.with_suffix(".profdata")
        meta[m["nodeid"]] = m
    usable = {k: m for k, m in meta.items() if m["merged"]}
    if not usable:
        print("no usable profiles in", root / "profiles")
        return 1
    stray = list((root / "stray").glob("*.profraw")) if (root / "stray").exists() else []

    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(lambda m: _export(m["profdata"]), usable.values()))
    universe: set[tuple] = set()
    reached: dict[str, set[tuple]] = {}
    for nodeid, (arms, covered) in zip(usable, results):
        universe |= arms
        reached[nodeid] = covered

    by_arm: dict[tuple, list[str]] = collections.defaultdict(list)
    for nodeid, arms in reached.items():
        for arm in arms:
            by_arm[arm].append(nodeid)
    union = set(by_arm)

    cat_of = {n: _category(n) for n in reached}
    cat_tests: dict[str, list[str]] = collections.defaultdict(list)
    for n, c in cat_of.items():
        cat_tests[c].append(n)
    cat_reached = {c: set().union(*(reached[t] for t in ts)) for c, ts in cat_tests.items()}
    cats_of_arm = collections.defaultdict(set)
    for c, arms in cat_reached.items():
        for a in arms:
            cats_of_arm[a].add(c)

    unique_test = {n: {a for a in arms if len(by_arm[a]) == 1} for n, arms in reached.items()}
    unique_cat = {c: {a for a in arms if len(cats_of_arm[a]) == 1} for c, arms in cat_reached.items()}

    src_cache: dict = {}
    print(
        f"tests profiled: {len(usable)} of {len(meta)} (merged)   not passed: "
        f"{sum(1 for m in meta.values() if m['outcome'] != 'passed')}   stray raw profiles: {len(stray)}"
    )
    print(
        f"arms in scope: {len(universe)}   reached by at least one test: {len(union)}   "
        f"never reached: {len(universe) - len(union)}\n"
    )

    print("Per file (arms reached by the whole set):")
    per_file_all = collections.Counter(a[0] for a in universe)
    per_file_hit = collections.Counter(a[0] for a in union)
    for name in sorted(per_file_all):
        print(f"  {name:55s} {per_file_hit[name]:4d} / {per_file_all[name]:4d}")

    print("\nPer category (tests; arms reached; arms only this category reaches; tests with no unique arm):")
    for c in sorted(cat_tests):
        ts = cat_tests[c]
        zero = sum(1 for t in ts if not unique_test[t])
        print(
            f"  {c:95s} {len(ts):4d} tests {len(cat_reached[c]):4d} reached {len(unique_cat[c]):3d} category-unique"
            f" {zero:4d} with no unique arm"
        )
        if args.tests:
            for a in sorted(unique_cat[c]):
                print(f"      category-unique: {_label(a, src_cache)}")

    print("\nPer test (arms reached; arms only this test reaches):")
    for n in sorted(reached, key=lambda n: (cat_of[n], n)):
        if len(unique_test[n]) < args.min_unique and not args.tests:
            continue
        print(f"  {n}  reached={len(reached[n])} unique={len(unique_test[n])}  ({usable[n]['seconds']}s)")
        for a in sorted(unique_test[n]):
            print(f"      only here: {_label(a, src_cache)}")

    seconds = [m["seconds"] for m in usable.values()]
    print(
        f"\ntime per test: mean {sum(seconds) / len(seconds):.3f}s over {len(seconds)} tests "
        f"(sum {sum(seconds):.1f}s inside the fixture window)"
    )
    sizes = [m["profdata_bytes"] for m in usable.values()]
    print(
        f"profile size: mean {sum(sizes) / len(sizes) / 1024:.0f} KB, max {max(sizes) / 1024:.0f} KB, "
        f"total {sum(sizes) / 1e6:.1f} MB"
    )

    index = sorted(universe)
    pos = {a: i for i, a in enumerate(index)}
    (root / "report.json").write_text(
        json.dumps(
            {
                "arms": [list(a) for a in index],
                "tests": {
                    n: {
                        "category": cat_of[n],
                        "outcome": usable[n]["outcome"],
                        "seconds": usable[n]["seconds"],
                        "reached": sorted(pos[a] for a in reached[n]),
                        "unique": sorted(pos[a] for a in unique_test[n]),
                    }
                    for n in reached
                },
            }
        )
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("slice")
    s.add_argument("--s1", type=int, default=25)
    s.add_argument("--a1", type=int, default=15)
    s.add_argument("--conformance", type=int, default=10)
    r = sub.add_parser("report")
    r.add_argument("dir")
    r.add_argument("--tests", action="store_true", help="list every test and the arms only it reaches")
    r.add_argument("--min-unique", type=int, default=1, help="list tests with at least this many unique arms")
    args = ap.parse_args()
    return cmd_slice(args) if args.cmd == "slice" else cmd_report(args)


if __name__ == "__main__":
    sys.exit(main())
