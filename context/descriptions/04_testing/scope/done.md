# Scope Test Coverage — Done

Covers the scoping language: `per` in front of a declaration (`per K: x(TYPE)`,
`per (): x(TYPE)`), `when` and `per` in front of a constraint or the objective,
`when` and `per` inside a reducer, and `by (G)` after one. Syntax:
`00_project_overview/syntax_reference.md` §1.1, §2, §4–§7 and §10.3; semantics:
[`03_expressivity/scope/done.md`](../../03_expressivity/scope/done.md).

Tests live in:
- `test/decide/tests/_scope_oracle.py` — the reference evaluator most suites below judge against
- `test/decide/tests/_scope_cases.py` — the deck's data and shared helpers
- `test/decide/tests/_brute_force.py` — enumeration for shapes the scope oracle cannot state
- `test/decide/tests/test_scope_*.py` — twelve suites, one per part of the language
- `test/decide/tests/test_no_old_spellings_remain.py` with `_old_spellings.py` — the scanner that keeps removed spellings out of every test surface
- `test/decidb/test_scope_parser.test` — sqllogic: every spelling parses and binds (also inside a nested DECIDE; inside an ordinary subquery written in the clause it answers "not supported yet"), quoted keywords as names, the exact parse errors, and the removed spellings' messages

The feature suites written in the same language are tracked by feature:
[`../per/`](../per/done.md) (`test_per_*.py`), [`../when/`](../when/done.md)
(`test_when_*.py`, `test_aggregate_local_when.py`),
[`../entity_scope/`](../entity_scope/done.md) (keyed decisions) and
[`../scalar_scope/`](../scalar_scope/done.md) (query-wide decisions).

## The reference pieces

### `_scope_oracle.py`

The oracle reads no SQL. A test states the query's result rows, its decisions with
their keys, and its clauses as plain Python data, and the oracle builds the model the
language defines through the `OracleSolver` interface. Because it never sees the SQL,
a test that keeps passing after its SQL is respelled shows the two spellings mean the
same thing.

- **`ScopeModel`** — keyed declarations. A `Decision` is one per row (`PER_ROW`), one
  per class of a key of row columns, or one for the whole query (`GLOBAL`, `per ()`);
  the model has one solver column per class. Clauses are `RowClause` (once per row) and
  `SumClause` (one sum over all rows); an `Objective` counts bare query-wide terms once.
- **`ScopedModel`** — the clause scopes. `Scoped` is `[when c] [per K]: lhs sense rhs`,
  each side a sum of parts: a tuple term (a function of the row), a `Reduce`
  (`factor * agg([when c] [per K]: body) by (key)`) or, in the objective, a `Nested`
  two-level form `outer(per k: inner(body) by (k))`. A `ScopedObjective` is generated
  once. The oracle follows the ANR definition step by step: keep the rows the `when`
  keeps (a NULL condition drops the row), split the kept rows into classes of the `per`
  key, generate the body once per class, and evaluate each reducer on the kept rows
  that share the class's `by` value, after its own `when` and one value per class of
  its own `per` (the first kept row stands for its class).
- **NULL is a value like any other.** Rows whose key is NULL share one class
  (`class_ids`, `_split`), in declarations, in `per` and in `by`.
- **The three run-time errors.** `check` raises `ScopeError` before any model is built,
  carrying the phrase DeciDB's message carries: `kept_no_rows` ("matches no rows"),
  `not_determined` ("does not determine": every tuple term, factor and `by` value must
  be one value over a class) and `reducer_empty` ("has no rows").
- **Solving and judging share one generator.** `solve_scoped` returns the optimum of the
  model; `judge_scoped` checks DeciDB's answer, row-aligned with the oracle's rows, and
  returns the objective it reaches: every row of a decision's class shows one value, a
  whole-numbered decision is whole and within its bounds, and every generated clause
  holds. Both build their clauses with the same code, so they cannot drift apart.
