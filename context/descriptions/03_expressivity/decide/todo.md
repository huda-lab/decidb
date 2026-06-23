# DECIDE Clause — Planned Features

## Signed decision variables — finite negative bounds: DONE; free (-∞) domain: deferred

**Implemented (see `done.md` → "Signed variables").** A variable becomes signed
via an explicit **finite** negative lower bound — `x >= -K`, `x BETWEEN -K AND K`,
or a negative literal in an `IN` domain. The default stays `[0, +inf)` for any
variable the query never lowers. The linearizations that were audited and
addressed: the bound-absorption clamp and the model-builder re-clamp (both
removed in favor of an "unset" sentinel + resolved-bound copy), and McCormick
(now emits the full four-corner envelope + widens the aux bound for `L<0`). ABS /
`norm` L0/L1/Linf / MIN-MAX / `<>` Big-M were verified already sign-safe (their
Big-M uses `max(|lb|,|ub|)`); the IN rewrite now widens the variable's lower
bound to the domain minimum for negative literals.

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
