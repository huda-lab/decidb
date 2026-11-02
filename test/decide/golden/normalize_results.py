#!/usr/bin/env python3
"""Reduce a golden results file (stdin) to what a solver promises, on stdout.

A continuous optimum is only defined up to the solver's tolerance, and its trailing digits move with the solver
version and the CPU. Every fractional number is rounded to DECIMALS places, and because box-mode column widths follow
the digits printed, runs of padding are collapsed too. The model dump is not passed through here: it stays byte-exact.
"""

import re
import sys

DECIMALS = 6

NUMBER = re.compile(r"(?<![\w.#])-?\d+(?:\.\d+(?:[eE][-+]?\d+)?|[eE][-+]?\d+)(?![\w.])")
PADDING = re.compile(r" +|─+")


def _round(match: re.Match) -> str:
    # Adding 0.0 turns a rounded -0.0 into 0.0.
    return f"{round(float(match.group()), DECIMALS) + 0.0:.{DECIMALS}f}"


for line in sys.stdin:
    sys.stdout.write(PADDING.sub(lambda m: m.group()[0], NUMBER.sub(_round, line)))