- **Strict comparisons step onto whole numbers.** `< K` becomes `<= ceil(K) - 1`,
  `> K` becomes `>= floor(K) + 1`, and `<> K` an indicator pair over both. The judge
  checks the strict senses as strict. This is exact only when the decision side is
  whole-numbered, so an `AVG` under `<>` is judged by `_brute_force` or by hand.
- `scope_solver` (in `_scope_cases.py`) is Gurobi, solving afresh each time so that a
  change to the oracle is never hidden behind a cached optimum; a test that needs it is
  skipped where gurobipy is missing.

### `_scope_cases.py`

The deck's relations, small enough to check by hand: five shipments joined to three
depots in two regions and two countries, with a priority flag, a status and one network
capacity. `from_clause` renders them as VALUES (any rows can be swapped in) and
`joined` as the oracle's rows in shipmentID order. Also: `table` / `table_rows` for a
one-relation source, `answer`, `aligned` (asserts DeciDB's rows come in the oracle's
order), `dump_counts` (`num_vars` and `num_rows` from `DECIDB_DUMP_MODEL`) and
`error_of`.

### `_brute_force.py`

For a model the scope oracle cannot state — a product of decisions, `POWER`, an L0
count, `<>` over an `AVG` — a test names each solver column with a small whole-number
domain and writes the constraints and the objective as Python over one assignment.
`optimum` enumerates every assignment; `judge` checks DeciDB's answer against the same
statement and returns its objective.

## The suites

"Oracle" below means the answer is judged by `judge_scoped` (or `ScopeModel.judge`)
and must reach the oracle's optimum. "Dump" means the model size or the whole model
read from `DECIDB_DUMP_MODEL`. "Hand-checked" means the test asserts a small answer its
docstring derives.

- **`test_scope_declare.py`** — `DECIDE per K: x(TYPE)`. A column key, a relation key
  (every stored column of the relation), a key across two relations; `per ()` is one
  decision and no `per` is one per row; a key column absent from SELECT, WHERE and the
  clause survives column pruning on a base table, on VALUES and through a serialization
  round trip; a key is a set (order and repetition do not matter); NULL is one class;
  both clause orders; several declarators, each with its own key; qualified key names
  and the kept side of a `USING` column; result types; EXPLAIN; every key error; a
  keyed decision inside a colon-form reducer and beside a reducer. Oracle, dump and
  pinned messages.
- **`test_scope_generation.py`** — `[when c] [per K]: C`. One constraint per class
  (`per ()` one; `per row` or none, one per kept row); an unused `per` builds the bare
  clause's model; a scope covers one constraint and both halves of a `BETWEEN`; NULL is
  a class; classes and reducers read kept rows only; a NULL condition drops the row; a
  compound condition needs no parentheses; a subquery in the condition; nothing is
  computed on a row the `when` drops, so a NULL or a failing function there is no
  error; a `when` that keeps no row is an error (D4) while an input with no rows is
  not; the scope errors; the EXPLAIN echo, including a key or condition ending in a
  colon; a serialization round trip. Oracle and dump; the fractional strict bound and
  the dropped-row cases are hand-checked.
- **`test_scope_reducer_by.py`** — `agg(e) by (G)`. No `by` is `by ()`; a reducer reads
  rows outside its class; a data-only reducer on the bound side reads its own group;
  NULL is a group; a constant key and an all-NULL key are one group; a `by` key with a
  subquery; the tightest bound per group without `per`; MIN and MAX in both directions,
  under a factor, a strict bound and in a composed clause; AVG divides by the rows its
  `when` keeps; the refused shapes. Oracle and dump; `test_avg_not_equal` and
  `test_a_square_in_a_grouped_reducer` are hand-checked.
- **`test_scope_reducer_inner.py`** — `agg([when c] [per K]: e)`. The inner `when`
  filters its own reducer only; an inner `per` counts each class once, from its first
  kept row; the colon form `SUM(D: e)` builds the model of `sum(per D: e)` and takes a
  `by`; inner `per ()`; AVG divides by the values it receives; MAX is unaffected by an
  inner `per`; an inner key must determine its body; a reducer with no row for a group
  is an error naming the group (D8, D18). Oracle, dump and pinned messages.
