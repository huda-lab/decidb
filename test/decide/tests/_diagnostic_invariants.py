"""Invariants every DECIDE diagnosis must satisfy, whatever construct produced it.

A diagnostics defect does not fail a test the way a wrong answer does. The solve
still fails, the diagnosis still appears, the numbers in it are often still
right — what breaks is *which line of the user's query we blame*. So the suite
stays green while the feature's whole promise ("here is the smallest edit to
YOUR query") quietly stops holding.

These checks exist so that promise is asserted mechanically, on every diagnostics
test, rather than re-stated by hand per construct. There are nine auxiliary kinds
that can reach the elastic engine (`<>` per-row and aggregate, ABS, MIN/MAX hard
and composed, `norm` 0/1/inf, bilinear); a per-test assertion only ever guards the
one its author was thinking about. B1 is the worked example: a composed MIN/MAX
clause reported

    MIN((x + c)) - x - 110*MIN((x + c)) >= -110

where 110 is an internal Big-M and the whole string appears nowhere in the query.
Every number in it but 110 was real, and the repair it prescribed was arithmetically
correct — which is exactly why nothing failed.

Three rules:

1. **A reported edit is made of the user's own text.** Enforced by token
   grounding (`assert_edits_are_users_text`): every identifier and every number
   in the blamed clause must occur in the query the user typed.
2. **Ties break on achievable objective.** When two edits cost the same, the one
   leaving the user better off is reported. Its observable form is cross-backend
   agreement (`assert_backends_agree`) — a tie broken by anything other than a
   stated rule is broken differently by Gurobi and HiGHS, because the two get
   different row sets for the same query.
3. **A construct is reported in the user's spelling.** `ABS(x) >= 5` reports as
   `ABS(x) >= 5`, never as the rows it lowers to. Enforced by
   `assert_no_internal_names` plus rule 1's token grounding: a lowered form
   cannot be spelled without introducing names or magnitudes the query never had.

Deliberately NOT consulted here: the ``subject_to_sql`` map that
``_apply_reported_fix`` takes. That map exists so a test can *apply* a fix whose
rendering differs from the typed SQL (a BETWEEN split in two, a reversed bound,
a composed extremum). If these checks honored it, a future author could silence a
fabricated clause by adding a map entry — the guard would be defeatable by the
same edit that makes a test pass. Token grounding needs no such hatch: a
re-spelling reuses the query's own names and numbers, so it passes on its own.
"""

from __future__ import annotations

import re

#: Identifiers and numeric literals, the two token kinds that must be grounded.
#: Everything else a rendered clause can contain — operators, parentheses, commas —
#: is punctuation the renderer is free to place.
_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_NUMBER_RE = re.compile(r"\d+(?:\.\d+)?(?:[eE][+-]?\d+)?")

#: Words a renderer may emit that are its own vocabulary, not the user's data.
#: Kept small on purpose: every entry is a place the check cannot see a leak, so
#: a name earns its way in only by being SQL the renderer must be able to write.
_RENDERER_VOCABULARY = frozenset({
    "and", "or", "not", "between", "in", "is", "null", "true", "false",
})

#: An internal column stands in for a term the user never named (`col3`), and every
#: pipeline tag opens and closes with `__` (see `AddDecideTag`). Either one in a
#: blamed clause means the user is reading our bookkeeping, not their query.
_INTERNAL_NAME_RE = re.compile(r"\bcol\d+\b|__[A-Za-z0-9_]+__")


#: Splits a rendered clause at its comparison. Numbers to the LEFT multiply a term --
#: they are structure, and no lowering may invent one. The number to the RIGHT is the
#: bound, which canonicalization legitimately *computes* from the user's constants
#: (`x + 2 <= 7` folds to `x <= 5`, and 5 was never typed). So the two sides are held
#: to different standards.
_COMPARISON_RE = re.compile(r"<=|>=|<>|!=|=|<|>")


def _split_at_comparison(clause: str) -> tuple[str, str]:
    match = _COMPARISON_RE.search(clause)
    return (clause[: match.start()], clause[match.end() :]) if match else (clause, "")


def _numbers(text: str) -> list[float]:
    return [float(m.group(0)) for m in _NUMBER_RE.finditer(text)]


def _identifiers(text: str) -> list[str]:
    # A number's exponent (`1e5`) would otherwise yield a bare `e`; strip numbers
    # before looking for names.
    return [m.group(0) for m in _IDENT_RE.finditer(_NUMBER_RE.sub(" ", text))]


def _query_vocabulary(sql: str) -> tuple[set[str], list[float]]:
    """The names and magnitudes the user actually wrote."""
    return {i.lower() for i in _identifiers(sql)}, _numbers(sql)


