# Stage 05 — Optimizer: open work

Each item makes the model smaller or the language wider without changing any answer that runs.

## OPT-4 — ABS, body helpers and `NORM_IN` take their auxiliary's scope by `JoinScopes`

**Why.** `JoinScopes` (`decide_optimizer.cpp`) gives a Boolean product's auxiliary the least upper bound of its
factors' scopes, so a query-wide product is one column. Three other sites still declare their auxiliary
`DecideVarScopeInfo::Row()`: the ABS auxiliary and its sign indicator (`decide_rewrite_abs.cpp:419`, `:356`), the
`__body_` helper (`decide_rewrite_define.cpp:333`), and the `NORM_IN` indicators (`decide_rewrite_norm_in.cpp:125`,
`:215`). Each is one column per row where its operands are query-wide or keyed.

**Design.** Each site's auxiliary takes `ScopeOf` its expression (`JoinScopes` over the leaves it reads, a data column
making it `Row`; `done.md`, NORM and IN) and is registered in `keyed_scopes[..].scoped_variable_indices`. Its rows go
through `ScopeRowPerInstance`, so execution emits one row per instance; `LinearizeBilinear` cannot carry them, since
their coefficients are evaluated in execution. The canonicalizer's `ScopeKeyOf` folds the same lattice over key indices
(`JoinDecideKeys`), so `ScopeOf` can become one fold over it.

**Done when.** Each site takes `JoinScopes` and the per-instance emission, with a test per site: a model-size test for
a query-wide expression, and an oracle-judged answer on both solvers.