- **`test_scope_objective.py`** — `MAXIMIZE|MINIMIZE [when c] [per ()]: E`. An omitted
  `per` is `per ()`; the `when` in front builds the same model as a `when` inside each
  reducer; a query-wide decision, or a keyed decision with one instance, beside a
  reducer counts once; tuple terms that are not one value are errors; the two-level
  form, also under a `when`, and its refusals (an outer AVG); a reducer nested in a
  constraint is refused; an objective `when` that keeps no row and an objective reducer
  with no row are errors. Oracle, dump and pinned messages.
- **`test_scope_determination.py`** — the determination rule. A `per` key must determine
  every tuple term (a bound, a factor), every decision outside reducers, every `by` key,
  and an inner key its reducer's body. Each case is a pair: the query runs on data where
  the key determines the value and stops with the named error where it does not. Only
  kept rows are compared and NULL is one value; without `per` the same clause is legal
  and takes the tightest bound; the errors are raised under DIAGNOSE too; messages quote
  the user's SQL only. Pinned messages.
- **`test_scope_value_rewrite.py`** — a reducer whose decision side mixes it with
  something a grouped row cannot hold (a per-row or keyed decision beside it, a factor
  that varies by row, a second `by` key, a reducer inside `abs` or a square) is built
  from a helper decision per reducer and `by` group. Covers SUM, AVG, MIN and MAX
  helpers, strict and `<>` comparisons over whole-numbered helpers, a helper that can be
  fractional (no step), helpers under a `when` and over a reducer with its own `per`,
  NULL as a group of the helper key, the model growing linearly, the scope checks still
  guarding the rewritten clause, a product of decisions inside a rewritten reducer
  refused, DIAGNOSE naming the clause rather than the helper, the EXPLAIN echo, and the
  same answers on HiGHS. Oracle, brute force and dump; the `abs`/square/product cases
  and `test_two_keys_hand_checked` are hand-checked.
- **`test_scope_bare_clauses.py`** — a clause with no `when`, `per` or `by` means its
  explicit form (`per row:` in front, `by ()` on each reducer, the objective once).
  Strict bounds step onto whole numbers; a negative factor on a hard MAX; an objective
  term must be one value; a reducer inside a reducer is refused; an easy `min(x) >= K`
  or `max(x) <= K` is absorbed into the decision's box (D41), so a negative K makes `x`
  signed and the model is the one the column bound builds; a constant inside MIN/MAX
  under a strict step is refused; query-wide decisions beside reducers under `<>`,
  beside boolean products and squared reducers, and in products inside and outside a
  reducer; `<>` on an AVG of boolean products; `norm(x, 0) by (g)` and
  `count(*) by (g)` (D34, D35); a data-only reducer scaling a decision inside a reducer
  is refused as a coefficient. Each runs on both solvers (a refusal is asserted where
  one cannot load the model). Oracle and brute force.
- **`test_scope_combinations.py`** — the grid: a `when` in front or not × `per` omitted
  / `row` / `()` / a key × a body with a tuple term only, one reducer, a decision beside
  a reducer, or two reducers with different `by` keys × the reducer's own scope (none,
  `when`, `per`, both) × SUM / AVG / MIN / MAX, over a per-row decision and a keyed one
  — 784 cells. Each cell has two allowed outcomes: the oracle's optimum, or the error
  the language defines for the data with the same phrase. Never "not supported yet", a
  wrong answer or an internal error. Oracle.
- **`test_scope_deck_examples.py`** — deck examples 1–8, as written and in their fully
  explicit form, over `DECIDE ship(INT), per D.depotID: reserve(REAL)`. Both forms build
  the same model, the model has one row per generated class (plus one definition row per
  helper for examples 1, 5 and 8), the optimum is the oracle's, and the answer satisfies
  every generated clause. Example 1 is written bare. Example 7 also runs on data where a
  depot has no priority shipment, which is the D8 error naming that class. Oracle and
  dump.
