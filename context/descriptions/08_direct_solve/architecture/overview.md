# Overview

Direct solve answers some DECIDE problems **without a solver**. When a problem has a shape the optimizer can
prove it understands, the DECIDE node is replaced by ordinary DuckDB operators (windows, filters, projections)
that compute the optimal assignment. Any other problem takes the existing solver path, unchanged.

Example: `SUM(x) <= 1 MAXIMIZE SUM(score * x)` is "pick the best row". The plan ranks rows by score and sets
`x = 1` where the rank is within the limit and the score helps. No model is built.

## The pipeline

```text
bound, canonical LogicalDecide
   -> facts          read the problem into a neutral summary          (facts.md)
   -> Match / Prove  each rule checks the shape and every detail      (rules.md)
        miss: the solver path runs as before
   -> Cost           pick the cheapest proved rule
   -> Rewrite        build the relational plan
   -> result boundary  keep DECIDE's output columns and bindings      (result_boundary.md)
   -> ordinary DuckDB optimizer and execution
```

## Where it hooks in

`DecideOptimizer::Optimize` (`src/optimizer/decide/decide_optimizer.cpp`) calls `TryDirectSolve` before
`OptimizeDecide` marks the node solver-specific. If it returns a replacement, the DECIDE node is gone. If it
returns the node unchanged, the solver path continues. There is no retry through a solver once a direct plan starts
executing, so a rule's proof has to be complete before it fires.

## Modes

`SET decide_direct_solve = 'auto' | 'off' | 'require'`. `auto` is the default: try direct, fall back silently.
`off` always uses the solver. `require` raises an error naming the clause that blocked a hit. `DIAGNOSE` queries
and the test-only `DECIDB_FORCE_SOLVER` always use the solver. Details: `policy.md`.

## Principles

- **Prove, don't guess.** Estimates and sampled data never decide eligibility. They only choose between rules that
  are already proved.
- **Same answer, same errors.** A user must not be able to tell from errors, types or row counts which path ran.
  Tied optima may differ. Details: `rules.md`.
- **Rules are separate from the engine.** The shared engine knows nothing about any problem class. A new class is a
  new rule, registered in one place.

## Code

`src/optimizer/decide/direct/`, headers in `src/include/duckdb/optimizer/decide/direct/`. Start with
`direct_coordinator.cpp` (the flow above) and `s1_rule.cpp` (the one rule so far). The rest of the folder is
named for what it does: facts, builders, result boundary, expression predicates, registry.
