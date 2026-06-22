# Query Diagnostics — Foundations (remaining)

Shared infrastructure consumed by all diagnosis states. The structured result,
constraint/variable provenance, the pragma gate, and the reporting relation have
shipped (`done.md`). What remains:

## F3 · Relaxability tagging

**Goal.** Distinguish *user* rows (choices — relaxable) from *structural* rows
(definitions — rigid). Slackening a McCormick / Big-M row redefines the math and
solves a different problem, so the elastic engine (infeasible) must slacken only
user rows. The `kind` field and the unambiguous USER / global-STRUCTURAL split
already exist (`done.md` · constraint provenance); F3 is the **exhaustive**
STRUCTURAL stamping across every linearization rewrite path.

**Build.**
- Stamp `STRUCTURAL` in each linearization expansion path. Stamped, not inferred —
  structural rows already emit in separate builder paths keyed on explicit tags.
- **Enumerate every structural kind** and confirm each maps to a distinct emission
  path: McCormick, Big-M MIN/MAX, `<>`, ABS, AVG scaling, entity-scoping links,
  composed-MIN/MAX pins. The rewrites live in `decide_optimizer.cpp`.
- **Variable bounds:** the only structural case is the McCormick-required finite
  upper bound — **widenable but not removable** (widening keeps it finite/safe;
  removal breaks the envelope). Gate the removal dial, not the widen dial.

**Open design call.** Within a clause each row is PARAMETER (carries the user's
editable `K`; slack lands here) or MECHANISM (linking `Σy≥1`, McCormick/ABS
definitions; rigid). Is this a second field, or a refinement of `kind`?

**Test.** Every structural rewrite stamps STRUCTURAL; the elastic program never
slackens a structural row; an all-structural conflict makes the elastic program
itself infeasible (the scope diagnostic, not a fake fix).

**Deps:** constraint provenance (satisfied).

## Slack → Δ conversion (build when the elastic engine needs it)

The slack→Δ conversion the elastic engine consumes (AVG `s*/N_g`, strict `</>`
re-quote against the typed `K`) has no live consumer yet — unbounded produces no
slacks. Build it here or in `infeasible/` when the elastic engine lands; render it
through the existing `decide_diagnostics()` relation.

## External dependency

**Decision-variable norms (v1.1)** — abs-aux / count-binary+Big-M / max-aux
linearizations reused by the elastic engine (`infeasible/` I3). Tracked in
`03_expressivity/sql_functions/todo.md`.
