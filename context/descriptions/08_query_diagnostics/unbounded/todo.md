# Query Diagnostics — Unbounded (remaining work)

The unbounded engine is shipped end to end: it reaches a clean `UNBOUNDED` state,
extracts a portable recession ray, names the escaping variables, reports direction, and
characterizes affected rows/entities with categorical rules. See `done.md` for how it
currently works. The engine's actionable core — name the runaway variable, prescribe the
bound — is complete; the secondary `affected_rows` / `affected_entities` cell now also
names SELECT-only columns from their user-written projection aliases. What remains is a
single deferred item, blocked on unrelated expressivity work.

---

## Deferred

- **Downward escape (`grows_toward = -inf`).** Blocked until signed/free variables land
  (`03_expressivity/decide/todo.md`); today all user variables are non-negative, so `-inf` is
  unreachable and untestable. When they ship: open the lower bound in the ray-fallback box
  (`BuildUnboundedRayFallbackModel`, `diagnostic_solves.cpp`), assert an oracle-confirmed
  downward-escaping case, and drop the "always `+inf` today" caveat from `done.md`.
