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
