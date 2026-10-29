# Known bugs

## BUG-1 — `DECIDB_TIME_LIMIT` is not enforced on HiGHS for large models

- **Location:** `src/decidb/naive/deterministic_naive.cpp` (`HighsSession`);
  contrast `src/decidb/gurobi/gurobi_solver.cpp:125`.
- **Observed:** on Q2 at the large TPC-H tier (~1.5M variables, mixed-integer),
  `highs.optimize` began and never returned — not at the requested 300s
  `DECIDB_TIME_LIMIT`, not by 420s wall clock, when an external harness killed
  the process. Every DeciDB-owned span (parse, rewrite, extraction, model
  build, `highs.load`) completed in under 350ms total; the entire remainder
  was spent inside HiGHS's own `run()`. Seen on the large benchmark tier (query Q2 on
  HiGHS, status `process_timeout`); see `../performance/findings.md#solver-execution`.
- **Cause:** `HighsSession` inherits `SolverSession::SetInterruptPoll`'s
  no-op base instead of overriding it, so a HiGHS solve has no external
  cancellation path — the only thing bounding it is HiGHS's own internal
  check of the `time_limit` option, and that check's granularity was too
  coarse to return within any reasonable margin of the budget on this model.
  Gurobi does override `SetInterruptPoll` and is not affected.
- **Expected:** `DECIDB_TIME_LIMIT` bounds a HiGHS solve the same way it
  bounds Gurobi's — the query returns with a `TIME_LIMIT` status near the
  requested budget, not an unbounded hang.
- **Impact:** a user who sets a time limit expecting a bounded wait can have
  the query hang indefinitely on HiGHS once the model is large enough; there
  is no DeciDB-level safety net.
- **Task.** `../pipeline/07_solver/todo.md` § SOL-1.

## BUG-2 — One matrix run of `DECIDB_TEST_DIRECT_SOLVE=off` reported one outcome fewer

- **Observed:** on 2026-10-27 `test/decide/run_direct_matrix.sh` printed `off: 4520 passed, 335 xfailed, 0 failed`
  (4,855 outcomes) where `auto` and `serializer` printed 4,521 + 335. The rerun of the off suite gave 4,521 passed and 0
  skipped. The matrix wrote its logs to a `mktemp -d` folder that was gone, so the missing test is unknown.
- **Next step:** the matrix now keeps its logs (`test/decide/results/matrix_<time>/`) and prints each mode's outcome
  total; when the totals differ again, find the missing test from the logs.

## BUG-3 — Q10 on HiGHS at the large tier is killed for memory, and the profiler records it as a timeout

- **Location:** HiGHS on `benchmark/decide/queries/q10_maximize_abs.sql` at `large`; the skip is
  `SKIP = {("Q10", "highs")}` in `benchmark/decide/profile_pipeline.py`, whose comment says the bug is recorded here.
- **Observed:** in the 2026-09-09 large sweep (`benchmark/decide/results/pipeline_profile_large.csv`,
  `sweep_large.log`), Q10 on Gurobi finishes in all three passes (about 72 to 75 s, peak 6.1 to 6.5 GiB). Q10 on HiGHS
  repeat 0 is recorded as `process_timeout` with an empty `query_s`, and the sweep restarts twice at that cell before the
  skip was added. On `medium`, Q10 on HiGHS peaks at 6,525 MiB at 2.5M variables (`../performance/findings.md#memory-and-scaling`).
- **Expected:** the query finishes, or fails with an ordinary out-of-memory error; the profiler records a memory kill
  as a memory failure, not a timeout.
- **Impact:** a large HiGHS query can take the whole process down, and the profile misreports why.
- **Reproduce:** `make decide-bench-setup`, remove the entry from `SKIP`, run `profile_pipeline.py`. Memory-heavy.

## BUG-4 — Messages print a subquery, a condition or a `by` key as DuckDB renders it

