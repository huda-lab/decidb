# Stage 02 — Binder: open work

Language-surface work is owned by `../../language/` (the scopes:
`../../language/scopes.md`); what the language does not accept is listed in
`../../language/errors.md`, and the scope suites in
`../../testing/scope_suite.md`.

## BIND-6 — A norm and a range frame refuse a query-wide body as any reducer does

- **Problem.** A reducer whose body is query-wide decisions only (`sum(y)` over a query-wide `y`) is refused at every
  position: its two readings (1 or `n` times `y`) are different problems (guide §2). `BindAggregate`
  (`decide_binder.cpp`) exempts a norm marker and a range frame's reducer (`binding_frame_range`), so both solve:
  `norm(d - 1, 0) <= 0` over a `per ()` `d` counts `n · [d ≠ 1]` (`test_norm.py`,
  `norm-l0-query-wide-is-one-indicator`), and a range frame over a query-wide payload reduces it once per frame row.
- **Ruling (owner, 2026-11-21).** No exemptions: a norm and a range frame's reducer over a query-wide body are refused
  like `sum(y)`, since once and once per row are different problems for them too. One rule with no case list.
- **Change.** The two exemptions leave the query-wide-body check in `BindAggregate`. A keyed body (`per k: d`) is not
  query-wide and stays accepted.
- **Done when.** `norm(d - 1, 0) <= 0` and a range frame over a query-wide payload raise *"'d' is a query-wide
  decision, so '…' has no rows to reduce"* (Binder) at the reducer; the tests that solved them become refusal tests;
  guide §2's sentence naming the exemptions and errors.md's clause go; `./test/decide/run_tests.sh -n 2 -k "norm or
  frame or binder"` passes.
