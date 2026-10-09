# ANR language surface — plan

> **Status (2026-10-09): items 1 (the new spellings parse) and 2 (`decide per K:
> x(TYPE)`) have shipped — see [`done.md`](done.md); items 3–7 remain.**
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
| D10 | (2026-10-09) A decision declared with `per` inside the old `SUM(D: e)` is refused ("not supported yet") until item 3 | user |
| D11 | (2026-10-09) Messages name a `per` decision as declared; `T.x` messages stay word for word; DIAGNOSE labels its scope `entity` until item 6 | user |

Small calls made while planning. All were put to the user on 2026-10-05 and none was
vetoed.

| # | Call |
|---|---|
| S1 | `per row` is accepted as the written-out default |
| S2 | Decisions are referenced by bare name; `D.open` in a constraint is no longer accepted |
| S3 | With no `per`, a constraint is per row — so `sum(x) <= cap` keeps today's meaning (the tightest `cap`); with an explicit `per`, a varying bound is the C8 error |
| S4 | `SUM(D: e)` uses the same data check as `sum(per D: e)`; today's bind-time rule for it goes away |
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

`LogicalDecide::scope_checks` — one entry per user clause and per reducer scope inside
it: the `when`, the key, and the items that must be constant per class (a data
expression, a decision, a `by` key), each with the SQL text to quote. Stage 4 builds it
from the user's canonical clauses, **before** any rewrite; stage 8 runs it before
formulation. It raises the three run-time errors of the spec's §9: no kept rows, key
does not determine, reducer empty for a class.

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
| 2 | Binder | Binds the parser's remaining markers -- `SCOPE_WHEN`, `SCOPE_PER` outside a declaration, `REDUCER_BY` (`decide.hpp`) -- shrinking `ValidateDecideNoUnsupportedScope` as each lands. `by` and inner `per` get key scopes from `FindOrCreateKeyScope` (§4.1; item 2 built it for declarations) and become scope tags on the aggregate; renaming the scope enum to ROW / KEYED / GLOBAL (today's ROW / ENTITY / SCALAR) goes with item 7; inner `when` stays `filter`. The colon reducer binds through the same code. The objective's two-level nested form binds onto today's nested-objective tree. Removed: `RewriteScopedVarRefs`, `CheckQualifiedReducerBody`, the "do not mix the two WHENs" rule | `decide_declarations_binder.cpp`, `decide_binder.cpp`, `decide_constraints_binder.cpp`, `decide_objective_binder.cpp` |
| 3 | Logical plan | `key_scopes` replaces `entity_scopes` + `entity_key_expressions`; new `scope_checks`. Both serialized, and both added to the hand-kept expression lists (binding resolution, column pruning) | `logical_decide.hpp/.cpp`, `logical_operator.json`, `nodes.json`, `column_binding_resolver.cpp` |
| 4 | Canonicalizer | Rule C5 (no mixing) is replaced by the shape classification of §4.2. Rule C6: a factor on a reducer may be any decision-free term; whole-query is required only to stay in the aggregate shape. An empty-key PER wrapper is legal. Builds `scope_checks` | `decide_canonicalizer.cpp/.hpp`, `decide_constraint_walk.hpp` |
| 5 | Optimizer | `RewriteScopedReducers` (§4.2). The linear form fills an aggregate clause's grouping from its shared `by` key | `decide_optimizer.cpp`, new `decide_rewrite_scoped.cpp`, `decide_linear_form.cpp`, `decide_prepared_model.hpp` |
| 6 | Model | `EmitReducerValueLinks`: definition rows per group; the value variable's box comes from its members' reach | `linearization_minmax.cpp`, `ilp_linearization.cpp` |
| 7 | Solver | none | — |
| 8 | Execution | One key-grouping cache; run `scope_checks` first; first-row-of-class mask for explicit `per`; bound-side reducers evaluated over their own `by` groups (`EvaluateRhsReducerPerGroup`); evaluate value-link members | `physical_decide.cpp`, `plan_decide.cpp` |
| — | Rendering | EXPLAIN and DIAGNOSE print the new spellings; a value variable is labelled with its reducer's SQL | `decide_source_provenance.cpp` |

---

## 6. Checklist: how each item is built and verified

New tests live in `test/decide/tests/test_scope_*.py`. "Dump" means the built model
read through `DECIDB_DUMP_MODEL`; "oracle" is the reference evaluator of §8. Error tests
match the fixed phrases and messages of the spec's §9.

### C3 — constraint `per K:`

