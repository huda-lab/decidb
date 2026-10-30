# Code quality

Code-quality issues include duplication, dead code, fragile patterns, unclear
naming, missing coverage, and diagnostic-quality defects that do not change the
ordinary optimization result. Solve-correctness bugs belong in [bugs.md](bugs.md).

Each entry: short title, location (`file:line`), what's wrong, why it matters,
and when/during which task it was discovered.

## Messages reprint expression trees instead of quoting the source text

Location: `src/planner/decide/decide_source_provenance.cpp` (`RenderDecideSource`), `TagDecideSourceFragments`,
`decide_source_info.hpp` (`DecideSourceText`). A message that names a value renders a bound tree, so it shows
what the engine built, not what the user wrote, and the renderer carries a growing list of tags that exist only for
it (`NULL_GUARD`, `L0_AUTO_BOUND`, `SKIP_NULL`, `COLON_FORM`, the text in `BOUND_REDUCER_TAG`'s alias).
`TagDecideSourceFragments` stores `ParsedExpression::ToString()`, itself a reprint. `DecideSourceText` already holds
spans keyed by query location (Clang's SourceManager pattern) for six roles. The fix with no case list: record one
span per expression of the DECIDE clause at bind time, carry it on `query_location` (kept by `Copy` and
serialization), let a fold or an implicit cast keep its root's location, and cut a located node's span before
reprinting anything. Risk: libpg_query keeps only start positions, so span ends need a tokenizer with paren
balancing or grammar work; messages and goldens that quote a reprint would change. Found 2026-10-30 while scoping
PERF-16 (architect review). The same cause shows in two more messages: `if(c, a, b)` and `iif` reach the binder as
the parser's CASE, so `SUM(x * if(s = 'F', 1, 2))` is refused as a CASE the user never wrote, and `per g: SUM(x) by
(CASE WHEN ...) <= 3` quotes the parser's expanded CASE in its determination message (lane AW, BUG-42).
