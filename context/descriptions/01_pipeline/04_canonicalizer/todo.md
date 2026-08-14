# Stage 04 — Canonicalizer: open work

**None.** Constraints and objectives share one boundary, the verifier covers
both at all three insertion points, and the pass is total, pure and idempotent.

One item that was once tracked here has moved to the area that owns it:

| Item | Now owned by |
|---|---|
| `DecideDegreeInternal` under-estimates degree through `POWER` | [`../../06_issues/code_quality/todo.md`](../../06_issues/code_quality/todo.md) |

Neither has a canonicalization dependency in either direction.

---

## Before changing anything here

Read `done.md` §6 first. The cast policy, the side-agnostic provenance path,
reducer-scale composition, scope-aware homogeneity validation and the objective
boundary are all settled, and each was settled against a specific failure that
re-litigating would reintroduce. If a change appears to need a fourth call site, a
fourth placement case, or a per-site "is this safe here?" predicate, that is the
signal to re-read §3.6 and §3.7 rather than to add one.

Stop and report any new wrong-answer case before expanding scope.
