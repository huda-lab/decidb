"""Databases for the query-rewriting pages (docs/rewriting.html, docs/rewrite-rules.html).

Each is the shortlist example's table with the rewriting setting pinned, so a page can show the same problem on the
solver path, or show the error `require` raises when no rule applies.
"""

import examples as _EX

_SHORTLIST = next(ex["setup"] for ex in _EX.EXAMPLES if ex["slug"] == "shortlist")

FIXTURES = {
    "rewrite-shortlist-solver": {"setup": _SHORTLIST + "\nSET decide_direct_solve = 'off';"},
    "rewrite-shortlist-require": {"setup": _SHORTLIST + "\nSET decide_direct_solve = 'require';"},
}
