# EXPLAIN — open work

## Layered constraint rendering: as-written → canonical → rewritten

**Goal**. The Constraints section prints one form per constraint — whatever tree
happens to sit on `LogicalDecide` at render time. That conflates three things and
gives the user no way to see what became of their query:

1. **As written** — `demand - sum(ship) <= cap`
2. **Canonical** — decisions and reducers left, data right: `-sum(ship) - cap <= -demand`
3. **Rewritten** — after the optimizer: AVG→SUM, ABS/MIN/MAX linearization,
   McCormick, `<>` indicators, plus the auxiliary constraints those passes emit

**Why**. The gap between what a user writes and what the solver receives is
invisible today. Someone who writes `MAX(x) <= K` and gets a cheap per-row rewrite
sees the same EXPLAIN as someone who writes `MAX(x) >= K` and gets a Big-M
encoding with an indicator per row. Layer 3 is also the first place auxiliary
variables (`__abs_aux_0__`, `__minmax_y_3__`) become explicable — they surface in
diagnostics today with no account of where they came from.

**What already supports it**. `DecideCanonicalizer` is a **pure function**: it
returns a new tree and leaves its input untouched. That is deliberate and is the
seam this rests on — at every call site the pre-image and the canonical form are
live locals at the same instant, so this is a rendering job rather than a
re-plumbing job. An in-place mutator would destroy layer 1 at the moment of
canonicalization. Layer 3 needs no new hook either: optimizer-emitted constraints
all arrive through `LogicalDecide::AddConstraint`, and in-place rewrites substitute
leaves rather than moving terms across the relation, so a snapshot taken at the end
of `DecideOptimizer::Optimize` diffs cleanly against layer 2.

**The hard part — solve it while building, not before**. Associating a rendered
line with its origin across the optimizer. The constraint set is not stable:
`AddConstraint` appends (ABS envelopes, Big-M rows, McCormick) and
`RewriteComposedMinMax` removes constraints from the tree entirely into
`composed_minmax_constraints`. Layer 3 is therefore not a line-for-line image of
layer 2, and positional association will drift. It needs a stable per-constraint
identity, or a rendering model that tolerates one-to-many and one-to-none.

`source_clause_id` now exists and is stable through physical extraction,
`SolverInput`, `SolverModel` and elastic diagnostics — it was added for
diagnostics after this was filed, and is very likely the identity this needs. It
reaches the renderer as a tag on the expression's alias, which `RenderDecideSource`
ignores rather than prints; reading it back as an identity rather than discarding
it is what this item needs. `LogicalDecide::constraint_sources` is already indexed
by it and already holds a rendered `canonical_lhs` / `canonical_rhs` / `qualifier`
per user clause, so the layer-2 line may need no new rendering at all.

**Scope notes**

- Applies to the Objective row too — it shares the renderer.
- Layers 2 and 3 are noise when neither canonicalization nor the optimizer touched
  a constraint. Collapse to one line in that case.
- `EXPLAIN (FORMAT JSON)` should carry the layers as structured fields, not one
  pre-joined string.
- The physical node calls the same walker, so there is no second implementation to
  update — that scope note from the original filing is obsolete.
- `test/decide/tests/test_explain.py` asserts on the current single-form output and
  will need updating.

**Filed**: 2026-08-10, while designing the canonicalization refactor. Its stated
prerequisite — that canonicalization happen in one place, so that "the canonical
form" is a well-defined thing to render — is now met.
