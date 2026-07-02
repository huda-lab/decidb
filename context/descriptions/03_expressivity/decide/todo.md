# DECIDE Clause — Planned Features

## Signed decision variables — finite negative bounds: DONE; free (-∞) domain: deferred


**Deferred — fully-free ($-\infty \ldots +\infty$) domain.** Out of scope by
design: a signed variable always has a finite lower bound. A truly
unbounded-below variable (e.g. `x <= 10` meaning `(-inf, 10]`) is the case most
likely to make objectives unbounded, so it is not expressible without a future
opt-in (`FREE`/`IS REAL UNBOUNDED` keyword). Two known smaller gaps left for
later: (1) **column-valued** `IN` domains with negative data values are not
auto-widened (only constant literals are); (2) a signed variable in a bilinear
product needs an *explicit* upper bound (implied-bound propagation skips signed
variables).

**Diagnostics interaction.** The unbounded diagnosis reports an escape
`direction` (`+∞` / `-∞`). Because signed variables still have a finite lower
bound, no variable is unbounded *below*, so downward escape remains unreachable
and `direction` is still always `+∞`. The `-∞` branch only becomes testable if
the deferred free-domain work lands. See
`08_query_diagnostics/unbounded/todo.md` (direction / downward escape).
