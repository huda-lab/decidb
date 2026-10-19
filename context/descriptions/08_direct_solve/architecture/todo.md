# Architecture — report: standard patterns and names

This goal is finished. Direct solve's design already followed known patterns, but under names we had made up. Every
concept now carries the standard name from database, compiler or optimization practice, and the code, tests, docs,
`EXPLAIN` output and website use it. `glossary.md` lists each name, the standard idea behind it, where it comes from,
and the old words it replaced; its last section lists the few places an old word had to stay and why.

## What changed

- **One word per idea.** "Facts" became the IR (intermediate representation), as compilers call the neutral form later
  passes read. The rule steps Match, Prove and Rewrite became one `CheckCondition` that returns the applicability
  condition and one `Apply` that builds the substitute plan, the words Volcano and Cascades optimizers use for
  transformation rules. The coordinator became the rule driver, the override a test seam, the proposal the substitute,
  "hit and miss" became "applies and does not apply".
- **The plan.** The validation barrier became a pipeline breaker, the second read became late materialization, and the
  result boundary became the result adapter, all standard query-engine terms. The shared helpers that return plan pieces
  are plan factory functions (they were called builders, which means something else in the design-patterns book), and
  the small helper that builds expressions is an `ExpressionBuilder`. The bundle of `EXPLAIN` lines a direct plan
  carries is the `EXPLAIN` annotation, not a "decision record".
- **The problem classes.** Each class keeps its id and adds the textbook name of its problem: S1 is uniform-matroid
  optimization and A1 is a separable box-constrained problem, and likewise for the other 17. Folders, rule files,
  namespaces, test files and the `EXPLAIN` value follow (`S1_UNIFORM_MATROID`, `A1_SEPARABLE_BOX`).
- **Outside direct solve.** An "absorbed bound" became a singleton-row bound folded into the variable's box, the
  presolve term.
- **Tests.** The S1 and A1 suites are differential oracle tests (`test_direct_s1_uniform_matroid.py`,
  `test_direct_a1_separable_box.py`), and the suite every rule must pass is the conformance suite
  (`test_direct_rule_conformance.py`). Their oracle answers were solved again once under the new names, with identical
  results.
- **Docs.** The shared-engine notes moved from `rules/_harness/` to `framework/`.

## What a SQL user notices

Only wording. `EXPLAIN` shows the new class names and a `Direct solve condition` line where it said `Direct solve
proof`; `require` reasons and error messages say "fixed value" where they said "pin"; the `decide_direct_solve`
setting description says "a rule that applies". Answers, plans and speed are unchanged. The
setting's name, its values, and every serialized name stayed, so saved plans and database files still read back.

## Decisions the owner made

P-01 keep "direct solve" as the feature's name; P-02 the shared-engine folder is `framework/`; P-03 adopt every standard
name the review found; P-04 one condition step and one apply step per rule, with a fixed `Summary()` line for
`EXPLAIN`; P-05 keep the `Direct` prefix in `namespace duckdb` and drop it inside a rule's own namespace; P-16 rename
the test files once, with one oracle re-run; P-20 the textbook class names; P-22 user-visible text follows every rename
except the setting and serialized names.
