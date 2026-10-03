# Direct Solve

Direct solve is an opt-in DECIDE optimizer prototype. When a problem matches its
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
relational construction, and output mapping separate. A second rule and the final
performance gate remain open; the current built-in optimizer audit is recorded in
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
the Word catalogue. The setting `decide_direct_solve` is `off` by default;
`auto` tries the rule and falls back; `require` reports why it missed. For a
small worked query, use:

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
rows. A miss in `require` is an error before solver work; use `auto` to retain
solver fallback. Both optimized logical and physical `EXPLAIN` display the
decision record.

## Source layout

Code is in `src/optimizer/decide/direct/`; headers are in
`src/include/duckdb/optimizer/decide/direct/`.

| File | Role |
| --- | --- |
| `direct_problem.cpp` | Reads exact facts from the bound DECIDE tree |
| `direct_coordinator.cpp` | Setting and mode, rule registry, Match/Prove/Cost/Explain/Rewrite, decision record, fallback and `require` errors |
| `direct_result_boundary.cpp` | Logical result boundary, output-slot map checks (`MapDirectResult`), serialization, unused-output hook |
| `direct_builder.cpp` | Rule-independent expression and window builders; the source-output pruning proof |
| `s1_rule.cpp` | Everything specific to S1: proof, `Prove`, explanation, and `Rewrite` in six stages |

A second rule adds its own file and one line in `RegisteredDirectRules()`; the other
files do not change.

## Where to read and work

- [00_design/](00_design/): architecture, first-build decisions, source facts,
  experiments, and implementation checks. Read [architecture](00_design/architecture.md),
  [decisions](00_design/decisions.md), [experiments](00_design/experiments.md),
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
