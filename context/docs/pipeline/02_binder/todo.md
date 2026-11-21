# Stage 02 — Binder: open work

Language-surface work is owned by `../../language/` (the scopes:
`../../language/scopes.md`); what the language does not accept is listed in
`../../language/errors.md`, and the scope suites in
`../../testing/scope_suite.md`.

## BIND-6 — Query-wide body refusal for a norm and a range frame

- **Decision needed (owner).** A reducer whose body is query-wide decisions only (`sum(y)` over a query-wide `y`) is
  refused at every position: its two readings (1 or `n` times `y`) are different problems (guide §2). Does that refusal
  cover a `norm` and a range frame's reducer?
- **Problem.** Both solve today and are tested (`norm(d - 1, 0)` over a query-wide `d`; a range frame over a
  query-wide payload). `BindAggregate` exempts a norm marker and `binding_frame_range`.
- **Options.**
  1. No exemptions: both are refused like `sum(y)`, since once and once per row are different problems for them too,
     and their tests become refusals.
  2. Keep the exemptions: a norm counts rows by definition and a range frame reduces its frame's rows.
- **Recommendation.** 1, one rule with no case list.
- **Done when.** The owner has ruled. Under 1, `norm(d - 1, 0)` and a range frame over a query-wide payload raise the
  query-wide-body refusal, the two exemptions leave `BindAggregate`, the guide §2 sentence naming them goes, and the
  tests that solved them expect the refusal. Under 2, the task is deleted.
