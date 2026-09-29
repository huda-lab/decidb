# Direct Solve

Direct solve is a proposed DECIDE optimizer path. For a problem whose complete
semantics match a proved rule, it constructs an optimal assignment with ordinary
DuckDB relational operators instead of building and running a solver model. A
miss keeps the existing solver path. **No direct rewrite is implemented yet.**

## Architecture in one minute

```text
bound, canonical LogicalDecide
  -> semantic adapter -> exact facts -> Match -> Prove
                                        | miss: unchanged solver path
                                        v
                     select rule -> build and check complete plan
                                        v
                     relational operators -> DuckDB executor
```

The shared harness owns policy, facts, rule registration, fallback, the external
output contract, and explanation. A rule owns its shape proof and relational
assignment. Matching and proof are separate. Estimates may rank already-proved
plans, never justify eligibility. There is no generated SQL, S1-specific physical
operator, or retry through a solver after execution begins.

The current design direction is a thin *logical-only* result boundary: it keeps
DECIDE's external bindings and result schema while its child is an ordinary
relational plan. It must block transformations that change the decision input,
but still permit safe optimization inside the plan and safe column pruning. This
is a design direction, not a verified implementation; the binding, validation,
and optimizer invariants are explicit open gates in [design/todo.md](00_design/todo.md).

The first vertical slice is the global upper-cardinality part of S1. It is
deliberately narrow and falls back on everything not proved. The class
mathematics and other problem classes live **only** in the Word catalogue.

## Where to read and work

- [00_design/](00_design/): architecture, agreed direction, source facts, and
  pre-code gates. Read [architecture](00_design/architecture.md),
  [decisions](00_design/decisions.md), [todo](00_design/todo.md), and
  [done](00_design/done.md).
- [01_harness/](01_harness/): adapter, facts, rule interface, coordinator,
  boundary, policy, and explanation. [Todo](01_harness/todo.md) ·
  [done](01_harness/done.md).
- [02_first_rule/](02_first_rule/): narrow S1 admission, proof, and assignment.
  Read the [contract](02_first_rule/spec.md), [todo](02_first_rule/todo.md), and
  [done](02_first_rule/done.md).
- [03_correctness/](03_correctness/): baseline corpus, differential/oracle tests,
  and edge outcomes. [Todo](03_correctness/todo.md) ·
  [done](03_correctness/done.md).
- [04_performance/](04_performance/): end-to-end measurement and cost evidence.
  [Todo](04_performance/todo.md) · [done](04_performance/done.md).
- [05_follow_on/](05_follow_on/): second-rule reuse, wider S1, and ANR adapter.
  [Todo](05_follow_on/todo.md) · [done](05_follow_on/done.md).
- [Problem-class catalogue](decidb_direct_relational_rewrites.docx): the sole
  source for class definitions, constructions, examples, and proofs. Leave the
  Word document unchanged.

Each work area has a `todo.md` for open work and a `done.md` for verified results.
The design reference and first-rule contract are separate from both: a proposal
is not marked done merely because it is written down.

## Dependency order

```text
source facts + current-syntax baseline
               |
       design gates (DES)
               |
       shared harness (HAR)
               |
       first S1 rule (RULE)
               |
       correctness gate (VAL)
         /-----+------\
         |            |
 performance (PERF)  second rule / wider coverage (NEXT)

stable language branch + HAR + VAL -> ANR adapter (NEXT)
```

The [correctness baseline](03_correctness/todo.md) begins before feature code;
correctness tests then grow alongside the harness and rule. The second rule
tests reuse of the harness. ANR integration waits for the language branch to
stabilize and must pass the same semantic adapter contract. Do not use a
performance estimate as a substitute for a correctness proof.

## Documentation rules

- The [syntax reference](../00_project_overview/syntax_reference.md) owns current
  DECIDE syntax. The Word catalogue owns all problem-class material. Markdown
  here describes implementation scope, architecture, tests, and evidence only.
- `todo.md` entries have stable IDs and dependencies. Move a shipped result to
  the owning `done.md` in present tense; remove its TODO rather than checking it
  off permanently.
- Code and tests are the source of truth for implemented behavior. Mark a design
  claim as proposed until its invariant has been checked. Do not infer ANR
  compatibility from the current bound-tree adapter.
- Prototype on a separate branch from a recorded `master` commit. Do not change
  DECIDE syntax on that branch; the language merge happens through the adapter.
