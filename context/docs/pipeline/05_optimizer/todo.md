# Stage 05 — Optimizer: open work

Each item makes the model smaller or the language wider without changing any answer that runs.

## OPT-2 — `IN` under `per K:` once per class

**Why.** `RewriteInDomain` keeps the clause's `when` on the rows it emits but not its
`per K:`, so `per g: x IN (1, 2)` over a keyed `x` emits one indicator set per kept row
rather than per class: three rows in two classes give six indicators where four would
do. It is exact, because the key determines the decision and the domain.

**Decision needed.** None in the language; one in the design, below. The indicators are row-scoped auxiliaries;
emitting them per class needs them keyed by `K` (a scope like a helper's) and the `per K:`
wrapper kept on the generated rows, as the MIN/MAX equality's easy half keeps it.

**Design choice.** Which scope keys the indicators. Per class, the indicators need a decision scope keyed by
the wrapper's `K`. A scope is made only by the binder (declarations, reducer keys) and by the value rewrite (helpers);
every key, `K` included, is an entry of the one key table (`LogicalDecide::keys`), from which a scope can be made.
(a) Key the indicators by `K`'s entry. (b) For the case where `K` is the decision's own declared key only: the
indicators take the decision's scope and the rows keep the wrapper (as the MIN/MAX easy half keeps it); any other `K`
keeps one set per row. Recommended: (a); (b) is a special case beside the one key table, which (a) reads directly.

**Test.** The dump of `per g: x IN (1, 2)` over three rows in two classes shows four
indicators and four rows (a pick-one row and a linking row per class), not six and six;
DIAGNOSE still names the clause.

**done.md.** "NORM and IN — `RewriteNorm` / `RewriteInDomain`".

**Ruling (owner's delegate, 2026-10-29).** Neither: an indicator's scope is `JoinScopes(scope(x), scope(domain))`, so (b)
is a result, not a case; a data domain under `per K:` stays per row until a scope is made from the key table.

## OPT-4 — ABS, body helpers and `NORM_IN` take their auxiliary's scope by `JoinScopes`

**Why.** `JoinScopes` (`decide_optimizer.cpp`) gives a Boolean product's auxiliary the least upper bound of its
factors' scopes, so a query-wide product is one column. Three other sites still declare their auxiliary
`DecideVarScopeInfo::Row()`: the ABS auxiliary and its sign indicator (`decide_rewrite_abs.cpp:419`, `:356`), the
`__body_` helper (`decide_rewrite_define.cpp:333`), and the `NORM_IN` indicators (`decide_rewrite_norm_in.cpp:125`,
`:215`). Each is one column per row where its operands are query-wide or keyed.

**Design.** Each site's auxiliary takes `ScopeOf` its expression (F4's OPT-2: `JoinScopes` over the leaves it reads,
a data column making it `Row`) and is registered in `entity_scopes[..].scoped_variable_indices`. Its rows are wrapped in
`per <key>:` (or `per ()`), so execution emits one row per class; `LinearizeBilinear` cannot carry them, since their
coefficients are evaluated in execution.

**Done when.** Each site takes `JoinScopes` and the per-instance emission, with a test per site: a model-size test for
a query-wide expression, and an oracle-judged answer on both solvers.
