# Query Diagnostics — Unbounded (remaining)

The unbounded state is functionally complete (it names the escaping variables —
see `done.md`). What remains makes the diagnosis *more actionable*: tell the user
which slice escaped, and prescribe the forced fix. Only open work is listed here.

## `escaping_instances` — **shipped** (residual enrichments below)

The categorical characterization landed — `done.md` · `escaping_instances` has the
full description (categorical sufficient-direction rules, total-escape summary,
count fallback, the three pragma knobs, row- vs entity-scoped). Residual future
work only:

- **Conjunctive rules.** Rules are an independent union of single columns today
  (`channel=export; region=APAC`). A conjunction (`channel=export AND region=APAC`)
  would localize finer when escape needs two predicates. Needs a rule-mining pass.
- **SELECT-only categorical columns named positionally.** Names are harvested from
  the DECIDE clause's `BoundReferenceExpression`s (`plan_decide.cpp`); a categorical
  column referenced only in the outer SELECT (not WHEN/PER/objective/constraint)
  gets a `colN` fallback name. Rare (such columns are usually high-cardinality and
  excluded), but a full child-output name resolver would fix it.
- **Tuple display for a single escaping instance.** Currently a single escaping
  instance (or a single-instance variable) reports the bare count / NULL; showing
  the offending row's relevant-column tuple was deferred.
- **Entity-scoped non-key characterization.** Entity-scoped vars are characterized
  only by entity-key columns (the columns constant within an entity). A join column
  that varies within an entity is not used.
- **Continuous-column causes** (e.g. escape driven by `margin > 0`) are out of scope
  by design — categorical only.

## `suggested_bound` — deferred design decision

Explicitly deferred (user). The relation names the runaway variable but does not
yet prescribe a value to cap it at, and there is no defensible number to put here
(a wrong cap anchors the user; the right cap is domain knowledge). Whether this
column becomes a concrete value, a non-anchoring remedy hint, or is dropped is an
open design decision to settle before any work. Separately, see "Prescribe the
fix" below for surfacing the *forced* remedy without committing to a number.

## Prescribe the fix (least-change wording)

The least-change promise says name the smallest edit that restores a usable
solution. For unbounded the edit is forced and known: add an upper bound (or
correct a sign). The diagnosis currently names the variable but stops short of
saying so. Add the remedy to the summary — e.g. "add an upper bound such as
`SUCH THAT x <= <cap>`" — without inventing a number. (This is the actionable half
of `suggested_bound`; it can ship independently of the column's design call.)

## Advertise the opt-in (agreed)

With no pragma, an unbounded solve throws the legacy static error, which never
mentions that a diagnosis is available. Append one line to the default
unbounded/infeasible error pointing at the opt-in (e.g. "for a diagnosis, set
`PRAGMA diagnose_decide='auto'` and re-run"). Advertises without auto-solving, so
manual-first is preserved.

## Same-session caveat

The diagnosis is stashed per-connection, so the `SELECT * FROM
decide_diagnostics()` the error points to returns an empty relation if run on a
fresh connection (e.g. a second `decidb -c …`). Either note "(in this session)"
in the pointer text, or have `decide_diagnostics()` explain an empty stash.

## Direction / downward escape

`direction` is always `+∞` and the `-∞` branch is unreachable today, because
variables are non-negative. Downward escape only becomes possible — and the `-∞`
path only becomes testable — once **signed/free variables** exist (tracked in
`03_expressivity/decide/todo.md`). When they do: open the lower bound in the ray
model (`BuildUnboundedRayFallbackModel` currently fixes `col_lower = 0`) and add a
free-variable oracle test for the `-∞` direction.

## Clause-aware context line (optional)

Name the clauses an escaping variable appears in — *context only* ("`x` appears in
clauses 2, 5; none cap it"), never blame, per the load-bearing limit in `done.md`.
Needs the clause-text plumbing shared with the infeasible engine. A static caveat
("DeciDB can name the runaway variable but not a single guilty clause") is a
cheaper alternative that sets the right expectation.

## Test coverage gaps

Partial-escape characterization (row-scoped rule, entity-scoped rule, scattered →
count fallback, total escape, escape-rate pragma) now lands in
`test_query_diagnostics_escaping_instances.py` + the `CharacterizeEscape` unit test.
Still uncovered:
- the `categorical_ratio` / `min_categories` knobs changing what is reported (only
  `escape_rate` is exercised);
- the fresh-connection empty-relation case (a second `decidb` process with no prior
  failed solve in-session).
