# Known Bugs — Open

## Degenerate `BETWEEN` bypasses composed MIN/MAX equality rejection

- **Location:** `src/optimizer/decide/decide_rewrite_minmax.cpp`; current behavior
  is pinned in `test/decide/tests/test_min_max.py`.
- **Observed:** `SUM(x * v) + MAX(x * v) = K` is rejected, while the equivalent
  `SUM(x * v) + MAX(x * v) BETWEEN K AND K` solves because `BETWEEN` has already
  become two directional comparisons.
- **Expected:** reject equal endpoints for a composed MIN/MAX constraint while
  direct equality is unsupported, without breaking a genuine interval.
- **Tracking:** implementation and acceptance details are in
  `../../04_testing/min_max/todo.md`.

Resolved behavior is documented by its owning `done.md`.
