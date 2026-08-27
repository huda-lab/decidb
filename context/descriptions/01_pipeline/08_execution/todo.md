# Stage 08 — Execution: open work

---

## `physical_decide.cpp` is ~3,800 lines and `Finalize` still holds several concerns

**Pointers**: `src/execution/operator/decide/physical_decide.cpp`.

At about 3,800 lines, `Finalize` remains the bulk of the file: three phases plus the ABS
`MAXIMIZE` upper-bound derivation and the auto-`M` refill.

**Decision needed before starting**: whether the remaining split is by phase or by
mechanism. Everything left reads `DecideGlobalSinkState`, so the split has to
decide what that state's interface is rather than just moving code.

**Test**: all golden models byte-identical; `make decide-test` unchanged. This
is a pure refactor, so a non-identical dump is a failure, not a finding.

**Done file**: `done.md` §8 — repoint the source map at the new files.
