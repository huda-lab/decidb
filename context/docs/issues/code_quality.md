# Code quality

Code-quality issues include duplication, dead code, fragile patterns, unclear
naming, missing coverage, and diagnostic-quality defects that do not change the
ordinary optimization result. Solve-correctness bugs belong in [bugs.md](bugs.md).

Each entry: short title, location (`file:line`), what's wrong, why it matters,
and when/during which task it was discovered.

## MSG-1 — Messages reprint expression trees instead of quoting the source text

Location: `src/planner/decide/decide_source_provenance.cpp` (`RenderDecideSource`), `TagDecideSourceFragments`,
`decide_source_info.hpp` (`DecideSourceText`). A message that names a value renders a bound tree, so it shows
what the engine built, not what the user wrote, and the renderer carries a growing list of tags that exist only for
it (`NULL_GUARD_TAG`, `L0_AUTO_BOUND_TAG`, `SKIP_NULL_TAG`, `COLON_FORM_TAG`, the text in `WRITTEN_VALUE_TAG`'s alias).
`TagDecideSourceFragments` stores `ParsedExpression::ToString()`, itself a reprint. `DecideSourceText` already holds
spans keyed by query location (Clang's SourceManager pattern) for eight roles, a product, power or function call
(`TERM`) and the objective among them, which is how a respelled product is quoted as written. The fix with no case
list: record one span per expression of the DECIDE clause at bind time, carry it on `query_location` (kept by `Copy` and
serialization), let a fold or an implicit cast keep its root's location, and cut a located node's span before
reprinting anything. Risk: libpg_query keeps only start positions, so span ends need a tokenizer with paren
balancing or grammar work; messages and goldens that quote a reprint would change. Found 2026-10-30 while scoping
PERF-16 (architect review). The same cause shows in two more messages: `if(c, a, b)` and `iif` reach the binder as
the parser's CASE, so `SUM(if(s = 'F', x, 2 * x))` is refused as a CASE the user never wrote, and `per g: SUM(x) by
(CASE WHEN ...) <= 3` quotes the parser's expanded CASE in its determination message.
Three more, from BLOAT-27: `SUM(x) * (w * b) <= 7` names the term
`"*"(CAST(CAST(w AS DECIMAL(12,1)) AS DECIMAL(38,1)), sum(x))`; a CASE factor prints the bound CASE; `MAXIMIZE (w +
1) * SUM(x)` prints `CAST(w AS DECIMAL(12,1)) + CAST(1 AS DECIMAL(12,1))`. Done when every message that names a
value quotes a span of the query as written, the renderer's tag list is gone, and these examples are tests.

## FMT-1 — DeciDB sources indented with spaces fail the formatter

Location: nine files under `src/decidb/` (among them `src/decidb/formulation/ilp_model_builder.cpp`,
`ilp_linearization.cpp`, `linearization_bigm.cpp`, `linearization_minmax.cpp`, `linearization_not_equal.cpp`), one in
`src/execution/operator/decide/` and two in `src/planner/expression_binder/decide/`. They are indented with four
spaces where `.clang-format` asks for tabs, and `make format-check` (which covers `src/`) reports them. A change that
formats only its own lines, as the repository asks, leaves tab lines among space lines in them. The fix is one
formatting-only commit, `make format-fix` limited to these files, made when no lane has edits open in them; it changes
no behavior. Found 2026-11-08 in BLOAT-30's review.
