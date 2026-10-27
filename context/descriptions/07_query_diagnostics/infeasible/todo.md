# Query Diagnostics — Infeasible — open work

Current behavior and the accepted repair-tie policy are recorded in [`done.md`](done.md).

## 1. An `=` on a MIN/MAX is echoed as the half its finding loosens

**Why.** Stage 05 splits `max(e) = K` into an easy `max(e) <= K` (one bound per member)
and a hard `max(e) >= K` (`RewriteMinMaxInConstraint`, `decide_rewrite_minmax.cpp`;
`min` the other way round), and both halves keep the clause's source id. A finding is
read off the row its slack sits on and quotes that row's sense, so it names the half
alone: `x <= c AND max(2 * x) = 20` reports `max(2 * x) >= 20` → `max(2 * x) >= 6`. The
user wrote neither half, and pasting the suggestion back replaces the equality with a
one-sided bound, which drops the `<=` the query had.

**Decision needed.** Quote the clause as written (`max(2 * x) = 20`) and suggest a moved
equality (`max(2 * x) = 6`), which holds the other half too; or keep the half and say
which half moved. A moved equality is a valid repair only when the other half still
holds at the new value, so the first choice needs a check that it does.

**Test.** In `test_query_diagnostics_relation.py`, both backends: an infeasible
`max(...) = K` and `min(...) = K`, with and without `per g:` and `by (g)`, report the
clause as written; `_apply_reported_fix` pastes the suggestion back and the query
solves.

**done.md.** "What the reader is quoted: `DisplayRhs`" and "A hard MIN/MAX clause is
loosenable on both arms".

## 2. A NULL class reads like the string `'NULL'`

**Why.** The `group` column labels a class by its `per` key value through
`FormatPerGroupKey` (`physical_decide.cpp`): a NULL key value renders `NULL`, and a
VARCHAR value that is the string `'NULL'` renders `NULL` as well, so the two classes
read the same. Under the expanded scope, `per g: sum(x) by (g) >= 2` over
`g IN (NULL, 'NULL', 'a', '')` reports four `expanded_group` findings labelled `a`,
`NULL`, `NULL` and `''`. The empty string already has its own label (`''`), so it does
not read as "no group". NULL is a class of its own (D5), so every `per` key over a
nullable column meets this.

**Decision needed.** Keep `NULL` (documented in `syntax_reference.md` §8.1), give a NULL
a label no value can have, or print every label as a SQL literal (`NULL`, `'NULL'`,
`'a'`, `''`), which changes every `group` pin and the composite form (`EU, 2024`). The
unbounded engine's `group` label for a NULL slice should follow the same choice; its remedy
already prints `IS NULL` (`../unbounded/done.md`, "Scoping the prescribed cap").

**Test.** In `test_query_diagnostics_relation.py`, beside
`test_infeasible_per_group_expanded_mode_preserves_empty_string_group`: a key holding
both NULL and `'NULL'` reports two distinguishable `group` values, on both backends.

**done.md.** "Group identity (group label + scope)"; `syntax_reference.md` §8.1's
`group` row.
