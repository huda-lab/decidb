# Stage 08 — Execution: open work

---

## `physical_decide.cpp` is ~7,400 lines and holds four distinct concerns

**Pointers**: `src/execution/operator/decide/physical_decide.cpp`. Its own section
markers already name the split: *Expression Transform Helpers* (45),
*Expression Analysis Helper Functions* (763), *Multi-variable per-row constraint
helpers* (1379), *Sink* (1429), the `DecideGlobalSinkState` class (1460-2660),
data-driven Big-M (2696), the slow-solve checkpoint report (3070), `Finalize`
(3214-7287) and *Source* (7287).

`Finalize` alone is ~4,000 lines and spans three phases plus composed MIN/MAX
emission, the deferred `<>` expansion, the ABS `MAXIMIZE` upper-bound derivation
and the auto-`M` refill.

**Decision needed before starting**: the natural seams are not the phase markers.
Term extraction (structural, no data) and coefficient evaluation (numeric, needs
the materialized chunk) are genuinely different jobs with different inputs, and
the mechanism-specific emitters (composed MIN/MAX, `<>`, ABS Big-M, auto-`M`) are
each self-contained. But every one of them reads `DecideGlobalSinkState`, so the
split has to decide what that state's interface is rather than just moving code.

**Test**: all 80 golden models byte-identical; `make decide-test` unchanged. This
is a pure refactor, so a non-identical dump is a failure, not a finding.

**Done file**: `done.md` §8 — repoint the source map at the new files.

---

## The `input_column_names` back-fill has two sources and a documented precedence

**Pointers**: `src/execution/physical_plan/plan_decide.cpp:131-165`.

Column names for the unbounded diagnosis come from two places: the child's
`GetColumnBindings()` (a bare source-column reference) and the child projection's
user-written names (harvested *before* `CreatePlan`, because `LogicalProjection`
moves its `expressions` vector away). The rule is that a name referenced in the
clause wins over a raw projection alias, and SELECT-only columns are back-filled
after.

This works, but it is the kind of two-source precedence that silently degrades:
a computed projection expression with no alias produces an empty name, and the
diagnosis then labels an escaping group by position.

**Decision needed**: whether an unnamed computed column should fall back to its
SQL text, to a positional label, or be excluded from `affected_rows` output
entirely. The current behavior is the second by accident rather than by choice.

**Test**: an unbounded query whose `PER` key is a computed expression with no
alias — check what `decide_diagnostics()` calls the escaping group.

**Done file**: `done.md` §7, and
[`../../07_query_diagnostics/unbounded/done.md`](../../07_query_diagnostics/unbounded/done.md).
