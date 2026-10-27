# Scopes — open work

## Name the keyed scope "keyed" everywhere

The language calls a `per K:` decision *keyed*, but the code and one user-visible label
still say *entity*: `DecideVarScope::ENTITY` (`common/enums/decide.hpp`),
`EntityScopeInfo`, `BuildEntityMappings`, `DecideVarScopeInfo::Entity()`/`Keyed()`, and
DIAGNOSE's `scope` column, which reports `entity` for a keyed decision's escape count.
`SCALAR` is the query-wide scope (`per ()`), and `PER_CONSTRAINT_TAG` /
`DecideObjective::per_columns` now serve only the two-level objective.

- **Decided** (D46): `DecideVarScope` becomes `ROW / KEYED / GLOBAL` with the values
  0/1/2 kept, so serialized plans still read; `Entity()` and `Keyed()` merge, `Scalar()`
  becomes `Global()`; the DIAGNOSE label becomes `keyed`. The two-level objective's tag
  and key field take names that say so.
- **Tests**: `test_query_diagnostics_escaping_instances.py` pins the `entity` label
  (update it to `keyed`); `DECIDB_VERIFY_SERIALIZER=1` over `test/decide/tests` proves
  the enum values still round-trip.
- **Docs to update**: `done.md` §2 and §9, `../diagnose/done.md`,
  `../../00_project_overview/syntax_reference.md` §8.1, and
  `../../07_query_diagnostics/`.

## A NULL key is not a class: its rows skip the keyed clause (reverses D5)

- **Owner's ruling (2026-10-27):** "NULL should not be a key. That group should skip." Today D5 makes a NULL key one
  more class (`done.md` "NULL is a class"; syntax reference §7.2): with `sum(x) by (g) <= 2`, the rows whose `g` is NULL
  get a limit of 2 of their own. Under the ruling those rows leave that clause, as `when g is not null` would, and keep
  every other clause and the objective. This matches the old `PER`, and the keyed direct plans that already skip NULL-key
  rows (framework todo H-05).
- **Decision needed before coding** (the ruling names `by`; ask about the rest):
  1. `by (g)` on a reducer: rows with a NULL `g` are out of that reducer. Assumed yes.
  2. `per g:` in front of a constraint: do NULL-`g` rows get no constraint at all?
  3. `per` inside a reducer (`sum(per cap: cap)`): are NULL-`cap` rows left out of the sum?
  4. A keyed decision (`per K:` declaration): what does a NULL-key row get? Its own decision, none (fixed 0 or NULL),
     or an error?
  5. A group-by in SQL keeps NULL as one group; say in the docs that DECIDE departs from it and why.
- **Where it changes:** the window partitions and class numbering in `RewriteRhsAggregates`
  (`decide_rewrite_rhs_window.cpp`) and the solver's grouping; the scope checks and messages that quote `g = NULL`
  (`PhysicalDecide::RunScopeChecks`); the removed-spelling help messages (syntax reference §12, "a NULL g is now its own
  group, so write when g is not null ...") become unnecessary; syntax reference §3 and §7.2; `done.md` D5.
- **Tests:** every D5 test turns over (for example `empty_all_keys_null` and the NULL-key cases of
  `test_direct_s1_uniform_matroid.py`, `test_direct_r1_continuous_knapsack.py`, `test_scope_*.py`), and the golden
  baselines with NULL keys. The keyed-plan C++ test ("S1 and R1 keyed plans answer as the solver does",
  `test/common/test_decidb_direct_decline.cpp`) can take NULL keys again (`KEYED_ROWS`). Flip each D5 test to the new
  rule in the same change as the code; none is changed before.
