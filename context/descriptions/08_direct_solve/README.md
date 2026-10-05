# Direct Solve

Direct solve is a DECIDE optimizer feature that is on by default. When a problem matches its
first rule exactly, it builds an optimal assignment from ordinary DuckDB operators
(windows, filters, projections) instead of building and running a solver model. A
problem it cannot prove keeps the existing solver path.

The first rule, S1, covers one row-scoped Boolean variable under count bounds on
`SUM(x)`: upper, lower, equal, or several combined, applied globally or per `PER`
group and optionally limited by `WHEN`. A bound can be a constant or a numeric source
column or source-only expression that varies by row. Exact zero/one pins on single
rows are allowed. The objective is a signed sum of per-row coefficients times `x`.
The [first-rule contract](02_first_rule/spec.md) lists the exact forms. A semantic
fact adapter and a rule coordinator keep recognition, proof, cost, explanation,
relational construction, and output mapping separate. A second rule remains open (the
[benefit report](04_performance/s1_benefit_report.md) holds the performance evidence behind the
`auto` default); the current built-in optimizer audit is recorded in
[optimizer_audit.md](00_design/optimizer_audit.md).

## Architecture in one minute

```text
bound, canonical LogicalDecide
  -> mode/policy -> exact/unknown facts -> rule Match and Prove
                      | miss: unchanged solver path (or require error)
                      v
                proved rule selection -> relational proposal
                      v
      scoped active check -> score guard -> free-row rank/count -> 0/1 assignment
                     v
             binding-preserving result boundary
                     v
             ordinary DuckDB physical operators
```

The adapter inspects the complete bound DECIDE trees. Its first rule produces
a typed S1 proof before building a relational assignment. A coordinator calls
Match, Prove, Cost, Explain, and Rewrite; its shared Map step verifies the
output-slot contract. A second class is still needed to demonstrate that these
interfaces are reusable. Estimates cannot justify eligibility. There is no
generated SQL, S1-specific physical operator, or retry through a solver after
execution begins.

