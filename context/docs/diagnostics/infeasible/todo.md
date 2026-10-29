# Query Diagnostics — Infeasible — open work

Current behavior and the accepted repair-tie policy are recorded in [`done.md`](done.md).

## INF-1 — An `=` on a MIN/MAX is echoed as the half its finding loosens

**Why.** Stage 05 splits `max(e) = K` into an easy `max(e) <= K` (one bound per member)
and a hard `max(e) >= K` (`RewriteMinMaxInConstraint`, `decide_rewrite_minmax.cpp`;
`min` the other way round), and both halves keep the clause's source id. A finding is
read off the row its slack sits on and quotes that row's sense, so it names the half
alone: `x <= c AND max(2 * x) = 20` reports `max(2 * x) >= 20` → `max(2 * x) >= 6`. The
user wrote neither half, and pasting the suggestion back replaces the equality with a
one-sided bound, which drops the `<=` the query had.

**Ruling (owner, 2026-10-27).** Quote the clause as written (`max(2 * x) = 20`) and suggest a moved
equality (`max(2 * x) = 6`), which holds the other half too; when the check below fails,
fall back to naming the half and saying which half moved. A moved equality is a valid repair only when the other half still
holds at the new value, so the first choice needs a check that it does.

**Ruling (owner's delegate, 2026-10-29).** (c): suggest the range `max(2 * x) BETWEEN 6 AND 20` through INF-3's
mechanism. It always repairs, so it needs no check and no second solve.

**Test.** In `test_query_diagnostics_relation.py`, both backends: an infeasible
`max(...) = K` and `min(...) = K`, with and without `per g:` and `by (g)`, report the
clause as written; `_apply_reported_fix` pastes the suggestion back and the query
solves.

**done.md.** "What the reader is quoted: `DisplayRhs`" and "A hard MIN/MAX clause is
loosenable on both arms".

## INF-2 — A relation key's NULL reads like the string `'NULL'`

**Why.** The `group` column labels a class by its `per` key value through
`FormatPerGroupKey` (`physical_decide.cpp`): a NULL key value renders `NULL`, and a
VARCHAR value that is the string `'NULL'` renders `NULL` as well. A written key column
that is NULL is no class (D5), so no finding names it; a relation key (`per D`) compares
NULL as a value, so a class of `per D` whose column is NULL and one whose column is the
string `'NULL'` read the same.

**Ruling (owner, 2026-10-27).** No label change for written keys. A relation key's label
is open.

**Ruling (owner's delegate, 2026-10-29).** A relation key's label writes each value as an SQL literal with the
formatter the determination message uses (`'NULL'` quoted, NULL bare), so labels and messages share one value form.

**Test.** In `test_query_diagnostics_relation.py`, beside
`test_infeasible_per_group_expanded_mode_preserves_empty_string_group`: a `per D` key whose
column holds both NULL and `'NULL'` reports two distinguishable `group` values, on both
backends.

**done.md.** "Group identity (group label + scope)"; `../../language/guide.md` §8.1's
`group` row.

## INF-3 — A `BETWEEN` is quoted as written

**Why.** `../../issues/bugs.md` § BUG-9. `ExpandBetween` (`decide_constraints_binder.cpp`) splits
`x BETWEEN 18 AND 25` into two comparisons before the source registry sees it, so a finding names one half and the
paste-back loses the other.

**Design (architect review done).** Keep one source id per half (one id for both is unsound:
`FinalizeConstraintSourceInfo` writes canonical text, `rhs_kind` and `source_*` once per id, so one half overwrites the
other) and link both to the written clause (a many-to-one source map, the Composite pattern): a binder tag pairs the
halves, `written_*` holds the whole clause, one pass after `ReadElasticEdits` spells `x BETWEEN 10 AND 25` from the
moved end and the partner's text, merging two moved ends into one edit (the paste-back cannot find the clause after the
first). EXPLAIN groups the halves.

**Ruling (owner's delegate, 2026-10-29).** The six calls as recommended: link fields on each half; a moved term is
quoted as comparisons are (canonical); both ends moved make one row with the summed amount; INF-1 uses this mechanism;
EXPLAIN prints two rows under the clause; DROP of one half drops the clause.

**Test.** The test at `test_query_diagnostics_relation.py` mapping `x <= 5` to `x BETWEEN 0 AND 5` hides this bug and
goes with the fix.