**How.** `scope ':' comparison` arrives as the `SCOPE_PER` marker outside the
`SCOPE_WHEN` marker (`decide.hpp`); the binder maps them onto today's bound PER/WHEN
wrappers, so every pass that walks those wrappers is unchanged. `per` no longer requires an
aggregate and no longer groups reducers by itself. Classes come from the unified
grouping; the rows shape keeps the first row of each class.

**Tests — `test_scope_generation.py`.**
1. `per K` over a keyed decision: constraint count = classes (dump), optimum = oracle.
2. `per ()` → one constraint; identical dump to the same aggregate-only clause without `per`.
3. `per row:` ≡ omitted.
4. Unused `per` (`per g: sum(x) <= 10`) accepted, one row in the dump (D6).
5. A scope covers one constraint: in `per g: A AND B`, only `A` is scoped.

### C4 — `when` filters known rows before `per`

**How.** The `SCOPE_WHEN` marker binds onto today's WHEN wrapper. Reducers in the body
see only kept rows, as expression-level WHEN already works.

**Tests — same file.**
1. `when c per K: …`: a class with no kept row does not exist — no constraint, no error.
2. The reducer reads kept rows only: `when S.priority: ship <= 0.5 * sum(ship)` against the oracle.
3. NULL condition = false.
4. Compound conditions without parentheses (`when a = 1 and b > 2 per g: …`).
5. A decision in the condition → error.
6. A `when` keeping no row → error for a rows clause, an aggregate clause and an objective (D4); an input with no rows at all → unchanged (S9).

### C5 — postfix `by (Γ)`

**How.** The parser takes `by` after any call's `)`; the binder refuses it unless the
call is `SUM`, `AVG`, `MIN` or `MAX` (§9). Γ binds as data-only
expressions into a key scope, stamped on the aggregate's alias the way
`__qualified_by_N__` is today; no `by` → no tag. Group ids come from the unified
grouping over kept rows. A bound-side reducer is evaluated per its own group.

**Tests — `test_scope_reducer_by.py`.**
1. Omitted ≡ `by ()` (identical dump).
2. Column key, two-column key, expression key (`by (S.dispatchDay + S.transitDays)`).
3. A reducer reading rows outside its own class (`per D.depotID … by (D.region)`), oracle.
4. A data-only reducer with `by` on the bound side.
5. NULL group key = one group (D5).
6. Errors: a decision in Γ; `by` after something that is not a reducer.

### C6 — `when` and `per` inside a reducer

**How.** Inner `when` → `BoundAggregateExpression::filter` (today's aggregate-local
WHEN, new spelling). Inner `per K'` → the existing qualifier tag pointing at a key
scope; columns are allowed, not only relations, and `SUM(D: e)` binds through the same
code (D1). De-duplication stays the keep-first-row mask (`BuildQualifierKeepMask`), now
per (group, inner class). The bind-time rule "everything inside must come from D" is
replaced by the data check of C8.

**Tests — `test_scope_reducer_inner.py`.**
1. Inner `when` filters only its own reducer.
2. `sum(per D: e)` ≡ `SUM(D: e)` (identical dump); a depot is charged once across a fan-out join.
3. Column key (`sum(per D.depotID: e)`); `when` + `per` together; inner `per ()`.
4. `AVG` divides by kept rows / distinct inner classes, per group.
5. Combined with `by` and an outer `per`.
6. Inner key that does not determine the body → named error; same query on data where it does → runs.
7. A reducer empty for one class → error naming the class (D8); moving the filter in front of the constraint fixes it.
8. A scope inside a call that is not `SUM`, `AVG`, `MIN` or `MAX` (`norm(when c: e, 1)`, which the parser accepts on the first argument) → bind error; its message is not in the spec's §9 yet.

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

### C8 — the `per` key must determine tuple terms and `by` keys

**How.** §4.3. Raised at execution, before any solve, with the message of the spec's
§9. The fixed phrase `does not determine` and the two quoted names are what tests match.

**Tests — `test_scope_determination.py`**, each as a pair (holds → runs, violated → error):
1. A bound column varies inside a class.
2. A `by` key varies inside a class (deck p. 27).
3. A decision is not determined (a per-row decision under `per K`).
4. A factor on a reducer varies inside a class.
5. `per ()` with a non-constant column.
6. An objective term (`MAXIMIZE x` with per-row `x`).
7. An inner `per` (C6).
8. The message quotes the user's SQL only — no internal names.
9. The same clause without `per` is legal and equals the tightest bound per group (S3, oracle).
10. Still raised under `DIAGNOSE`: nothing was solved.

### C9 — objective: omitted `per` means `per ()`

**How.** The two-level form binds onto today's nested-objective tree (D2); any other
nesting → "not supported yet".

