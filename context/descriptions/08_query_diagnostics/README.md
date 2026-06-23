# Query Diagnostics

Turning failed or useless DECIDE solves into actionable diagnoses. SQL always
returns rows; a DECIDE *solve* can fail in ways a lookup can't — and today each
failure is a dead end (a static error paragraph, a timeout, or a silently
arbitrary answer). This area replaces each dead end with a **diagnosis**: *why*
it failed and the **least-change** edit that restores a usable solution.

Phase tags (`v1.2`, `v2.1`, …) trace back to the research build plan; they encode
build order, not version numbers.

## The map — everything flows through the router

Every solve outcome flows through one dispatch tree (the **router**): it inspects the
solver status — plus a couple of sub-signals (is there a recession ray? an
incumbent?) — and routes to exactly one terminal. The states below are its leaves.

```
                              solve result
                                   │
        ┌──────────────────────────┼──────────────────────────┐
     solved                      failed                    time_limit
        │                          │                            │
  (success,         ┌──────────────┼──────────────┐       ┌─────┴─────┐
 no diagnosis)   unbounded     infeasible       inf/unb  incumbent   no sol
                    │              │               │         │          │
                 find ray       elastic        check ray   report     report
                    │              │               │      incum+gap    slow
                  report         report      ┌──────┴──────┐
                                          found        not found
                                            │              │
                                   "add a bound —       elastic
                                    may still be          │
                                    infeasible"         report
```

- **[`router/`](router/)** — the **spine**: the dispatch tree above and the inf/unb
  `check ray` disambiguation. Lands incrementally as the terminals below ship.
- **[`unbounded/`](unbounded/)** — terminal `failed → unbounded`: name the escaping
  variable via the ray and prescribe a bound (**tighten** a too-open region). Shipped.
- **[`infeasible/`](infeasible/)** ★ — terminal `failed → infeasible`: elastic
  relaxation (**loosen** a too-small region). The flagship engine; not built yet.
- **[`slow/`](slow/)** — terminal `time_limit`: read incumbent / bound / gap and
  report. Least-settled; revisit after infeasible + unbounded.
- **[`foundations/`](foundations/)** — the **substrate** every terminal sits on (not
  a terminal): structured solver result, constraint + variable provenance, the
  `diagnose_decide` gate, the solver-behavior reference, the reporting relation.
  **Build first — gates everything.**

Infeasible and unbounded are mirror images — loosen a too-small region vs. tighten a
too-open one; slow is a runtime event masking the other states.

## Principles

- **Least-change** — propose the smallest edit, not a rewrite.
- **On by default** — `PRAGMA diagnose_decide` is `auto` by default: a failed solve
  is diagnosed automatically wherever an engine exists. Set `off` to suppress and
  get the plain static error. We never edit the user's query; a diagnosis only ever
  *describes* the failure and prescribes a remedy.
- **Solver-agnostic** — everything works on Gurobi and HiGHS. We build the
  elastic model in our own model builder so both backends solve it natively;
  Gurobi `feasRelax` is an *accelerator*, never a dependency.
- **Differential testing** — every phase tests against `oracle_solver` on
  constructed cases, never hand-computed answers.

## Build order (dependency tiers)

```
Tier 1  shipped: F1 structured-result · F2 constraint-prov · F6 variable-prov
        F3 relaxability (needs F2) · F5 reporting-relation (needs F2) · F4 pragma (needs F1)
Tier 2  I1/I2 elastic · U2 ray (fallback-only)   (engines; need Tier 1)
        R1/R2 router seam + unbounded terminal (needs U2)
Tier 3  R3/R4 router inf/unb check-ray + remove facade probe (subsumes the old U1
        disambiguation) · I4 infeasible-reporting · U3 ray-reporting (full) · S1–S4 slow
        R5/R6 router infeasible + time_limit terminals (land with I4 / slow)
```

## Invocation — `PRAGMA diagnose_decide`

Sticky session pragma with two modes: `auto` (default — diagnose whichever failed
state the solve lands in, wherever an engine exists) and `off` (suppress diagnosis;
reproduce the plain static solver error). Diagnosis only ever runs when the solve
*actually* fails, so leaving `auto` on costs nothing on a successful solve. The
earlier per-state filter modes (`infeasible` / `unbounded` / `slow`) and the
opt-in `none` default were removed — `auto` subsumes them. See `foundations/done.md`
(F4).

## External dependency — decision-variable norms (v1.1)

User-writable L0/L1/L2/L∞ norms on decision variables. The elastic engine reuses
its abs-aux / count-binary+Big-M / max-aux linearization machinery. Implementation
lives in `03_expressivity/sql_functions/todo.md` (it is an expressivity feature,
not a diagnostic); tracked there, listed here only as an upstream dependency of
`infeasible/` (I3).