The implemented *logical-only* result boundary has an explicit output-slot
map. It keeps DECIDE's external bindings and schema while its child is an
ordinary relational plan. Its unused-column hook prunes only source outputs
proved safe to skip: stored table columns, constants, and passthrough aliases
through projections, filters, or inner comparison joins.
Potentially observable computed outputs stay live. A hidden rank keeps
validation live when `x` is unused or the upper bound is zero. The
[optimizer audit](00_design/optimizer_audit.md) covers the current built-in
passes; [performance measurements](04_performance/s1_api_phase.md#window-sort-storage-and-joined-source-pruning)
record the wide-row and joined-source gains, rank sort-buffer size, and peak
memory uncertainty.

The current S1 proof accepts source-column `PER` keys and a deterministic,
source-only top-level `WHEN` or aggregate-local `WHEN` on the count.
Paired bounds require identical membership.
Numeric source-valued count bounds can vary by row; each is converted to
DOUBLE before its group limit is reduced, with every NULL/NaN checked.
Rows with NULL `PER` keys or a false aggregate `WHEN` bypass that bound but may
still obey per-row pins; score validation covers every row. If no row is eligible
for a scoped aggregate on a nonempty source, the direct plan raises DECIDE's
empty-aggregate error. The [grouped five-million-row
measurements](04_performance/s1_api_phase.md#grouped-cardinality-intervals)
test this partitioned plan at scale.

The first vertical slice is deliberately narrow and falls back on everything
not proved. The class mathematics and other problem classes live **only** in
the Word catalogue. The setting `decide_direct_solve` defaults to `auto`: it
tries the rule and falls back to the solver on a miss. `off` always uses the
solver; `require` reports why a query was not proved. This example uses
`require` so a miss fails loudly:

```sql
SET decide_direct_solve = 'require';
EXPLAIN SELECT id, x FROM (
  FROM (VALUES (1, 2.0), (2, -1.0), (3, 5.0)) t(id, score)
  DECIDE x(BOOL)
  SUCH THAT SUM(x) <= 1
  MAXIMIZE SUM(score * x)
) q;
```

The plan ranks all three scores, assigns `x=1` to id 3, and returns all three
rows. A miss in `require` is an error before solver work. Under the default
`auto`, a hit shows its decision record in optimized logical and physical
`EXPLAIN`, and a miss leaves `EXPLAIN` exactly as the solver path prints it.

## Source layout

Code is in `src/optimizer/decide/direct/`; headers are in
`src/include/duckdb/optimizer/decide/direct/`.

| File | Role |
| --- | --- |
| `direct_problem.cpp` | Reads the semantic facts (`direct_rule.hpp`) from the bound, canonical DECIDE tree; the only file that knows its layout. Also the queries on facts (`FirstUnknownReason`, `DirectConstraintFact::PlainSum`) |
| `direct_registry.cpp` | The rule list, `RegisteredDirectRules()` |
| `direct_coordinator.cpp` | Setting and mode, Match/Prove/Cost/Explain/Rewrite, cost context, output bindings and prunability, decision record, fallback and `require` errors |
| `direct_result_boundary.cpp` | Logical result boundary, output-slot map checks (`MapDirectResult`), serialization, unused-output hook |
| `direct_expression.cpp` | Rule-independent questions about a bound expression (decision-free, source-only, may throw, foldable), which coefficients and data-valued bounds a rule may admit (`DirectIsNumericDecisionFree`, `DirectSourceNumericColumn`, `DirectIsSourceOnlyNumeric`), and the lower/upper bound classification |
| `direct_builder.cpp` | Rule-independent plan builders (windows, counts, error predicates, the score check, the validation barrier), the DECIDE semantics every aggregate rule shares (`PER`/`WHEN` eligibility with NULL-key bypass, the empty-aggregate error, data-valued bound validation in source-clause order), and the source-output pruning proof |
| `s1_rule.cpp` | Everything specific to S1: `Match`, `Prove` in four steps (scope, pins, bounds, objective), explanation, and `Rewrite` in six stages, the first three through the shared builders |

The term splitter both the facts and the solver path use is `src/planner/decide/decide_term_split.cpp`.

## Adding a rule

A new problem class is its own rule file and touches no harness code. Check each item:

1. **Rule file.** `src/optimizer/decide/direct/<name>_rule.cpp`, with every helper inside
   `namespace direct_<name>` (unity builds merge anonymous namespaces across rule
   files), a `Make<Name>Rule()` declared in `<name>_rule.hpp`, and the file listed in
   that directory's `CMakeLists.txt`.
2. **Registry.** One line in `RegisteredDirectRules()` (`direct_registry.cpp`). Rules
   are tried in that order; the cheapest proved rule wins and a tie goes to the first.
3. **Facts.** Read only `DirectProblemFacts` (`direct_rule.hpp`), never the bound
   tree. Decline any unknown fact; `FirstUnknownReason()` names the clause.
4. **Proof.** `Match` binds the shape; `Prove` certifies every constraint, objective
   part, decision, runtime value obligation and output. Ask whether a coefficient or
   data-valued bound is admissible with the shared predicates in
   `direct_expression.hpp`, so every rule admits the same values. Estimates appear
   only in `Cost(proof, context)`.
5. **Builders.** Aggregate scope semantics come from `DirectProjectScope`,
   `DirectGuardEmptyAggregate` and `DirectValidateBounds`; a plan that could stream
   rows uses `DirectValidationBarrier`. The proposal states its output slots, whether
   each decision output may be skipped, and its validation slots.
6. **Obligations.** Reproduce the [solver input contract](00_design/solver_input_contract.md):
   the all-rows read, error classes and order, the DOUBLE domain, result types.
7. **Tests.** A `RuleFixture` in `test/decide/tests/_direct_rule_fixtures.py` runs the
   contract suite (`test_direct_rule_contract.py`); a generator for
   `compare_direct_with_solver` (`_direct_differential.py`) runs the seeded differential
   comparison; the rule's own mathematics gets its own test file with oracle checks on
   both solvers.
8. **Docs.** The class's definition and proof come from the Word catalogue, which
   stays unchanged; add a spec beside
   [the S1 contract](02_first_rule/spec.md), this page's source layout, and any new
   policy in [decisions](00_design/decisions.md).

## Where to read and work

- [00_design/](00_design/): architecture, first-build decisions, source facts,
  experiments, and implementation checks. Read [architecture](00_design/architecture.md),
  [decisions](00_design/decisions.md), the
  [solver input contract](00_design/solver_input_contract.md), [experiments](00_design/experiments.md),
  [todo](00_design/todo.md), and [done](00_design/done.md).
- [01_harness/](01_harness/): adapter, facts, rule interface, coordinator,
  boundary, policy, and explanation. [Todo](01_harness/todo.md) ·
  [done](01_harness/done.md).
- [02_first_rule/](02_first_rule/): narrow S1 admission, proof, and assignment.
  Read the [contract](02_first_rule/spec.md), [todo](02_first_rule/todo.md), and
  [done](02_first_rule/done.md).
- [03_correctness/](03_correctness/): [baseline corpus](03_correctness/baseline.md),
  differential/oracle tests, and edge outcomes. [Todo](03_correctness/todo.md) ·
  [done](03_correctness/done.md).
- [04_performance/](04_performance/): end-to-end measurement and cost evidence.
  [Large-scale S1 measurements](04_performance/s1_large_scale.md) ·
  [API phase measurements](04_performance/s1_api_phase.md) ·
  [todo](04_performance/todo.md) · [done](04_performance/done.md) ·
  [raw runs](04_performance/raw/).
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
source facts + baseline + design decisions
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

The [correctness baseline](03_correctness/baseline.md) records pre-feature
solver behavior; permanent tests then grow alongside the harness and rule.
The second rule tests reuse of the harness. ANR integration waits for the
language branch to stabilize and must pass the same semantic adapter
contract. Do not use a performance estimate as a substitute for a correctness proof.

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