**Tests — `test_scope_objective.py`.**
1. Omitted ≡ `per ():` (identical dump).
2. `MAXIMIZE when c: sum(e)` ≡ `MAXIMIZE sum(when c: e)`.
3. The nine `outer(per k: inner(e) by (k))` combinations against the existing oracles.
4. `by` key ≠ inner `per` key, three levels, or a nested reducer in a constraint (deck ex. 9) → "not supported yet".

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

**Tests — `test_scope_deck_examples.py`.** Fixture: small hand-checkable `Shipment` and
`Depot` tables and a one-row `Network`. Each example in its original **and** its fully
explicit form: identical dump; optimum = oracle; constraint count = expected classes;
both solvers. Ex. 7 also on data where one depot has no priority shipment → the D8 error.

---

## 7. Order of work

Seven items, one commit each: a feature plus the tests that prove it. Old spellings keep
working until item 7. Items 1 and 2 have shipped — see [`done.md`](done.md).

| Item | Delivers | Checklist |
|---|---|---|
| 3 | Reducer scope: `by (Γ)` (also on the bound side; NULL is a group), `when` and `per` inside a reducer with column keys, `SUM(D: e)` through the same code, the data checks that the inner key determines the body and that no reducer is left without rows; the old `C PER g` runs as `by (g)` on each reducer. Tests `test_scope_reducer_by.py`, `test_scope_reducer_inner.py`; existing `PER`, aggregate `WHEN` and `SUM(D: e)` tests and the golden models must not change except the cases rewritten for D5 and D8 | C5 C6 |
| 4 | Constraint and objective scope: `when c:` (any condition; keeping no row is an error), `per K:` without implicit grouping, `per ()`, `per row`, NULL as a class, the determination check with its named error; objective `per ()` only, `when` in front, the two-level nested form. Tests `test_scope_generation.py`, `test_scope_determination.py`, `test_scope_objective.py`; deck examples 2, 3, 4, 6, 7 | C3 C4 C8 C9 |
| 5 | New constraint shapes for `SUM`, `AVG`, `MIN`, `MAX`: a plain term beside a reducer, reducers with different `by` keys, a row-varying factor — the value rewrite of §4.2. Tests `test_scope_value_rewrite.py`, `test_scope_deck_examples.py` (all eight, both written forms), `test_scope_combinations.py`, one golden entry per new shape | C7 C11 |
| 6 | Switch everything to the new spellings: EXPLAIN and DIAGNOSE echo them; golden corpus, parser tests, pytest suite and benchmarks converted; no other engine change. `golden/check.sh` identical on both solvers | — |
| 7 | Remove the old spellings: `T.x(TYPE)`, `scalar x(TYPE)`, postfix `PER`, postfix `WHEN` and `D.x` references raise the error naming the new spelling; dead code removed; `SUM(D: e)` stays; the behaviour file is folded into `syntax_reference.md`. Tests `test_scope_removed_syntax.py` | C10 |

Items 3 and 4 change existing behaviour (NULL keys, empty sets, a varying bound under
`per`); each rewrites the few existing tests it affects in the same commit.

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

- **`_scope_oracle.py`** — a reference evaluator of about 150 lines. Input: rows,
  variables with keys, and clauses as plain Python data (when, per, terms, reducers
  with by / when / per). It follows `ANR Implementation.pdf` literally — filter,
  classes, group lookup, inner filter and de-duplication — builds a gurobipy model,
  and raises the same three errors. It parses no SQL.
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
| D4, D5 and D8 change answers of existing tests | Listed and rewritten deliberately (§8, point 3) |
| DIAGNOSE on rewritten clauses names an internal variable | Value variables carry their reducer's SQL as label; definition rows are structural, so only the user's clause is ever offered for repair |
| Until item 7, an old reducer `WHEN` after a `by` or a scoped call (`sum(x) by (g) WHEN c <= 1`) parses as a whole-constraint `WHEN` whose condition takes the bound | To settle before the first item that binds a new spelling: refuse a constraint that mixes old and new spellings, naming the new one |

---

## 10. Not in this plan

Deck features left out on purpose: reducers nested in a constraint (ex. 9) and the
one-pass evaluation algorithm; variant signatures; frame selectors (`at`, `over`,
`within`); `if` guards; bounds in a declaration; `SEMIREAL`, `SEMIINT`, `TEXT`;
`then` and `satisfy`; scope-aware optimizer rewrites.

Follow-ups worth doing once this lands: let a keyed decision sit beside a reducer in
the aggregate shape without a value variable (the facility-location linking
constraint), and share one value variable between clauses that use the same reducer.