- **Location:** a subquery's source fragment, and a `by` key's text, is its parsed
  expression's `ToString` (`TagDecideSourceFragments` in `decide_source_provenance.cpp`,
  `BindReducerBy`), and a condition is re-rendered from its bound form
  (`RenderDecideSource`).
- **Observed:** `when v in (SELECT 1000): ...` is quoted as `when (v = ANY(SELECT
  1000))`; `when exists (SELECT 1 WHERE false): ...` gives
  *"... when EXISTS(SELECT 1 WHERE CAST('f' AS BOOLEAN)) ..."*; `when id > 100 and g is
  not null:` gives *"when id > 100 AND (g IS NOT NULL)"*. A `VALUES` list in a subquery
  becomes `(SELECT * FROM (VALUES ('East', 30)) AS valueslist) AS c(region, cap)`, in a
  `when` message and in the NULL-subquery message alike; `by (id % 2)` is quoted as
  `by ((id % 2))`.
- **Expected:** the text as written. That needs each node's source span, which the
  parser does not keep (only its start).
- **Impact:** messages only; they stay recognizable.
- **Ruling (owner, 2026-10-29).** Written text is cut from the query, never re-rendered (architect review done).
  - The parser tracks bison's `{start, end}` locations: `YYLTYPE` (`scanner.hpp`) becomes a struct with an implicit
    `int` conversion so every `@n` compiles unchanged, `YYLLOC_DEFAULT` sets the end from the last right-hand symbol,
    and the lexer sets each token's end (`scan.l`, regenerated with `scripts/generate_flex.py`). Start offsets, and so
    DuckDB's error positions, do not change; no node outside DECIDE grows.
  - `scripts/generate_flex.py` first reproduces the committed `src_backend_parser_scan.cpp` byte for byte from an
    unchanged `scan.l` (today a plain run differs by about 87 `int`/`size_t` lines), in its own commit.
  - Spans are recorded only at the DECIDE grammar positions (clause, scope, `when`, `per`/`by` keys) and on both sides
    of the six comparison operators, whose node location is the operator (`select.y`, the `@2` actions); a subquery
    gets no span of its own. They go in a side table of `{node, start, end}`, filled only while a DECIDE clause is
    parsed and carried on `PGDecideClause` to the transformer, so no shared parser node grows.
  - **Transport (owner, 2026-10-29, architect review).** A source map, as Clang's SourceManager and DuckDB's
    `CreateInfo::sql`: `SelectNode` keeps the DECIDE tail's text (`decide_tail`, SUCH THAT to the end of the
    objective), its start offset, and the spans relative to it, keyed by `(anchor, role)`: the anchor is the node's
    `query_location`, which DuckDB keeps through copy, binding and serialization; the roles are comparison left,
    comparison right, `when`, `per`, `by`, and (LP-2) column. The parser records `{anchor, role, start, end}`; the
    transformer stores the map and tags nothing; binders cut text by `(query_location - tail_start, role)`.
    `SelectNode::ToString` prints the tail as typed when the text is present, so a print-and-parse round trip (the
    serializer check, prepared statements) gets identical spans. No text rides in a parsed node's alias, so no
    alias-stripping in `FunctionExpression::ToString`, no moving marker text before binding, no consumed tag.
    `SelectNode::Equals` ignores the text, as it ignores `query_location`. A macro argument inside a DECIDE clause
    gets a test first; if its location can collide with the body's spans, stop: the fix is keying spans by their
    source text, not a text-match guard.
  - The transformer gets the string actually parsed (both `Parser::ParseQuery` paths) and, while transforming the
    DECIDE clause only, cuts each span's text into the existing serialized `source_fragments` registry, as DuckDB
    cuts `CreateInfo::sql`. Written lhs, rhs, qualifier and key text are these cuts; `WhenQualifier`,
    `PerClassQualifier`, `ScopedInQualifier`, `FrontQualifier`, `AppendQualifier` and the key display text from
    `DecideKeyToString` go.
  - Implicit casts and folded constants are not in the text. A `BETWEEN`'s halves are made by the binder and take the
    fallback, as today; quoting it whole is `../diagnostics/infeasible/todo.md` § INF-3.
  - `source_lhs` / `source_rhs` (the text an edit is spelled from) stay tree-rendered; only `written_*`, the
    qualifier and key text are cuts.
  - Cut text keeps the user's case: `SUM(x) <= 100` echoes `SUM(...)`. D32's lower-casing of reducers and keywords
    no longer applies to quoted text (lower-casing a cut would re-render it through a keyword list); the rest of D32
    stands (qualifier in front, no synthesized `per row:`, `by ()` not printed). Pins of the lower-cased echo assert
    the typed text exactly.
  - Every column reference prints as written (the D45 change, `../pipeline/03_logical_plan/todo.md` § LP-2).
  - The tree renderer (`RenderSource`) stays only for text DeciDB writes itself: the canonical layer, moved bounds,
    edited clauses. A cast or subquery inside a moved term keeps DuckDB's rendering there. Anything with no span (a
    node the binder makes up, a view or macro stored by an older build) takes one `ToString` fallback, not a branch
    per node kind.
  - The NULL-subquery message quotes its clause (from the span), not the subquery.

