# Query Diagnostics

Turning failed or useless DECIDE solves into actionable diagnoses. SQL always
returns rows; a DECIDE *solve* can fail in ways a lookup can't — and today each
failure is a dead end (a static error paragraph, a timeout, or a silently
arbitrary answer). This area replaces each dead end with a **diagnosis**: *why*
it failed and the **least-change** edit that restores a usable solution.

Phase tags (`v1.2`, `v2.1`, …) trace back to the research build plan; they encode
build order, not version numbers.

## The four states

| State        | Response                                            | Folder                |
| ------------ | --------------------------------------------------- | --------------------- |
| Solved       | return the result normally — no diagnosis needed    | —                     |
| Infeasible ★ | **loosen** — elastic relaxation                     | `infeasible/` (flagship) |
| Unbounded    | **tighten** — name the escaping variable via the ray | `unbounded/`          |
| Slow         | read incumbent / bound / gap and route              | `slow/`               |

Infeasible and unbounded are mirror images (loosen a too-small region vs. tighten
a too-open one); slow is a runtime event masking the other states.

## Principles

- **Least-change** — propose the smallest edit, not a rewrite.
- **Manual-first** — diagnosis is opt-in via `PRAGMA diagnose_decide`; never
  silently spend a second solve or edit the user's query. Automate only once the
  engine is proven.
- **Solver-agnostic** — everything works on Gurobi and HiGHS. We build the
  elastic model in our own model builder so both backends solve it natively;
  Gurobi `feasRelax` is an *accelerator*, never a dependency.
- **Differential testing** — every phase tests against `oracle_solver` on
  constructed cases, never hand-computed answers.

## Folders

- `foundations/` — shared plumbing every state consumes: structured solver
  result, constraint provenance (F2) + variable provenance (F6) + relaxability
  tagging, the invocation pragma, the diagnostic reporting relation. **Build
  first — gates everything.**
- `infeasible/` — the elastic relaxation engine, per-constraint-type slack
  treatment, and reporting.
- `unbounded/` — ray extraction, ray→SQL mapping, reporting.
- `slow/` — interrupt infra, read-and-route (least-settled; revisit after
  infeasible + unbounded ship).

## Build order (dependency tiers)

```
Tier 1  F1 structured-result · F2 constraint-prov · F6 variable-prov
        F3 relaxability (needs F2) · F5 reporting-relation (needs F2) · F4 pragma (needs F1)
Tier 2  I1/I2 elastic · U1 disambiguation · U2 ray (fallback-only)   (engines; need Tier 1)
Tier 3  I4 infeasible-reporting · U3 ray-reporting (full) · S1–S4 slow
```

## Invocation — `PRAGMA diagnose_decide`

Sticky session pragma, the manual-first consent gate. Modes: `none` (default —
fail fast as today), `infeasible` / `unbounded` / `slow` (scoped), `auto`
(whichever state the solve lands in). **Filter semantics, not force:** a mode
acts only when the solve *actually* lands in that state, so a left-on pragma is
harmless and `auto` doesn't violate manual-first (setting the pragma *is* the
opt-in). See `foundations/todo.md` (F4).

## External dependency — decision-variable norms (v1.1)

User-writable L0/L1/L2/L∞ norms on decision variables. The elastic engine reuses
its abs-aux / count-binary+Big-M / max-aux linearization machinery. Implementation
lives in `03_expressivity/sql_functions/todo.md` (it is an expressivity feature,
not a diagnostic); tracked there, listed here only as an upstream dependency of
`infeasible/` (I3).
