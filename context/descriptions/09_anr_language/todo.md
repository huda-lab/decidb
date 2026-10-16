# ANR language surface — plan

> **Status (2026-10-15): items 1 (the new spellings parse), 2 (`decide per K:
> x(TYPE)`), 3 (reducer scope) and 4 (constraint and objective scope) have shipped — see
> [`done.md`](done.md); items 5–7 remain.**
> Target branch `Sami-Active`, designed from this branch's code alone; earlier attempts
> on other branches are not a reference. As an item ships, its content moves into
> `done.md` and the stage docs it changes; this file keeps only what remains.

DECIDE gains the scoping language of the "DeciQL proposals" deck:

- `per` **in front** — how many decisions or constraints are created;
- `when` **in front** — which rows are looked at;
- `by` **after an aggregate** — which rows that one aggregate reads.

The goal is the **language**, plus the smallest formulation work that makes deck
examples 1–8 run. The deck's general algorithm for aggregates nested inside aggregates
is not part of this plan; §4.4 lists what already leaves room for it.

**Authority, highest first:** the checklist (§6) and the decisions (§3) — written out
as exact behaviour in the spec (§0) — then `~/Desktop/Capstone/ANR Implementation.pdf`
(semantics) and `ANR.pdf` (deck pp. 6–7, 14–32, 62). The PDFs are context only: a
feature is not in scope because a PDF shows it (§10).

---

## 0. Words used here

Reducer, tuple term, key, class, kept rows and "determines" are defined in the
behaviour spec,
[`../00_project_overview/anr_language_extension.md`](../00_project_overview/anr_language_extension.md)
§1 — "the spec" below. Three more words are used only in this plan:

| Word | Meaning |
|---|---|
| **generation key** | the key after a constraint's `per`: one constraint per class |
| **group key** | the key in a reducer's `by (...)` |
| **value variable** | an internal helper decision holding one reducer's result for one group (§4.2) |

---

## 1. Target language

Specified in the spec and not restated here: grammar and defaults (§1), declarations
(§2), constraints (§4), reducers (§5), objective (§6), removed spellings (§10), and deck
examples 1–8 (§11).

## 2. Meaning

Also in the spec: the four evaluation steps (§3), the determination rule (§7), NULL and
empty-set outcomes (§8), and every error with its message (§9). The spec says *what*
the language does; this plan says *how* it gets built and verified.

---

## 3. Decisions (2026-10-05)