## BUG-5 — Messages raised while choosing a formulation name columns without their relation

- **Location:** stage 05 has no bind context, so `RenderDecideSource` there (the
  composed walker's *"'s' beside MIN or MAX"* in `decide_rewrite_minmax.cpp`, the
  objective's product refusal in `decide_linear_form.cpp`) cannot qualify a column by its
  relation over a join; stage 04's refusals can (`QuoteMessagesWith`).
- **Observed:** over the deck's join, `MAXIMIZE when S.priority: sum(ship) - max(ship) +
  D.capacity * s` gives *"DECIDE: 'capacity * s' beside MIN or MAX is not supported
  yet"*. A reducer's `by` key keeps its written text either way.
- **Expected:** `'D.capacity * s'`: stage 05 carries the relation names (a `LogicalDecide`
  field, serialized), or the refusal moves to stage 04.
- **Impact:** messages only.
- **Ruling (owner, 2026-10-29).** No BUG-5 code; it closes with BUG-4 and LP-2. Each bound column carries its
  spelling as written (LP-2), which `RenderSource` prints at any stage, so a stage-05 quote reads `D.capacity` where
  the user wrote it. The example above no longer reaches either site (the objective walk keeps a non-MIN/MAX term
  beside one). Find a query that reaches `decide_rewrite_minmax.cpp` (the `beside` refusal) and
  `decide_linear_form.cpp` (the product refusal, the 4-argument overload) with a data column over a join; each
  that is reached gets a test, each that is not reached at all is deleted.

## BUG-7 — A MIN/MAX `<>` refusal drops the clause's qualifier

- **Observed:** the refusal for `<>` on a MIN/MAX quotes `when p: max(x) <> 2` (and
  `per K:`, `when p per K:`, `per ():`) without its qualifier. Every other message site
  attaches it.
- **Location:** `decide_rewrite_minmax.cpp` calls `WrittenDecideComparison` without
  `AttachDecideQualifier`.
- **Expected:** the clause as written, qualifier included. A bare `per row:` is not printed
  in any message, by design (D32), and pasting a suggested edit back without it gives the
  same model. **Ruling (owner, 2026-10-28).** D32 stands. **Widened (owner, 2026-10-29):**
  the cause is that a message site can quote a clause without its qualifier at all.
  Every message that quotes a clause goes through one quoting function that attaches the
  qualifier; no site calls `WrittenDecideComparison` on its own. Audit every caller,
  route each through it, and test the refusal names `when p:` and `per K:`.
  Audit (2026-10-29): two shapes of quote, each composed by hand at every site. A clause as
  written is `AttachDecideQualifier(WrittenDecideComparison(source), source.qualifier)`
  (`physical_decide.cpp` ×2, `decide_diagnostic_engines.cpp`). An edited
  clause is `AttachDecideQualifier(lhs + " " + sense + " " + rhs, qualifier)`
  (`decide_diagnostic_render.cpp` ×4, `deterministic_naive.cpp`). The minmax refusal is
  the one site that skipped the attach.
  **Ruling (owner, 2026-10-29), architect review done.** The audit missed a second site: the `<>` refusal over a
  product or power (`decide_rewrite_notequal_avg.cpp`, `RenderForPlan(comp)`) and its `ne_clause_labels` also drop
  the qualifier.
  - Two functions beside `ConstraintSourceInfo` (`decide_source_info.hpp`, bodies in a new
    `src/common/decide_source_info.cpp`): `QuoteWrittenClause(source)` and
    `QuoteEditedClause(source *, lhs, cmp, rhs)` (a null source quotes a row with no clause bare). The object that
    holds the qualifier does the quoting (Information Expert). `WrittenDecideComparison` and `AttachDecideQualifier`
    become `static` there.
  - `MakeClauseLabel`, `MakeLoosenEdit` and `MakeVirtualOffsetEdit` take the source instead of a qualifier string;
    `SourceQualifier` goes.
  - Stage 05 reads a comparison's source through one adapter beside `RenderForPlan`, used by the MIN/MAX and the
    product `<>` refusals. The untagged fallback in the MIN/MAX refusal is deleted if no query reaches it.
  - `qualifier` stays a public field; the tests at each site are the guard.
  - EXPLAIN's rewritten rows keep their own qualifier, taken from the rewritten plan: a rewrite can put a row under a
    different `per` than its clause.
  - The `<>` label keeps a reference to its clause and is quoted when printed; no second string is stored.
  - Tests, one per site, each failing if that site drops the qualifier: the MIN/MAX `<>` refusal and the product
    `<>` refusal under `when p:` and `per K:`, the DROP label, the HiGHS refusal. If `physical_decide.cpp`'s bound
    site never sees a qualified clause, record that instead of a test.
- **Impact:** one known message; the audit says whether there are more.

## BUG-8 — Messages: operands, coefficients and keys

Each reproduces on the current build.

- **Two keys over the same columns keep the key text of the one bound first**
  (cosmetic): with `per D.d, D.cost: open(BOOL)` declared, `SUM(D: D.cost * open) <= 7`
  is echoed by EXPLAIN as `sum(D.d, D."cost": "cost" * open) <= 7`, and a determination
  message quotes `(in sum(D.d, D."cost": ...))`; declared `per D: open(BOOL)`, the same
  clause echoes `sum(D: "cost" * open) <= 7`. Both keys name the same columns, so they
  share one entry of the key table (`FindOrCreateDecideKey`, `logical_decide.cpp`), which
  holds one key text. Expected: each clause echoed as written.
- **A column reference is named in two forms:** `KeyElements` (`decide_binder.cpp`) names a key column's
  reference by its SQL spelling (`t.depot`, `t."my col"`, `D."cost"`); every other reference
  carries its bare column name. Anything that prints a reference's name has to tell the two
  apart. The unbounded remedy (`plan_decide.cpp`, the name harvest) does it by comparing the
  name with the binder's source name: equal is bare and is quoted where SQL needs it,
  different is taken as SQL. A reference whose name differs from the source only in case
  is taken as SQL unquoted. Expected: one form on every reference (the bare name, with the
  key's SQL spelling kept beside it, as `DecideKeyElement::name` already keeps it), so no
  printer infers which it holds. Same field as the shared key text above.
- **Impact:** messages only.
- **Needs.** `../pipeline/03_logical_plan/todo.md` § LP-2, whose design records display text per occurrence for both
  bullets. Under the 2026-10-29 ruling a clause's key text is cut from its own `per`/`by` span (BUG-4), so the first
  bullet closes with LP-2; the second closes when every reference carries its spelling as written.

## BUG-9 — A `BETWEEN` echoes its lower half only

- **Observed:** `x BETWEEN 18 AND 25` beside `SUM(x) <= 20` over two rows reports the clause `x >= 18` and suggests
  `x >= 10`; pasted back, the upper half is lost. The binder splits a BETWEEN into two comparisons that share the
  clause. Reproduces on the current build.
- **Impact:** a suggested change that does not repair the query.
- **Task.** `../diagnostics/infeasible/todo.md` § INF-3.

## BUG-10 — A quadratic clause is reported once per row or group in the query scope

- **Observed:** quadratic rows never share a slack (I2.d), so `power(x - 20, 2) <= 4` beside `x >= 25` over two rows
  gives two identical query-scope findings with no row to tell them apart; the expanded scope names each class (D28).
  Reproduces on the current build.
- **Impact:** findings the user cannot tell apart.
- **Needs.** The agent finds the cause and stops with a proposed fix before coding.

## BUG-11 — HiGHS's expanded repair can carry a 1e-5 cushion

- **Observed:** `x <> -1 AND ... AND per (): sum(x) + max(x) <= -5 AND per row: x >= 18` suggests
  `per (): sum(x) + max(x) <= 1e-05` (amount 5.00001) in the expanded scope where Gurobi and the query scope say
  `<= 0`; the edit over-repairs by 1e-5. Reproduces on the current build.
- **Needs.** The agent finds the cause and stops with a proposed fix before coding.

## BUG-12 — Wrong answer: a NULL-key row's bound limits a `per K:` clause

- **Observed:** over rows `(id, g, cap)` = `(1,'a',5)`, `(2,NULL,1)`, `(3,'a',5)`,
  `DECIDE x(INT) SUCH THAT x <= 3 AND per g: SUM(x) <= cap MAXIMIZE SUM(x)` answers a total of 1. D5 puts the NULL
  row in no class and D6 bounds the whole total once per class of `g`, so the one instance is class `'a'`,
  `SUM(x) <= 5`, and the optimum is 5. The NULL row's `cap` is read as a limit of its own. Reproduces on the current
  build, solver path; the direct rules (F6) copy the solver, so both paths agree and no test pins it.
- **Impact:** a wrong optimum, silently, whenever a `per K:` key is NULL on a row whose bound is tighter.
- **Needs.** The agent finds where the solver path instantiates a class for a NULL key and stops with a proposed fix
  before coding; the fix covers both paths.

## Limitations (not bugs)

- **A decision as a factor on a reducer is refused:** `b * sum(x) <= 5` with
  `per (): b(BOOL)` (or an INT `s`, in a constraint or in the objective) answers *"'b' is
  a decision, so it cannot multiply SUM(x). Only data can scale
  SUM/AVG/MIN/MAX."* (`decide_linear_form.cpp`); the product written inside the reducer,
  `sum(b * x)`, is a bilinear term and answers. `../language/guide.md` §5 states the
  refusal. **Ruling (owner, 2026-10-27).** Lift it for a `BOOL` factor only: one McCormick helper over
  the reducer's value, as the bilinear rules of §4 linearize any `BOOL` product. An `INT`
  or `REAL` factor stays refused; its product is not convex.
  A lift changes §5; the `INT` pins `test_canonicalize_scale.py::test_decision_factor_rejected` and
  `::test_decision_factor_in_objective_rejected` stay. **Task.**
  `../pipeline/04_canonicalizer/todo.md` § CAN-1.
- **A quadratic row refuses a row-varying factor on a query-wide decision that the linear
  row accepts:** with `s` declared `per ()` and `gcap` varying by row,
  `sum(x) + gcap * s <= 30` is generated per row and answers, while
  `sum(power(x, 2)) + gcap * s <= 30` answers *"'sum(power(x, 2))' is not supported yet in
  this constraint: ... with nothing beside them but data and query-wide decisions, each on
  its own"* (`decide_rewrite_scoped.cpp`); `sum(power(x, 2)) + s <= 30` answers. No test
  covers this shape. **Ruling (owner, 2026-10-27).** Lift it: the linear row accepts the same factor,
  so the refusal is an inconsistency, not a policy. **Task.** `../pipeline/05_optimizer/todo.md` § OPT-3.

Resolved behavior is documented by its owning `done.md`.
