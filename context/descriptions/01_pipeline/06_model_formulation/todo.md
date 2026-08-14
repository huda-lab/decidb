# Stage 06 — Model formulation: open work

---

## The committed golden baseline is older than the corpus it baselines

**Pointers**: `test/decide/golden/baseline.dump` (+ `.results`) against
`test/decide/golden/corpus.sql`.

The baseline was captured 2026-08-12; the corpus was last edited 2026-08-13 and
has uncommitted changes. Diffing a fresh capture therefore reports corpus edits as
if they were regressions.

**Why it matters for this stage specifically**: `capture.sh` dumps the built
`SolverModel`, which makes it *this stage's* characterization oracle. A change
claiming not to alter the model is verified by diffing dumps, and a baseline that
no longer corresponds to the corpus silently removes that guarantee. The natural
reaction to a noisy diff is to stop trusting it.

**Fix**: regenerate `baseline.dump` and `baseline.dump.results` in the same commit
that settles `corpus.sql`.

**Test**: `./test/decide/golden/capture.sh` against the committed baseline must
diff clean on an unmodified tree.

**Done file**: `done.md` §9 — the golden-corpus row can then be described as a
live oracle rather than a corpus.

*(Also listed in [`../../06_issues/code_quality/todo.md`](../../06_issues/code_quality/todo.md),
where it was first recorded. Remove from both when fixed.)*

---

## `VarIndexer::BuildRef()` has no production callers

**Pointers**: `src/include/duckdb/decidb/ilp_model.hpp:106-110`.

The non-owning constructor is retained "for tests and future use". The owning
`Build()` is used everywhere in production. Two constructors with one caller
between them is a small maintenance surface, but it is a real one: any change to
the entity-mapping lifetime has to be reasoned about twice.

**Decision needed**: delete it, or find the caller it was built for. The lifetime
contract it encodes (caller guarantees the `SolverInput` outlives the indexer) is
the harder of the two to get right, so keeping it unused is the worse of the two
outcomes.

**Test**: build + full suite; there is no behavior to pin.

**Done file**: `done.md` §1 — drop the second constructor bullet.
