# Overview

Direct solve answers some DECIDE problems **without a solver**. When a problem has a shape the optimizer can
prove it understands, the DECIDE node is replaced by ordinary DuckDB operators (windows, filters, projections)
that compute the optimal assignment. Any other problem takes the existing solver path, unchanged.

Example: `SUM(x) <= 1 MAXIMIZE SUM(score * x)` is "pick the best row". The plan ranks rows by score and sets
`x = 1` where the rank is within the limit and the score helps. No model is built.

## The pipeline

```text
bound, canonical LogicalDecide
   -> IR             the front end reads the problem into one form    (ir.md)
   -> CheckCondition each rule checks its applicability condition     (rules.md)
        no rule applies: the solver path runs as before
        two rules apply: internal error, the rules must be disjoint
   -> Apply          build the substitute, a relational plan
   -> result adapter   keep DECIDE's output columns and bindings      (result_adapter.md)
   -> ordinary DuckDB optimizer and execution
```

## Where it hooks in

`DecideOptimizer::Optimize` (`src/optimizer/decide/decide_optimizer.cpp`) calls `ApplyDirectSolveRules` before
`OptimizeDecide` marks the node solver-specific. If it returns a replacement, the DECIDE node is gone. If it
returns the node unchanged, the solver path continues. There is no retry through a solver once a direct plan starts
executing, so a rule's condition has to be complete before it applies.

## Modes

`SET decide_direct_solve = 'auto' | 'off' | 'require'`. `auto` is the default: try direct, fall back silently. `off`
always uses the solver. `require` raises one fixed error when no rule applies, without saying which clause kept a rule out. `DIAGNOSE`
queries and the test-only `DECIDB_FORCE_SOLVER` always use the solver. Details: `policy.md`.

## Principles

- **Prove, don't guess.** Estimates and sampled data never decide eligibility.
- **Same answer, and an error on the same inputs.** Direct solve returns the answer the solver would, and raises an
  error whenever the solver does, with the solver's text. It need not pick the same error when several apply.
  Types and row counts match. Tied optima may differ. Details: `rules.md`.
- **Lean.** This layer is a light shortcut that detects some problems and solves them faster than a solver. It is
  not a second solver. It takes only shapes where the proof is short and the win is large, and everything else stays
  on the solver path. A feature, error format or check earns its place only if it protects the answer or the speedup.
  When matching the solver is hard, or costs more than it protects, give it up and let the solver take the shape.
- **Rules are separate from the engine.** The shared engine knows nothing about any problem class. A new class is a
  new rule, registered in one place.

## Patterns used

Names follow [glossary.md](glossary.md).

- **Front end and IR** (compiler practice). The front end reads `LogicalDecide` once into an intermediate
  representation. Rules read only the IR, so no rule depends on the bound tree's layout.
- **Conservative analysis.** The IR records "not proved" for anything it cannot read exactly, and no rule applies on
  a "not proved" input.
- **Strategy.** Each rule implements one interface: check its applicability condition, then apply its substitute.
- **Registry and Factory Method.** One registry lists the rules; each rule has a factory function.
- **Template Method.** The rule driver runs the same steps for every rule: build the IR, offer it to each rule, accept
  at most one, apply it, wrap the substitute.
- **Transformation rules** (Volcano, Cascades). A rule replaces the DECIDE node with an equivalent relational plan.
- **Adapter with an optimization fence.** The result adapter presents the substitute under the DECIDE node's bindings
  and keeps parent filters and limits from crossing it.
- **Pipeline breaker and late materialization** (DuckDB, column stores). Inputs the solver path would reject are
  validated before any row is emitted, and wide columns are fetched by row id after the plan has chosen its rows.

**Departure from Cascades: rules are disjoint, not chosen by cost.** Cascades lets several rules apply and keeps the
cheapest plan. Here, rules are told apart by the IR they require, and two rules applying to one query is an internal
error naming both: the overlap is a bug in the rules, and no ordering is safe to pick (`rules.md`).

## Code

`src/optimizer/decide/direct/`, headers in `src/include/duckdb/optimizer/decide/direct/`. Start with
`direct_rule_driver.cpp` (the flow above), then `s1_uniform_matroid_rule.cpp` (a rule built on a window) and
`a1_separable_box_rule.cpp` (a rule built on a chain of projections and the pipeline breaker). The rest of the folder is
named for what it does: IR, plan fragments, runtime checks, bound reader, result adapter, expression predicates,
registry.
