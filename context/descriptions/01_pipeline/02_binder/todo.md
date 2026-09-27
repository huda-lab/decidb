# Stage 02 — Binder: open work

- Derived keys (`BY (D, S.day + S.transit)`, spec §9 #8): `FindOrCreateKeyScope` accepts
  columns and relations only, since an `EntityScopeInfo` is a list of column bindings.
- Range frames reducing with `MIN`/`MAX`/`AVG` (`BindFrame` accepts `SUM` only).
- Variant Signature (spec §7.2) as an alternative to the reject policy of §6.3.
- A SEMI domain whose data floor is negative on some rows needs an explicit negative
  constant bound; only a constant floor widens the column box.
