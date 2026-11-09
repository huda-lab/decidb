# Stage 02 — Binder: open work

Language-surface work is owned by `../../language/` (the scopes:
`../../language/scopes.md`); what the language does not yet accept is listed in
`../../language/errors.md` §2, and the scope suites in
`../../testing/scope_suite.md`.

## BIND-1 — An outer column inside a DECIDE marker leaks the marker's name

- **Problem.** A DECIDE cannot read a column of an outer query (`../../language/guide.md` § Outer queries; `done.md`
  § the outer-query refusal): a plain outer column is refused at the end of `BindSelectNode` naming it, and one inside
  a reducer is refused quoting the reducer. An outer column inside a DECIDE marker construct is not: the clause's
  retry binds the marker in the outer query, where it is an unknown function. Under `LATERAL` over `(VALUES (1e0,'p'))
  o(cap, s)`:
  - `SUCH THAT ... AND when o.cap > 1: x <= 2` and `SUM(when o.cap > 1: x) <= 2`: *"Catalog Error: Scalar Function
    with name __scope_when__ does not exist!"*;
  - `SUM(x) by (o.s) <= 2`: the same for `__reducer_by__`, and `__scope_per__` for a `per` qualifier;
  - in a filter form the query fails with an INTERNAL Error (found by TEST-2, 2026-11-09).
  `DECIDE per o.s:` already gives a DECIDE message (*"o.s is neither a column nor a relation of the FROM clause"*),
  but not the outer-query one.
- **Change.** The cause is one: a marker is a DECIDE construct, and the outer query's binder has no such function.
  Refuse where the retry hands the marker to the outer binder, with the outer-query message naming the column, the
  same path the reducer case takes; no per-marker list.
- **Done when.** Each form above (and `per` with an outer key) raises the outer-query refusal naming the column, as
  tests; no internal name reaches a message.
- **Ruling (owner, 2026-11-09, delegated).** The forms reach the outer binder by three paths (a marker left intact for
  the retry, a tree the DECIDE binder took apart before the failing bind, and the per-key resolver with no retry), all
  from one cause: an outer column is discovered only when DuckDB's retry rebinds an expression in a binder that knows
  nothing of DECIDE. So the refusal moves to the column: where a column in a DECIDE clause fails to resolve against the
  query's own bind context and resolves in an outer query's binder, one helper raises the outer-query refusal naming
  it, before any retry; the per-key resolver calls the same helper. `BindSelectNode`'s end check stays as the backstop.
  No DECIDE predicate goes into DuckDB's function binding. The message names the column everywhere, a reducer
  included (it no longer quotes the reducer).

