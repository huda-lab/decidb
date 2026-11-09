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