| # | Decision | By |
|---|---|---|
| D1 | `SUM(D: e)` stays; it and `sum(per D: e)` are two spellings of one feature | user |
| D2 | The nested objective stays, written `outer(per k: inner(e) by (k))`, on today's implementation | user |
| D3 | The new clause shapes support `SUM`, `AVG`, `MIN` and `MAX` | user |
| D4 | A `when` that keeps no rows is an error, with or without a reducer in the clause | user |
| D5 | NULL is its own class for `decide per`, constraint `per` and `by` | user |
| D6 | `per g: sum(x) <= 10` is accepted; no `by` means `by ()` | user |
| D7 | Each declarator has its own key; a `per` does not carry over | user |
| D8 | A reducer with no rows for some class is an error, not a skipped constraint | user |
| D9 | (2026-10-09) `T.x` keeps dropping from its key the columns its clause reads as data until item 7; a `per` key is exactly as written | user |
| D10 | (2026-10-09) A decision declared with `per` inside the old `SUM(D: e)` is refused ("not supported yet") until item 3 (D13 keeps the refusal until item 7; `sum(per D: e)` takes one) | user |
| D11 | (2026-10-09) Messages name a `per` decision as declared; `T.x` messages stay word for word; DIAGNOSE labels its scope `entity` until item 6 | user |
| D12 | (2026-10-14) D4, D5, D8 and the determination check apply to the new spellings only; the old spellings keep today's behaviour until item 7. A clause with no new spelling is the same text in both languages and keeps today's rules | user |
| D13 | (2026-10-14) `SUM(D: e)` keeps today's bind-time rules and messages (S4 waits for item 7); `sum(per D: e)` follows ANR; both may sit in a scoped clause and take a `by` | user |
| D14 | (2026-10-14) A clause mixing an old postfix `WHEN`/`PER` (on the clause or after a reducer) with a new spelling is refused, naming the new spelling | user |
| D15 | (2026-10-14) A scope or `by` on a call that is not `SUM`/`AVG`/`MIN`/`MAX` is refused with a message (`by`: the spec's §9 text) | user |
| D16 | (2026-10-14) The 21 "not supported yet" cases of `test_scope_parser.test` that now bind change only their expected result; the four ordinary-subquery cases keep the gate message. `_scope_oracle.py` may gain code, its existing behaviour unchanged | user |
| D17 | (2026-10-14) New paths are correct; old spellings keep today's behaviour, wrong answers included, and every old bug found is logged in `06_issues/bugs/todo.md` | user |
| D18 | (2026-10-14) The D8 message names the `by` group the reducer reads (`has no rows for D.region = 'North'.`); with `by ()` it ends `has no rows.` | user |

Small calls made while planning. All were put to the user on 2026-10-05 and none was
vetoed.

| # | Call |
|---|---|
| S1 | `per row` is accepted as the written-out default |
| S2 | Decisions are referenced by bare name; `D.open` in a constraint is no longer accepted |
| S3 | With no `per`, a constraint is per row — so `sum(x) <= cap` keeps today's meaning (the tightest `cap`); with an explicit `per`, a varying bound is the C8 error |
| S4 | `SUM(D: e)` uses the same data check as `sum(per D: e)`; today's bind-time rule for it goes away (item 7: D13 keeps the rule until then) |
| S5 | Old and new spellings coexist until the last item |
| S6 | `per (a, b):` is refused with a message; parentheses mean only `per ()` |
| S7 | Inside a DECIDE clause `when`, `per`, and `by` after a `)` are keywords; a column with such a name is quoted there |
| S8 | `per` keys are columns and relations; expressions are allowed only in `by` (checklist wording) |
| S9 | An input with no rows at all keeps today's behaviour (empty result); D4 is about a `when` that removes every row of a non-empty input |
| S10 | Data-only reducers stay limited to the bound side of a comparison, as today |
| S11 | Today's rule that rejects a reducer over a lone whole-query decision (`SUM(cap)`) is left as is |

---

## 4. Design

### 4.1 One kind of key

Today rows are grouped by two separate mechanisms — entity scopes (variables and
`SUM(D: …)`) and `PER` columns (constraints) — and `by` has none. They become one:

- **A key scope** — `EntityScopeInfo` generalised from "all columns of some tables" to
  "a list of bound key expressions", created by one `FindOrCreateKeyScope` that sorts
  and de-duplicates, so equal keys share a scope. It serves a declaration's `per`
  (shipped, item 2), a reducer's inner `per`, and a reducer's `by`.
- **A constraint's generation key** keeps living in the bound `PER` wrapper, where
  every pass already expects it.
- **Execution** turns any key into row → class ids through one cache (merging
  `BuildEntityMappings` and `LookupOrBuildPerGroupIds`), NULL as a value. A wrapper key
  and a scope with the same expressions resolve to the same ids.

**How items 3–4 built it, and what is left.** A reducer's inner `per` is a key scope
(`FindOrCreateKeyScope`, `exact_key`), as planned. A `by` key is not: it may be an
expression, and a key scope holds column bindings, so it lives in its own table,
`LogicalDecide::group_keys`, and a constraint's `per` key in its `PER_CLASS_TAG`
wrapper. Execution keeps two grouping paths: `BuildEntityMappings` for key scopes,
`LookupOrBuildPerGroupIds` (one cache) for `by`, class and check keys, both with NULL a
value; an empty key is one group in `BuildGroupIds` for both. Merging them is open. It
must keep today's numbering, which golden dumps 22, 23, 38, 39, 56, 61, 62, 65, 70 and
91–94 depend on: entity ids are first-seen over all rows, NULL a value; old `PER` ids
are dense in first-kept-row order, NULL excluded.

### 4.2 Three shapes, one rewrite

Stage 4 classifies each canonical clause by what its **decision side** contains. Only
the third row is new machinery.

| Decision side contains | Shape | Built by |
|---|---|---|
| no reducer | **rows** | today's per-row path. Under an explicit `per`, one row per class (its first row) instead of one per row |
| only reducers sharing one `by` key, with whole-query factors (whole-query decisions may sit beside them) | **aggregate** | today's aggregate paths, grouped by that `by` key, tightest bound per group — exactly how `SUM(x) <= cap` works today |
| anything else: a per-row or keyed decision beside a reducer, reducers with different `by` keys, a factor that varies by row | **value rewrite**, then rows | below |

`=` and `<>` under an explicit `per` whose key differs from the `by` key also take the
value rewrite: "tightest bound" is only exact for `<=` and `>=`.

**Value rewrite** (new stage-5 pass `RewriteScopedReducers`, after the ABS and bilinear
rewrites and before the MIN/MAX passes). For each reducer `agg(E) by (Γ)` of the clause
that contains a decision (a data-only reducer is just a number and is left alone):

1. add a value variable `v` keyed by Γ (whole-query when Γ is empty) — `INT` when the
   reducer is whole-numbered by declared types, else `REAL`. Unlike a user decision it
   may be negative: its box is derived from its members' reach at stage 6;
2. replace the reducer by `v`; the clause keeps its `when`/`per`, now has no reducer,
   and re-enters through `LogicalDecide::AddConstraint` as a rows-shape clause;
3. record a `ReducerValueLink` (`v`, the aggregate, its inner terms, its inner
   `when`/`per`, the clause's own `when`). Stage 6 emits it per group: `SUM`/`AVG` →
   one equality row `v = Σ…`; `MIN`/`MAX` → the existing `EmitExtremumLink`, with `v`'s
   column as the result and the envelope/closing sides chosen from how the clause uses
   `v` (unknown sign or `=` → both).

A reducer whose body holds a product of two non-Boolean decisions or a square is not
rewritten — its definition would be a quadratic equality. Such a clause is refused with
"not supported yet" unless it is already in the aggregate shape (spec §12).

Deck example 1 then has the form `SUM(ship) - v = 0 AND ship <= 0.2 * v` with one
whole-query `v` — two things the engine documents today: a whole-query decision beside
a reducer (`syntax_reference.md` §5) and one compared per row (§2.2). It costs O(N)
matrix entries instead of N². The pattern is the one `RewriteAbs` set: helper variable
at stage 5, linking rows at stage 6. Stage 6 gets no new constraint path.

### 4.3 Checks are built once and run once

`LogicalDecide::scope_checks` — one entry per checked item: a `when` that must keep a
row, a reducer whose own `when` must leave every `by` group a row, and each item that
must be constant per class (a data expression, a decision, a `by` key), each with its
`when`, its key and the SQL text to quote. Stage 4 builds them from the clauses as bound,
**before** canonicalization, which drops an aggregate-shape clause's `per K:` wrapper:
the checks are all that remains of that key. Stage 8 runs them before formulation,
computing nothing on a row a `when` drops. They raise the three run-time errors of the
spec's §9: no kept rows, key does not determine, reducer empty for a class.

So whether a query is valid never depends on which shape a clause takes, and stages 5–6
may assume a well-defined clause — they assert, they do not repair.

### 4.4 Room left for nested aggregates

- The grammar is already recursive: a reducer's argument is a scoped expression that
  may contain reducers.
- Scope lives as tags on each bound aggregate, so depth is encoded nowhere else.
- One binder gate ("not supported yet") is the only refusal. Lifting it for a shape
  means letting the value rewrite recurse: an inner value per inner class, inside the
  outer group.
- Variant signatures (deck pp. 34–40) would turn the "does not determine" error into
  one constraint per distinct combination; `scope_checks` already lists the parts.

---

## 5. What changes in each layer

| # | Layer | Change | Main files |
|---|---|---|---|
| 1 | Parser | Item 7 (C10): `WHEN_DECIDE_OBJECTIVE` and its lexer state go, and the parenthesis hints in `decide_parse_hints.cpp` are deleted, with the old spellings. What item 1 shipped is in `done.md` | `grammar/statements/select.y`, `src_backend_parser_parser.cpp`, `decide_parse_hints.cpp` |
| 2 | Binder | Items 3–4 bound every marker (`done.md`). Item 7: rename the scope enum to ROW / KEYED / GLOBAL (today's ROW / ENTITY / SCALAR); remove `RewriteScopedVarRefs`, `CheckQualifiedReducerBody` (D13) and the "do not mix the two WHENs" rule with the old spellings | `decide_declarations_binder.cpp`, `decide_binder.cpp`, `decide_constraints_binder.cpp`, `decide_objective_binder.cpp` |
| 3 | Logical plan | Items 3–4 added `group_keys`, `scope_checks`, `scoped_objective`. Open: one `key_scopes` table if the key tables merge (§4.1) | `logical_decide.hpp/.cpp`, `logical_operator.json`, `nodes.json` |
| 4 | Canonicalizer | Items 3–4: `ClassifyScoped` (rows, aggregate, refused) and the scope checks. Item 5: the refused shapes take the value rewrite; rule C6 lets a factor on a reducer vary by row | `decide_canonicalizer.cpp/.hpp` |
| 5 | Optimizer | Item 5: `RewriteScopedReducers` (§4.2). Items 3–4 already fill an aggregate clause's grouping from its shared `by` key | `decide_optimizer.cpp`, new `decide_rewrite_scoped.cpp`, `decide_linear_form.cpp`, `decide_prepared_model.hpp` |
| 6 | Model | `EmitReducerValueLinks`: definition rows per group; the value variable's box comes from its members' reach | `linearization_minmax.cpp`, `ilp_linearization.cpp` |
| 7 | Solver | none | — |
| 8 | Execution | Items 3–4: checks first, kept-row reading, `by` groups, first row of a class, bound-side reducers over their own groups. Open: one key-grouping cache (§4.1). Item 5: evaluate value-link members | `physical_decide.cpp`, `plan_decide.cpp` |
| — | Rendering | Items 3–4: EXPLAIN echoes a scoped clause in its spelling. Item 6: the old spellings converted; a value variable labelled with its reducer's SQL; DIAGNOSE's `group` column for an aggregate-shape clause names its `by` group, where the spec's §1 asks for the `per` key value | `decide_source_provenance.cpp` |

---

## 6. Checklist: how each item is built and verified

C3–C6, C8 and C9 shipped with items 3–4; their tests are listed in `done.md`.

New tests live in `test/decide/tests/test_scope_*.py`. "Dump" means the built model
read through `DECIDB_DUMP_MODEL`; "oracle" is the reference evaluator of §8. Error tests
match the fixed phrases and messages of the spec's §9.

### C7 — several reducers in one constraint, each with its own `by`

**How.** The value rewrite of §4.2. Data-only reducers need none of it; they are numbers.

**Tests — `test_scope_value_rewrite.py`.**
1. Two decision reducers with different keys; three, one of them `by ()`.
2. A per-row decision beside a reducer (deck ex. 1); a row-varying factor (ex. 5); an expression key (ex. 8).
3. `MIN`/`MAX`: easy and hard direction, `=`, mixed with `SUM`, with an inner `when` (D3).
4. Strict `<` and `<>` over whole-numbered reducers.
5. Model size: ex. 1 on N rows has O(N) matrix entries (dump).
6. An unbounded variable under a hard `MIN`/`MAX` → the existing "bound this variable" refusal, never a wrong answer.
7. A square or a product of two non-Boolean decisions inside a rewritten reducer → "not supported yet".
8. Both solvers.

### C10 — old spellings removed

**How.** Each old production stays with a single action: the error named in the spec's §10.
Deleted with them: `WHEN_DECIDE_OBJECTIVE` and its lexer state, the parenthesis hints
in `decide_parse_hints.cpp`, `RewriteScopedVarRefs`, the old objective alternatives.
Done last (§7).

**Tests — `test_scope_removed_syntax.py`.** One case per row of the table → the error
contains the new spelling; `SUM(D: e)` still accepted. `test/decidb/test_parser.test`
and the round-trip matrix in `test/common/test_decidb_plan_serialization.cpp` move to
the new spellings.

### C11 — deck examples 1–8 run as written

**How.** Nothing of its own — the sum of C1–C9. Expected shapes: ex. 2, 4, 7 aggregate;
ex. 3, 6 rows; ex. 1, 5, 8 value rewrite.

**Tests — `test_scope_deck_examples.py`.** Examples 2, 3, 4, 6 and 7 are in it (items
3–4): each in both forms with an identical dump, the optimum = oracle, one row per
class, and ex. 7 on data where one depot has no priority shipment → the D8 error. Item 5
adds 1, 5 and 8 the same way.

---

## 7. Order of work

Seven items, one commit each: a feature plus the tests that prove it. Old spellings keep
working until item 7. Items 1 to 4 have shipped — see [`done.md`](done.md).

| Item | Delivers | Checklist |
|---|---|---|
| 5 | New constraint shapes for `SUM`, `AVG`, `MIN`, `MAX`: a plain term beside a reducer, reducers with different `by` keys, a row-varying factor — the value rewrite of §4.2. Tests `test_scope_value_rewrite.py`, `test_scope_deck_examples.py` (all eight, both written forms), `test_scope_combinations.py`, one golden entry per new shape | C7 C11 |
| 6 | Switch everything to the new spellings: EXPLAIN and DIAGNOSE echo them; golden corpus, parser tests, pytest suite and benchmarks converted; no other engine change. `golden/check.sh` identical on both solvers | — |
| 7 | Remove the old spellings: `T.x(TYPE)`, `scalar x(TYPE)`, postfix `PER`, postfix `WHEN` and `D.x` references raise the error naming the new spelling; dead code removed; `SUM(D: e)` stays; the behaviour file is folded into `syntax_reference.md`. Tests `test_scope_removed_syntax.py` | C10 |

Items 3 and 4 changed no old answer (D12, D17): the new rules apply to the new
spellings. Item 7 changes the old ones when it removes them (NULL keys, empty sets, a
varying bound under `per`) and rewrites the tests it affects.

Gate at the end of every step — all green:

```bash
make grammar-build && make release          # grammar-build only when the grammar moved
test/decide/.venv/bin/python3 -m pytest test/decide/tests/test_scope_*.py
make decide-test                            # default solver, includes the golden check
DECIDB_FORCE_SOLVER=highs make decide-test
DECIDB_VERIFY_SERIALIZER=1 test/decide/.venv/bin/python3 -m pytest test/decide/tests
```

---

## 8. Shared test pieces

- **`_scope_oracle.py`** — built (items 2–4): `ScopedModel` takes rows, decisions with
  keys and clauses as plain Python data (`Scoped`, `Reduce`, `Nested`), follows `ANR
  Implementation.pdf` literally, builds a gurobipy model and raises the three errors. Its
  `<`, `>` and `<>` step by one, exact for whole-numbered sides only, so an `AVG` under
  `<>` is judged by hand.
- **`test_scope_combinations.py`** — the grid `when {none, some}` × `per {omitted, row,
  (), K}` × body `{tuple only, one reducer, reducer with another by, two reducers}` ×
  inner `{none, when, per, both}` × `{sum, avg, min, max}` on tiny data, each cell
  against the oracle. A cell the engine refuses must give the "not supported yet"
  message — never a wrong answer or an internal error.
- **Golden corpus** — one new entry per new shape (ex. 1–8, a `MIN`/`MAX` value link,
  an unused `per`), captured for both solvers.
- **EXPLAIN and DIAGNOSE** — one case per construct (prefix scope, `by`, inner scope, a
  value variable's label) checking that a clause is echoed in the new spelling and
  that no internal name leaks.

**Converting what exists (item 6).** About 495 `PER` and 700 `WHEN` uses in ~50 test files,
75 `T.x` and 38 `scalar` declarations, `test/decide/golden/corpus.sql`, 15 benchmark
queries, the C++ and `test/decidb` parser tests, and the docs.

1. The oracles build their models in Python and never read SQL, so a converted test
   that still passes proves the new spelling means the same thing.
2. `corpus.sql` is converted by hand and `test/decide/golden/check.sh` must report
   identical models; any difference is read and explained.
3. Tests whose behaviour changes by decision are rewritten on purpose and listed in the
   commit: NULL keys under `PER` (D5); a `when` matching nothing on a per-row
   constraint, formerly a no-op (D4); groups formerly skipped for an empty filter
   (D8); a row-varying bound under `PER`, formerly tightest, now the C8 error unless
   `per` is left out.

Docs to rewrite in items 6 and 7: `00_project_overview/syntax_reference.md` §1, 2, 5–7; the
`decide`, `per`, `when`, `such_that`, `maximize_minimize`, `sql_functions` and
`explain` folders of `03_expressivity/` and its keyword table; stage docs 01–06 and 08
of `01_pipeline/`; `04_testing/` (new `scope/` area).

---

## 9. Risks

| Risk | Answer |
|---|---|
| A plan statement about current behaviour is wrong | Each item's first test pins the "today" behaviour it builds on, before changing it |
| A converted query silently changes meaning — `per g:` without `by (g)` | The old-`PER` error spells out `by (g)`; converted tests are judged by oracle and golden dump, not by eye |
| Key expressions read the wrong column after a join reorders columns | They join the hand-kept resolver and pruning lists; every key test has a join variant with the key left out of `SELECT` |
| A hard `MIN`/`MAX` value link needs a Big-M and a variable is unbounded | Existing refusal with its "bound this variable" message; native constraint where the backend has one |
| D4, D5 and D8 change answers of existing tests | Not before item 7 (D12); then listed and rewritten deliberately (§8, point 3) |
| DIAGNOSE on rewritten clauses names an internal variable | Value variables carry their reducer's SQL as label; definition rows are structural, so only the user's clause is ever offered for repair |

---

## 9a. Left open by items 3–4

- **Item 5's shapes answer "not supported yet"**: a per-row or keyed decision beside a
  decision reducer (deck ex. 1, 5, 8; the facility-location link), reducers with
  different `by` keys, a reducer inside another expression, a query-wide decision beside
  a reducer in a row that multiplies decisions. A row-varying factor on a decision
  reducer still gets today's message ("'w' varies per row ... Move it inside the
  aggregate"), which item 5 replaces with the value rewrite.
- **Refused until a later item**: `AVG` under `<>` beside another term, over a product,
  or with its own `when`/`per`; a `MIN` or `MAX` beside another term, or under a factor
  whose sign is not known before the query runs, with a `by` or a bound that is not a
  constant (the composed formulation is one row over every kept row); a strict bound on
  a `MIN` or `MAX` whose value can be fractional; a decision beside a `MIN` or `MAX`; a
  `when` inside either reducer of the two-level objective, a `per` inside its inner one,
  or a `by` key that is not its `per` key; a product of two non-Boolean decisions outside
  the objective's reducers; a window function or a reducer in a `when`, a key or a `by`;
  `norm(D: e, p)`.
- **Parse errors, not D14/D15 messages**: a postfix `WHEN`/`PER` after a clause written
  with a scope in front, and a `by` after anything but a function call (a column, a
  literal, `CASE`, `CAST`, `TRY_CAST`, `::`, `COALESCE`, `NULLIF`, `EXTRACT`,
  parentheses) or a scope inside such a form (the item-1 grammar admits neither). D14's
  and D15's messages cover every position the binder sees.
- **`per row` alone** leaves no node (item 1), so its clause keeps the old rules (D12):
  `per row: max(x) > 3` is the old `MAX(x) > 3`, with its logged bugs. Whether a written
  `per row` should switch a clause to the new rules is open.
- **DIAGNOSE** (item 6): an aggregate-shape clause's `group` column names its `by` group;
  the spec's §1 asks for the `per` key value. Its suggested edit quotes the clause in
  the new spelling, `per` included.
- **An `IN` under `per K:`** is emitted once per kept row, not per class: exact (the key
  determines the decision and the domain), one indicator set per row.
- **Old spellings**: wrong answers found next to the shared code are logged in
  `06_issues/bugs/todo.md`; they go with item 7 or are fixed on their own.

---

## 10. Not in this plan

Deck features left out on purpose: reducers nested in a constraint (ex. 9) and the
one-pass evaluation algorithm; variant signatures; frame selectors (`at`, `over`,
`within`); `if` guards; bounds in a declaration; `SEMIREAL`, `SEMIINT`, `TEXT`;
`then` and `satisfy`; scope-aware optimizer rewrites.

Follow-ups worth doing once this lands: let a keyed decision sit beside a reducer in
the aggregate shape without a value variable (the facility-location linking
constraint), and share one value variable between clauses that use the same reducer.
