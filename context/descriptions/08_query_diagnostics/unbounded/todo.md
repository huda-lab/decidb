# Query Diagnostics — Unbounded (remaining work)

The unbounded engine is shipped end to end: it reaches a clean `UNBOUNDED` state,
extracts a portable recession ray, names the escaping variables, reports direction, and
characterizes affected rows/entities with categorical rules. See `done.md` for how it
currently works. What remains makes the escaping-slice explanation richer without
changing core solver behavior.

Substantive tasks carry **Location** / **Problem** / **Decision** / **Test** / **Done**;
the lower-priority residuals stay one-liners.

---

## Escaping-slice enrichment (`affected_rows` / `affected_entities`)

### T1 — Conjunctive categorical rules

- **Location**: `decide_diagnostic.cpp` (`CharacterizeEscape`) + the pure tests in
  `test_decidb_escape_characterization.cpp`; candidate groupings built in
  `physical_decide.cpp` (`UnboundedCandidateProvider`).
- **Problem**: rules are an independent union of single columns today
  (`channel=export; region=APAC`). When the true escaping slice needs two predicates
  (`region='APAC' AND channel='export'`), output falls back to a bare count or prints
  broad one-column hints that are not specific enough.
- **Decision**: cap the search at pairwise conjunctions, or allow wider ones with
  pruning? And the ordering rule when a single-column and a conjunctive rule both clear
  the escape-rate threshold.
- **Test**: a constructed case where no single column clears the threshold but a
  two-column conjunction identifies the slice exactly; assert the formatted string plus a
  pure `CharacterizeEscape` unit test for thresholding and deterministic order.
- **Done**: `done.md` "`affected_rows` / `affected_entities`" — single-column vs.
  conjunctive rules and the configured search cap.

### T2 — Child-output name resolver for SELECT-only columns

- **Location**: `plan_decide.cpp` (`input_column_names` harvest); suppression currently in
  `physical_decide.cpp` (`BuildRowGrouping`).
- **Problem**: a low-cardinality column referenced only in the outer `SELECT` (not
  WHEN/PER/objective/constraint) can perfectly characterize the slice but carries no
  harvested source name, so its rule is suppressed and the user sees only the bare count.
- **Decision**: recover names from the child output only when the name is unambiguous and
  user-written, or allow generated aliases too? Keep suppression for expressions whose
  displayed name would mislead.
- **Test**: convert the existing select-only case in
  `test_query_diagnostics_escaping_instances.py` from count fallback to a named rule; add
  a negative case for an unsafe alias that should stay suppressed.
- **Done**: `done.md` "Mechanism" — replace the SELECT-only suppression limitation with
  the name-resolution contract.

### T3 — Tuple display for a single escaping instance

- **Location**: `decide_diagnostic.cpp` (`FormatEscapingInstances` /
  `BuildUnboundedDiagnostic`) + the candidate provider that would supply representative
  column values for one row/entity.
- **Problem**: single-instance escape has no useful `affected_rows` value — a
  single-instance variable reads NULL, a scattered one-row escape falls back to
  `1 of N rows`. Correct but unhelpful when the user needs to find the offending
  row/entity.
- **Decision**: which columns are eligible for the tuple and how to cap it. Conservative
  default: named categorical columns already eligible for characterization, capped to a
  small fixed count; no wide rows or high-cardinality IDs.
- **Test**: a constructed one-row escape asserting a compact tuple value, plus a cap test
  proving wide input does not produce an unreadable cell.
- **Done**: `done.md` fallback semantics for single-instance and one-row scattered escapes.

### T4 — Entity-scoped non-key characterization

- **Location**: the entity key-column harvest in `physical_decide.cpp`
  (`UnboundedCandidateProvider`) + `CharacterizeEscape` (`decide_diagnostic.cpp`).
- **Problem**: entity-scoped variables are characterized only by entity-key columns
  (constant within an entity) and constant join columns (`done.md`). A join column that
  *varies* within an entity is never used, so an escape localized by such a column falls
  back to the bare count.
- **Decision**: is a within-entity-varying column even meaningful to report for an entity
  subject (it is not constant for the entity), or does this stay out of scope? Settle that
  before building — it may be a documented limitation rather than a task.
- **Test**: a constructed entity-scoped escape where only a within-entity-varying column
  separates the escaping entities.
- **Done**: `done.md` entity-characterization paragraph.

> **PER / WHEN group context (note, low priority).** `done.md` states PER is covered only
> indirectly — escaping instances are never mapped onto PER groups. The categorical scan
> surfaces a PER/WHEN column when it happens to be categorical, but the engine does not
> directly report "these PER groups are uncapped" or reuse the group labels the infeasible
> engine already builds. Context only, never clause blame. Promote to a task if a real case
> needs it.

---

## Direction / downward escape

### T5 — Downward escape once signed/free variables exist

- **Location**: `diagnostic_solves.cpp` (`BuildUnboundedRayFallbackModel`, currently fixes
  `col_lower = 0`) + the model-builder domain plumbing for signed/free variables.
- **Problem**: `grows_toward = -inf` is unreachable today because user variables are
  non-negative. The public schema already supports `-inf`, but no test can exercise it
  until signed/free variables exist.
- **Decision**: blocked until the signed/free-variable syntax lands in
  `03_expressivity/decide/todo.md`. When it does: open the lower bound in the ray box and
  decide how finite lower/upper bounds gate the box for each direction.
- **Test**: after signed/free variables ship, an oracle-confirmed unbounded case whose
  objective improves by driving a variable downward; assert `grows_toward = -inf`.
- **Done**: `done.md` "The output relation" / "Ray extraction" — remove the "always
  `+inf` today" caveat.

---

## Coverage

### T6 — Fresh-connection empty diagnostics relation

- **Location**: `test_query_diagnostics_relation.py`, or a small CLI test using two
  separate connections/processes.
- **Problem**: the stash is per-connection, but no pinned test asserts that a fresh
  connection with no prior failed solve returns an empty relation. A lifecycle regression
  guard, not an engine feature (no design choice — either harness is fine).
- **Test**: fail a DECIDE in connection A; open connection B and assert
  `SELECT * FROM decide_diagnostics()` returns no rows.
- **Done**: add to the `done.md` tests list only if it becomes documented coverage.

---

## Clause-aware context line (optional)

Name the clauses an escaping variable appears in — *context only* (`x` appears in clauses
2, 5; none cap it), never blame, per the load-bearing limit in `done.md`. Needs the
clause-text plumbing shared with the infeasible engine. (The old static caveat — "names
the runaway variable, not a single guilty clause" — was **removed** from the user error
for concision, so it is no longer the cheaper alternative; any context line, if built,
stays opt-in detail, not inlined blame.)

---

## Out of scope unless the contract changes

- **Continuous-column causes.** The escaping-slice contract is categorical: it can say
  `where region = 'APAC'`, not `where margin > 0` or `where price in [10, 20]`. Numeric /
  range characterization would need a different rule-mining policy (bucketing / threshold
  discovery) and output that does not pretend an arbitrary cutoff is a fact from the query.
  Keep out of the active backlog unless we explicitly decide unbounded diagnostics should
  explain numeric ranges.
