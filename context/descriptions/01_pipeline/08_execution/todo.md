# Stage 08 — Execution: open work

## 1. One key-grouping path (optional)

**Why.** Execution turns a key into row → class ids in two places, both over
`BuildGroupIds` and both with NULL a key value:

- `BuildEntityMappings` (§4) for the key scopes (`EntityScopeInfo`: a keyed decision's
  key, a reducer's own `per` or colon form, a helper decision's path key). Ids are
  first-seen over every row; a helper's scope reads only the rows its path keeps and puts
  every dropped row in one spare class. Nothing is cached.
- `LookupOrBuildPerGroupIds` (§5) for the `by` keys, a constraint's `per K:` classes,
  the scope checks' keys and an objective AVG's `by` key. The ids over every row are
  cached in `PerGroupCache`; each call then renumbers the groups its filter keeps densely,
  in the order their rows first appear, and a clause whose `when` drops rows builds
  afresh over its kept rows.

A key used both ways — a keyed decision `per g: x(INT)` and a reducer's `by (g)` or a
constraint's `per g:` — is evaluated twice. One cache serving both would evaluate each
key once and give equal keys equal ids.

**Decision needed.** Whether to keep both numberings or re-capture the golden dumps. The
column order of keyed decisions and helpers follows the entity ids, and the row order of
grouped rows follows the dense ids; golden corpus entries 22, 23, 38, 39, 56, 61, 62, 65,
70 and 91–94 depend on them. A merge that keeps entity ids first-seen over every row and
the per-call dense renumbering changes no dump. If `entity_scopes` and `group_keys` also
merge into one key table, that part is stage 03's.

**Test.** The golden corpus dumps identically on Gurobi and HiGHS; a query with a
decision keyed by `g` and a `by (g)` reducer evaluates `g` once (a profile counter on
`extraction.group_ids`); the scope suites pass unchanged.

**done.md.** §4 "PHASE 1.5 — entity mappings" and §5 "`when`, `by` and `per` →
`row_group_ids`".
