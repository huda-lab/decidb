"""``python -m fuzz MODULE --seed S --count N``: see the package's docstring."""

from __future__ import annotations

import argparse
import os
import sys
from collections import Counter

from .runner import MODULES, load, run


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="python -m fuzz", description=__doc__)
    parser.add_argument("module", choices=(*MODULES, "all"))
    parser.add_argument("--seed", type=int, default=1, help="the first seed")
    parser.add_argument("--count", type=int, default=100, help="how many seeds")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--solver", choices=("gurobi", "highs"), help="force DeciDB's solver")
    parser.add_argument("--broken", help="a wrong oracle of the module, which must report mismatches")
    parser.add_argument("--show", type=int, default=3, help="scripts printed per category of new failure")
    return parser.parse_args()


def main() -> int:
    options = arguments()
    names = MODULES if options.module == "all" else (options.module,)
    failed = False
    for name in names:
        if options.broken and options.broken not in load(name).BROKEN:
            sys.exit(f"{name} has no broken oracle {options.broken!r}: {', '.join(load(name).BROKEN)}")
        seeds = range(options.seed, options.seed + options.count)
        report = run(name, seeds, broken=options.broken, solver=options.solver, jobs=options.jobs)
        print(f"{name}: seeds {seeds.start}..{seeds.stop - 1}: {dict(sorted(report.counts.items()))}")
        for message, count in report.refusals.most_common():
            print(f"  refused {count}: {message}")
        known: dict = {}
        for failure in report.failures:
            if failure.known:
                known.setdefault(failure.known, set()).add(failure.seed)
        for defect, failing in sorted(known.items()):
            print(f"  known {defect}: seeds {', '.join(map(str, sorted(failing)))}")
        new = [failure for failure in report.failures if not failure.known]
        shown = Counter()
        for failure in new:
            shown[failure.category] += 1
            if shown[failure.category] <= options.show:
                print(f"\n{failure}")
        failed = failed or bool(new)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