def _magnitude_is_grounded(value: float, query_numbers: list[float]) -> bool:
    """A canonical clause may carry a query number with its sign flipped — moving a
    term across the comparison negates it (`demand - SUM(s) <= cap` canonicalizes to
    `-SUM(s) - cap <= -demand`). So a magnitude, not a signed value, is what has to
    be grounded; the renderer owns the sign.
    """
    return any(abs(abs(value) - abs(q)) <= 1e-9 * max(1.0, abs(q)) for q in query_numbers)


def assert_no_internal_names(text: str, context: str) -> None:
    """Fail if user-facing diagnosis text names something the user cannot see."""
    found = _INTERNAL_NAME_RE.search(text)
    assert found is None, (
        f"Diagnosis names {found.group(0)!r}, which is internal bookkeeping and "
        f"appears nowhere in the user's query — {context}.\n"
        f"offending text: {text}"
    )


def assert_edits_are_users_text(sql: str, edits: list[dict], context: str) -> None:
    """Rule 1: every clause a diagnosis blames is spelled from the user's own query.

    ``edits`` is the output of the caller's ``_clause_edits(rows)``. For each edit
    the blamed ``subject`` is grounded whole — names and magnitudes both. Its
    ``suggested_change`` is grounded on names only, because introducing a new
    magnitude is precisely what a repair does: telling the user to change
    ``SUM(x) <= 22`` into ``SUM(x) <= 28`` has to be able to say 28.
    """
    query_names, query_numbers = _query_vocabulary(sql)

    for edit in edits:
        subject = edit.get("subject", "")
        assert_no_internal_names(subject, f"{context} (blamed clause)")

        for name in _identifiers(subject):
            if name.lower() in _RENDERER_VOCABULARY:
                continue
            assert name.lower() in query_names, (
                f"Diagnosis blames a clause naming {name!r}, which the user never "
                f"wrote — {context}.\n"
                f"  blamed:  {subject}\n"
                f"  query:   {sql}"
            )

        for value in _numbers(_split_at_comparison(subject)[0]):
            assert _magnitude_is_grounded(value, query_numbers), (
                f"Diagnosis blames a clause whose term {value:g}* was never in the "
                f"user's query — {context}. A coefficient the user never typed comes "
                f"from a lowering (a Big-M, a scaling factor), so this row states some "
                f"construct's mechanism, not their bound, and is not theirs to edit.\n"
                f"  blamed:  {subject}\n"
                f"  query:   {sql}"
            )

        suggested = edit.get("suggested_change")
        if not suggested:
            continue
        assert_no_internal_names(suggested, f"{context} (suggested change)")
        for name in _identifiers(suggested):
            if name.lower() in _RENDERER_VOCABULARY:
                continue
            assert name.lower() in query_names, (
                f"Diagnosis suggests an edit naming {name!r}, which the user never "
                f"wrote — {context}.\n"
                f"  suggested: {suggested}\n"
                f"  query:     {sql}"
            )


def assert_backends_agree(results_by_backend: dict[str, dict], context: str) -> None:
    """Rule 2, in its observable form: the same query gets the same advice everywhere.

    ``results_by_backend`` maps a backend name to
    ``{"edits": <_clause_edits output>, "achievable_objective": <str|None>}``.

    Gurobi and HiGHS do not state every construct the same way — ABS is native on
    one and lowered on the other — so they hand the elastic engine different row
    sets, and any repair choice not fixed by a stated rule falls out differently.
    A user moving hosts is then told to edit a different line of their own query.
    Comparing the two is how an unstated tie-break gets caught.
    """
    backends = sorted(results_by_backend)
    assert len(backends) >= 2, "cross-backend agreement needs at least two backends"

    reference = backends[0]
    ref = results_by_backend[reference]
    ref_subjects = sorted(e.get("subject", "") for e in ref["edits"])

    for other in backends[1:]:
        cur = results_by_backend[other]
        cur_subjects = sorted(e.get("subject", "") for e in cur["edits"])
        assert cur_subjects == ref_subjects, (
            f"{reference} and {other} blame different clauses of the same query — "
            f"{context}. Both edits may be valid, but the user is told to change a "
            f"different line depending on which solver is installed.\n"
            f"  {reference}: {ref_subjects}\n"
            f"  {other}: {cur_subjects}"
        )
        ref_obj, cur_obj = ref["achievable_objective"], cur["achievable_objective"]
        if ref_obj is None and cur_obj is None:
            continue
        assert ref_obj is not None and cur_obj is not None and (
            abs(float(ref_obj) - float(cur_obj)) <= 1e-6 * max(1.0, abs(float(ref_obj)))
        ), (
            f"{reference} and {other} promise a different payoff for repairing the "
            f"same query — {context}. The tie must break on achievable objective, so "
            f"both must land on the better one.\n"
            f"  {reference}: {ref_obj}\n"
            f"  {other}: {cur_obj}"
        )
