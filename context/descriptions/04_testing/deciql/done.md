# DeciQL Surface Test Coverage — Done

The combination suites written in the 2026-09-29/30 syntax review. Each file
pins one dimension of the `WHEN` / `PER` / `IF` / `BY` surface described in
`../../00_project_overview/syntax_reference.md`; every correctness test carries
an independent gurobipy oracle, every refusal test asserts a short topic phrase
(never the message text), and every docstring names the rule and the wrong
answer a plausible bug would give.

| File | Dimension |
|---|---|
| `test_deciql_deck_conformance.py` | The deck's examples: ANR 1–7 (8 and 9 as named refusals), frame examples 1, 2, 4–6 and the p50 NULL-skip rule, the conditional example of p58, the lexicographic nurse example of p63, the p73–75 production plan end to end |
| `test_deciql_prefix_grid.py` | The eight `{WHEN, PER, IF}` subsets × the `PER` spellings (omitted, `ROW`, `()`, columns, `(a, b)`, a relation, relation + column) × body kinds; instance counts; `WHEN` before `PER`; a prefix over `BETWEEN`; prefix-order and postfix refusals; `USING` keys; both clause orders |
| `test_deciql_generation_keys.py` | The functional-dependency rule, admissions and refusals; the refinement rule for `<=`, `>=`, `=`, `<>`; NULL as a key value; `PER ROW` / `PER (k)` |
| `test_deciql_declarations.py` | Scope spellings, the `D.x` alias, shared keys, once-per-key charging, declaration bounds, `BOOL`/`SEMI`/`TEXT` domains, hidden switches, EXPLAIN scopes |
| `test_deciql_reducers_by.py` | Reducer prefixes, `BY` spellings, several `BY` keys in one body, factors on reducers (constant, `PER ()` decision, `BY`-determined data), the empty-reducer rules, `COUNT(*) BY`, `norm` with `WHEN` and `BY` |
| `test_deciql_frames_matrix.py` | Selectors × policies × `CYCLIC` × `DESC` × `WITHIN` × surrounding prefix × body; absolute endpoints; ties; NULL keys; scaled frames; every named refusal |
| `test_deciql_objectives.py` | `THEN` chains (integer-exact freezing, tolerance for `REAL`), arithmetic over `PER ()` decisions, nested reducers with a factor, `SATISFY`, EXPLAIN, refusals |
| `test_deciql_lexing_errors.py` | The DECIDE words as ordinary identifiers (columns, aliases, keys, order keys, decision names, `t.per` / `"within"`, `WITHIN row` versus `PER ROW`), `CASE WHEN` outside the body, a bound subquery's own `OVER`, nested DECIDE in both clause orders and through a CTE, lexer state across a script; then the catalogue of 55 named parser and binder errors (retired declarator spellings, domains, prefix order, colons, postfix `PER`/`WHEN`, `SUM(K: e)`, comma/`OR` between constraints, decisions in keys, NULL in `IN`, frame slips, objective spellings, `SATISFY` mixes, `DECIDE` twice) |
| `test_deciql_minmax_easy.py` | Easy-direction `MIN`/`MAX` under `PER`, `BY`, `WHEN` and `IF`: the tightest bound per `BY` group (keyed, finer key, row instances, scaled, NULL key), the reducer's `WHEN` against the clause's, `MAX(e) = K` with a clause `WHEN` and a group-varying bound, the hard direction for contrast, the one admitted `IF` shape and the refused ones |
| `test_deciql_declarations_edges.py` | Declaration edges: key spellings, the `D.x` alias's limits, reads through `PRIMARY KEY` / `UNIQUE`, bounds that scale into the objective, `SEMI` and `TEXT` edges, serializer round-trip, refusals naming the fix |
| `test_deciql_combinations.py` | The pairs the other files leave open, found from a coverage matrix over declaration scope × prefix × body × objective: keyed and query-wide decisions under `WHEN`, `IF`, `WHEN`+`PER`+`IF`; prefixes over `=`, `<>`, `IN`, `AVG`, easy and hard `MAX`, a reducer beside a per-row term, frames; guarded group and row instances; `THEN` and `SATISFY` over those bodies; each table case also runs a mutant that must change the optimum |
| `test_deciql_restrictions.py` | The restrictions of the language spec's "not yet implemented" list, refused by name |

The pre-existing suites keep their coverage of the older spellings' semantics
(`per/`, `when/`, `entity_scope/`, `qualified_reducer/`, `scalar_scope/`), read
through the mapping in their headers.

## Rules pinned here and nowhere else

- NULL is a key value for generation (`PER k` imposes its instance on the NULL
  group; `WHEN k IS NOT NULL` excludes it).
- A reducer over no rows has no value: instance skipped, empty term contributes
  nothing, a clause with no instance imposes nothing, a bound whose data reducer
  reads no row is not imposed; this holds for `SUM` and `AVG` alike. `MIN`/`MAX`
  terms over no rows and objectives over no rows are refused.
- An easy `MIN`/`MAX` bounds each row by the tightest instance of its own `BY`
  group; the reducer's `WHEN` picks the bounded rows, the clause's `WHEN` the
  instances; `MAX(e) = K` keeps the clause's `WHEN` on both halves; an `IF` guard
  on it needs the instance to be the reducer's group.
- A `WHEN`-excluded row is never read, so a NULL in it is not an error.
- Two reducers of one decision with different `BY` keys, two frames of one
  decision, and a frame beside a reducer of the same decision are distinct terms.
- A `BY`-determined factor scales a reducer (deck example 5).
- A decision is determined through a table key; `FIRST`/`LAST` under a keyed
  `PER` need only the partition.
- An integer-valued `THEN` stage is held exactly at any magnitude.
