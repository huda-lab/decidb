"""Shared assertions about what DecidB's user-facing output must never contain.

A rejection, a diagnostic and an EXPLAIN are all read by a SQL user, so none of
them may expose the machinery underneath: a C++ trace, an assertion, or one of
the internal tags the pipeline stamps into an expression's alias.

The internal-tag check is deliberately a *pattern* rather than a list of known
symbol names. A named-symbol list only pins the leaks someone already found, and
goes stale the moment the symbol is renamed or deleted — the previous version of
this check still forbade ``ToSymbolicRecursive`` long after that function was
removed with the SymbolicC++ layer, so it protected nothing. Every DECIDE tag
opens and closes with ``__`` by construction (see ``AddDecideTag`` in
``duckdb/common/enums/decide.hpp``), so matching that shape catches tags that do
not exist yet.
"""

from __future__ import annotations

import re

#: Tags are ``__name__`` with no internal ``__``; payload tags carry digits
#: (``__source_clause_0__``), so the character class has to allow them.
INTERNAL_TAG_RE = re.compile(r"__[A-Za-z0-9_]+__")

#: Markers that mean the user was shown an internal failure instead of an error.
INTERNAL_ERROR_MARKERS = (
    "INTERNAL Error",
    "Stack Trace",
    "assertion failure",
)


def assert_no_internal_leak(result, context: str) -> None:
    """Fail if the CLI's combined output leaks internals to the user.

    ``result`` is a completed ``decidb_cli.execute_raw`` result; ``context``
    names the behaviour under test, so a failure says which path regressed.
    """
    combined = result.stderr + result.stdout

    for marker in INTERNAL_ERROR_MARKERS:
        assert marker not in combined, (
            f"Found {marker!r} in output — {context} regressed to the "
            f"internal-error path.\n"
            f"stdout: {result.stdout[:500]}\n"
            f"stderr: {result.stderr[:500]}"
        )

    leaked = INTERNAL_TAG_RE.search(combined)
    assert leaked is None, (
        f"Found internal tag {leaked.group(0)!r} in user-facing output — "
        f"{context} is showing pipeline metadata instead of the user's SQL.\n"
        f"stdout: {result.stdout[:500]}\n"
        f"stderr: {result.stderr[:500]}"
    )