- **`test_scope_explain_diagnose.py`** — what EXPLAIN and DIAGNOSE print. EXPLAIN shows
  a clause as written, its canonical reading when that differs, auxiliaries as the
  expression they stand for and one account line per rewrite, including the objective's
  rewrites and the two-level key. The echo is in the plan style: lower-case reducers
  and keywords, `when c per K:` in front, a reducer's `by (G)` after it, and no
  `per row:` or `by ()`. DIAGNOSE, in the expanded slack scope, reports a `by`-grouped
  clause once per class: per value of its `per` key, or per kept row when it has none
  (D28); covers easy and hard MIN/MAX, absorbed and keyed bounds, unreachable bounds,
  composed clauses, quadratic clauses, the unbounded remedy naming a key column, and a
  suggested change that pastes back and runs. Pinned output text; no oracle.
- **`test_scope_removed_syntax.py`** — the one file that writes the removed spellings
  (`00_project_overview/syntax_reference.md` §10.3). Every removed declaration,
  constraint and objective spelling, in each variant the parser distinguishes, is
  refused with a message carrying "is removed" and the suggested spelling built from
  the user's text, keyword-named columns quoted. The refusal holds under EXPLAIN,
  DIAGNOSE, in a CTE, a nested DECIDE, a view and a prepared statement. A removed form
  written after a clause that already has a scope in front is a plain syntax error. A
  relation-qualified reference to a decision is refused in every position, with the
  data-only message inside `when`, `per` and `by`; a struct field or column named like
  a decision still binds. `scalar` is an ordinary name, the colon form stays, and a
  suggested spelling pastes back and runs. Pinned message fragments.

## Keeping removed spellings out

- **The scanner.** `test_no_old_spellings_remain.py` runs `_old_spellings.scan_file`
  over every test surface — `test/decide/**/*.py`, `test/decidb/*.test`,
  `test/common/test_decidb_*.cpp`, `test/decide/golden/corpus.sql`,
  `benchmark/decide/queries/*.sql` and `run.sh` — one parametrized case per file. It
  flags the removed declarations, a `when` or `per` written after the clause it scopes,
  and a relation-qualified reference to a declared decision (§10.3). It is token-aware:
  comments and string literals are skipped, `CASE ... END` and ordinary subqueries are
  skipped, and a `when` or `per` that reaches its colon is the scope spelling. From Python it
  reads string literals and f-strings, not docstrings, assertion messages or regex
  patterns. Deliberate cases are skipped: `test_scope_removed_syntax.py` (and the
  scanner's own two files) as a whole, a sqllogic block whose comment or text says
  "removed spelling" or whose `statement error` expects a syntax error, and a C++ string
  literal with "removed spelling" on its line or the two above.
- **The runtime guard.** Every query a test runs through `decidb_cli.py` (`execute`,
  `execute_raw`, `execute_script`, `execute_interactive`) fails its test when stderr
  carries "is removed", so a loose error pattern cannot pass on a removed-spelling
  error by accident. `execute_raw(..., removed_spelling=True)` opts in; only
  `test_scope_removed_syntax.py` does.
- **The leak guard beside it.** The same runner calls
  `_output_helpers.assert_no_internal_leak` on every query: an INTERNAL error or an
  internal `__tag__` in the output fails the test, unless the `__name__` is text the
  query itself contains. `test_error_time_limit.py` and `test_query_diagnostics_slow.py`
  start the CLI with `subprocess` directly and bypass both guards.

## Coverage tiers

| Tier | Where |
|------|-------|
| Oracle (`_scope_oracle`) | declare, generation, reducer_by, reducer_inner, objective, value_rewrite, bare_clauses, combinations, deck_examples |
| Brute force (`_brute_force`) | bare_clauses, value_rewrite |
| Model dump | declare, generation, reducer_by, reducer_inner, objective, value_rewrite, deck_examples |
| Hand-checked | listed under each suite above |
| Error / message pins | determination, removed_syntax, and the error cases of every suite |
| Output text pins | explain_diagnose, and the echo cases of generation and value_rewrite |
| Scanner and guards | `test_no_old_spellings_remain.py`, `decidb_cli.py` |
