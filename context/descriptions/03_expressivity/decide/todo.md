# DECIDE Clause — Planned Features

## Signed / free decision variables (variables that can go negative)

**Need.** Today every decision variable is non-negative: the ILM model builder
fixes bounds to `[0, 1e30]` for INTEGER/REAL and `[0, 1]` for BOOLEAN
(`ilp_model_builder.cpp`; see `done.md` · Code Pointers). There is no way to
declare a variable that may take a negative value, so problems whose natural
answer is signed — adjustments/deltas that can go either way, repairs that may
subtract, positions that can be short — cannot be expressed, and the user must
hand-shift by a constant offset.

**Scope to define (open).** What the surface should be — e.g. a free variable
(`-∞ … +∞`), an explicit lower bound (`DECIDE x >= -10`), or a signed type
marker — and how it interacts with the linearizations that currently *assume*
non-negativity (ABS, McCormick finite-bound requirement, Big-M, `norm`). These
all need re-checking; several rely on `[0, …]` today.

**Diagnostics interaction (why this is tracked now).** The unbounded diagnosis
reports an escape `direction` (`+∞` / `-∞`). Because variables are non-negative,
escape is always upward, so `direction` is always `+∞` and the `-∞` branch is
unreachable. Downward escape only becomes possible — and the `-∞` path only
becomes testable — once signed variables exist. See
`08_query_diagnostics/unbounded/todo.md` (direction / downward escape).
