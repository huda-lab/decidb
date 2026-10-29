"""The oracle for DeciDB's large-number warning on a query whose model is written in literals.

DeciDB warns about a coefficient of 1e6 or more (1 / the integrality tolerance) and about a right-hand side or a bound of
about 4.5e9 or more (the feasibility tolerance / the machine epsilon). `writes_large_number` reads the SQL text for a
literal past the threshold of the position it stands in, independently of the model, so a test declares the warning
(`expect_large_numbers`) from what the query says.

A position is read from the syntax alone: a literal beside `*` is a coefficient, and any other literal is a right-hand side
or a bound. A literal in a `VALUES` list is data whose role the SQL text does not give; the caller says which (`data`).
The read does not follow a number through a rewrite: a query that folds a literal away (`10000000 * x <= 50000000` is a
bound on `x`), or builds a large number from small ones, is for the caller to judge. The harness checks the declaration
both ways, so a wrong read fails the test that makes it.
"""

import re

#: DeciDB warns about a coefficient of this magnitude or more (DECIDE_LARGE_COEFFICIENT) ...
LARGE_COEFFICIENT = 1e6

#: ... and about a right-hand side or bound of this magnitude or more (DECIDE_LARGE_TOTAL).
LARGE_TOTAL = 1e-6 / 2.220446049250313e-16

#: A bound written at this magnitude or more reads as no bound (DECIDE_EFFECTIVE_INFINITY), and no bound is no number.
OPEN = 1e20

_NUMBER = re.compile(r"(?<![\w.])\d+(?:\.\d+)?(?:[eE][+-]?\d+)?")


def _values_spans(sql):
    """The (start, end) of each `VALUES (...), (...)` list in the SQL."""
    spans = []
    for match in re.finditer(r"VALUES", sql, re.IGNORECASE):
        position = match.end()
        start = position
        while True:
            while position < len(sql) and sql[position].isspace():
                position += 1
            if position >= len(sql) or sql[position] != "(":
                break
            depth = 0
            while position < len(sql):
                depth += {"(": 1, ")": -1}.get(sql[position], 0)
                position += 1
                if depth == 0:
                    break
            end = position
            while position < len(sql) and sql[position].isspace():
                position += 1
            if position < len(sql) and sql[position] == ",":
                position += 1
        spans.append((start, end))
    return spans


def writes_large_number(sql, bounded=(), data="total"):
    """Does the SQL write a literal at or past the threshold of its position?

    `bounded` names decisions whose bound `d <= v` or `d >= v` is read as written: a literal of 1e20 or more there is no
    bound, so it is left out. `data` is the role of a literal inside a `VALUES` list: "coefficient" or "total".
    """
    for decision in bounded:
        sql = re.sub(
            rf"(?<![\w.]){re.escape(decision)}\s*[<>]=?\s*-?(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)",
            lambda match: "" if float(match.group(1)) >= OPEN else match.group(0),
            sql,
        )
    values = _values_spans(sql)
    for match in _NUMBER.finditer(sql):
        before = sql[: match.start()].rstrip()
        after = sql[match.end() :].lstrip()
        if any(start <= match.start() < end for start, end in values):
            role = data
        elif before.endswith("*") or after.startswith("*"):
            role = "coefficient"
        else:
            role = "total"
        threshold = LARGE_COEFFICIENT if role == "coefficient" else LARGE_TOTAL
        if float(match.group()) >= threshold:
            return True
    return False
