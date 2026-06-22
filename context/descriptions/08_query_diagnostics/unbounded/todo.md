# Query Diagnostics — Unbounded (remaining)

The unbounded state is functionally complete (it names the escaping variables —
see `done.md`). What remains makes the diagnosis *more actionable*: tell the user
which slice escaped, and prescribe the forced fix. Only open work is listed here.

## `group_label` — which instances escape

**Need.** When a variable has multiple scope-instances and only *some* escape, the
diagnosis should say *which*, so the user bounds the right slice instead of
re-deriving it. Today `BuildUnboundedDiagnostic` dedups by name and leaves
`group_label` NULL, so a partial escape is indistinguishable from a total one.

**Concept (precise).** The escaping *group* is the variable's **scope instance**,
not a `PER` group. (`PER` groups *constraint rows*; it does not create per-group
variables, and a decision variable is not a legal `PER` key — see
`syntax_reference.md` §7. What multiplies one variable *name* into many escaping
solver columns is its scope.) Each escaping column already maps to a
`(name, instance)` via `ColumnProvenance` (`foundations/done.md` · variable
provenance). The work:

- **Dedup by `(name, instance)`** when the variable is scoped, so partial escapes
  report separate rows (today's dedup-by-name collapses them).
- **Resolve the instance to a human label** and put it in `group_label`:
  - *entity/table-scoped* (`DECIDE Table.var`): the entity key column(s)=value(s),
    e.g. `empID=Alice`. The scope key exists in provenance; the missing piece is
    plumbing the key column name + value down to the diagnosis site.
  - *single-instance* variable (no scope multiplicity — the common case, e.g. the
    `run.sh` demo): genuinely NULL, nothing to disambiguate.

**Open design call (the one remaining fork).** For a *row-scoped* variable there
is no inherent group key — the instance is just a result row. Options for its
label: (a) leave NULL (only entity-scoped variables get labels); (b) characterize
escaping row-instances by a relevant data column (e.g. the `PER`/`WHEN` column of a
constraint the variable participates in), reporting one row per distinct value;
(c) report a raw row identifier. Decide before implementing row-scoped labeling —
this is a naming/granularity choice, not a coding detail.

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

The current suite only exercises the single-connection flow and full (not partial)
escape. Add: a partial entity-scoped escape (some instances escape, some bounded)
once `group_label` lands; the fresh-connection empty-relation case.
