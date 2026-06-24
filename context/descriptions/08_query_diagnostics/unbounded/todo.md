# Query Diagnostics — Unbounded (remaining)

The unbounded state is functionally complete (it names the escaping variables —
see `done.md`). What remains makes the diagnosis *more actionable*: tell the user
which slice escaped, and prescribe the forced fix. Only open work is listed here.

## `affected_rows` / `affected_entities` — residual enrichments

The categorical characterization itself is shipped (`done.md` · `affected_rows`).
Open enrichments:

- **Conjunctive rules.** Rules are an independent union of single columns today
  (`channel=export; region=APAC`). A conjunction (`channel=export AND region=APAC`)
  would localize finer when escape needs two predicates. Needs a rule-mining pass.
- **Child-output name resolver for SELECT-only columns.** A categorical column
  referenced only in the outer SELECT (not WHEN/PER/objective/constraint) carries no
  harvested name, so its rule is suppressed rather than labeled with a positional
  `colN`. A full child-output name resolver would recover the user's identifier and
  let such columns characterize the escape instead of being dropped.
- **Tuple display for a single escaping instance.** Currently a single escaping
  instance (or a single-instance variable) reports the bare count / NULL; showing
  the offending row's relevant-column tuple was deferred.
- **Entity-scoped non-key characterization.** Entity-scoped vars are characterized
  only by entity-key columns (the columns constant within an entity). A join column
  that varies within an entity is not used.
- **Continuous-column causes** (e.g. escape driven by `margin > 0`) are out of scope
  by design — categorical only.

## Direction / downward escape

`grows_toward` is always `+inf` and the `-inf` branch is unreachable today, because
variables are non-negative. Downward escape only becomes possible — and the `-inf`
path only becomes testable — once **signed/free variables** exist (tracked in
`03_expressivity/decide/todo.md`). When they do: open the lower bound in the ray
model (`BuildUnboundedRayFallbackModel` currently fixes `col_lower = 0`) and add a
free-variable oracle test for the `-inf` direction.

## Clause-aware context line (optional)

Name the clauses an escaping variable appears in — *context only* ("`x` appears in
clauses 2, 5; none cap it"), never blame, per the load-bearing limit in `done.md`.
Needs the clause-text plumbing shared with the infeasible engine. (The old static
caveat — "names the runaway variable, not a single guilty clause" — was **removed**
from the user error for concision, so it is no longer the cheaper alternative; any
context line, if built, stays opt-in detail, not inlined blame.)

## Test coverage gaps

Still uncovered: the fresh-connection empty-relation case (a second `decidb` process
with no prior failed solve in-session).
