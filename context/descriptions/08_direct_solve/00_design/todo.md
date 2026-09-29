# Design — open work

These gates close before feature C++ begins. Each needs a written mechanism,
invariant, and concrete test design; the [decisions](decisions.md) document
records the chosen direction. The implementation tests themselves run later
under [correctness](../03_correctness/todo.md). Branch creation can happen
earlier, but is not design approval.

- [ ] **BASE-01 — Record the prototype baseline.** Depends: documentation review.
  Commit the documentation checkpoint, create a separate branch from a recorded
  `master` SHA, and keep current DECIDE syntax fixed there. Do not merge the
  concurrent language branch into the first rule.
- [ ] **DES-01 — External output interface.** Depends: none. Specify how either
  the logical-only boundary or a scoped remap preserves old bindings, positional
  columns, and types. Audit the binding resolver, projection maps,
  nested/correlated plans, serialization, pruning, and physical lowering; write
  a test for each risk. Reject any design that could let a parent read the
  wrong column.
- [ ] **DES-02 — Mandatory value validation.** Depends: DES-01. Specify which
  input rows the current solver validates, the direct operator placement that
  must check them, and how to prevent skipped checks with unused decisions,
  zero capacity, or parent filters. Decide the error category and deterministic
  throwing-expression policy; list the tests that will verify the guarantee.
- [ ] **DES-03 — Legal optimizer movement.** Depends: DES-01, DES-02. Audit the
  remaining optimizer passes and list which transformations may cross the
  boundary. Specify enforcement for input-changing outer filters and
  guard-removing pruning while preserving safe inner optimization, plus tests.
- [ ] **DES-04 — Explanation surface.** Depends: DES-01. Decide how the one
  structured decision record appears in optimized logical and default physical
  `EXPLAIN`, profiling, and `require` errors; do not infer a rule from `WINDOW`.
- [ ] **DES-05 — Feature-setting contract.** Depends: none. Specify session
  setting validation, `off`/`auto`/`require` behavior, `DECIDB_FORCE_SOLVER` and
  `DIAGNOSE` precedence, and when prepared statements capture the mode.
- [ ] **DES-06 — Numeric contract.** Depends: none. Fix the capacity range,
  coefficient-to-DOUBLE conversion, zero comparison, NULL/non-finite errors,
  and objective comparison tolerance for tests.
- [ ] **DES-07 — Freeze the implementation contract.** Depends: DES-01 through
  DES-06 and [VAL-00](../03_correctness/todo.md). Update the architecture and
  rule contract with the resolved answers, then approve source implementation.

**Exit gate:** no open choice can change the harness/result boundary, admission
meaning, runtime validation, or evidence standard after implementation starts.
