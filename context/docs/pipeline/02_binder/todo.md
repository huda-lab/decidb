# Stage 02 — Binder: open work

Language-surface work is owned by `../../language/` (the scopes:
`../../language/scopes.md`); what the language does not yet accept is listed in
`../../language/errors.md` §2, and the scope suites in
`../../testing/scope_suite.md`.

## BND-1 — The two-level objective under a factor or a minus

- **Problem.** `MAXIMIZE MAX(per k: SUM(x * v) by (k))` answers, but the same problem written
  `MINIMIZE -MAX(per k: SUM(x * v) by (k))`, and `MAXIMIZE 2 * MAX(per k: ...)`, are refused with
  `DECIDE: an aggregate inside an aggregate is not supported yet` (`decide_binder.cpp:259`; a roadmap row of
  `../../language/errors.md` §2). The binder reads the two-level form only at the objective's root.
- **Change.** The binder reads the two-level form under a constant factor or a unary minus as the objective's one
  term, and the MIN/MAX rewrites read its sign as they read every term's. Its row in `errors.md` §2 goes when it ships.
- **Done when.** `MINIMIZE -MAX(per k: ...)` answers as `MAXIMIZE MAX(per k: ...)`, and a factor on the two-level form
  answers as the oracle does; `./test/decide/run_tests.sh -n 2 -k per_objective` passes.
