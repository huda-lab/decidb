# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved bugs are moved to `done.md`.

---

## L0 norm can over-count zero expressions in lower-bound/equality/maximize contexts

- **Location**: `src/planner/binder/query_node/bind_select_node.cpp:551` (`RewriteNormL0`)
- **Discovered**: 2026-06-29 while reviewing teammate's norm implementation
- **Symptom**: `norm(e, 0[, M])` introduces one indicator `z` per row and only enforces `e != 0 => z = 1` via `ABS(e) <= M*z` or the auto-M two-sided equivalent. It does not enforce `z = 0` when `e = 0`.
- **Reproduction**:
  ```sql
  SELECT id, x
  FROM (VALUES (1), (2)) t(id)
  DECIDE x IS REAL
  SUCH THAT x = 0 AND norm(x, 0, 100) >= 2
  MINIMIZE SUM(x);
  ```
  This should be infeasible because the true L0 count is 0, but it currently returns both rows with `x = 0`.
- **Cause**: The one-way Big-M link is sound for minimization penalties and `norm(e, 0) <= K` caps, where the solver has pressure to keep false-positive indicators at 0. It is unsound for `norm(e, 0) >= K`, equality, negative-weight minimization, and `MAXIMIZE norm(e, 0)` because false-positive `z = 1` values can inflate the count.
- **Where to look next**: Either reject unsupported L0 contexts at bind/optimizer time, or implement an exact nonzero indicator formulation with a tolerance/epsilon policy and finite bounds.
