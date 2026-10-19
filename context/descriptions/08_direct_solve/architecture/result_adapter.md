# Result adapter: keeping DECIDE's output intact

The replacement plan must look exactly like the DECIDE node it replaces: every source column, then one column per
decision, with the same bindings and types. A small **logical-only** operator wraps the generated plan to guarantee
that. It has no problem-specific logic and lowers to ordinary projections, so there is no new physical operator.

It is two standard ideas in one operator. It is a GoF **Adapter**: the substitute presents the interface (bindings and
types) of the DECIDE node it replaces. It is also an **optimization fence**: parent filters and limits cannot cross it.
In `EXPLAIN` it shows as `DIRECT_RESULT_ADAPTER`; the code is `LogicalDirectResultAdapter`, built by
`AdaptDirectResult`.

## What it does

- **Explicit slot map.** Each DECIDE output maps to one slot of the generated child (`DirectSubstitute::slot_map`).
  Parents keep reading the old bindings.
- **Blocks parent work from crossing it.** A filter or limit above the DECIDE result cannot move into the rank input,
  since that would change which rows are ranked. Optimizing inside the child is fine.
- **Carries the `EXPLAIN` annotation**: the mode, rule and condition lines that `EXPLAIN` and profiling show when a
  rule applies (see `policy.md`).

## Why it is explicit

The adapter declares one dependency per output, plus the rule's validated columns, so the optimizer cannot prune the
child below what a parent still reads. A transparent wrapper does not work: the optimizer prunes the child to the
columns it can see used, and a parent that reads another column gets wrong data. Never substitute table indexes globally
or re-resolve bindings while lowering.

## Pruning unused columns

An unreferenced output is replaced by a typed NULL placeholder only when skipping it is proved safe: stored columns,
constants, and passthrough aliases through projections, filters, or inner comparison joins. A computed column that could
raise an error stays live, because the solver would have raised it. This is what lets a wide unused payload leave the
rank sort. The validated columns stay live even when no parent reads any output.

## Pipeline breaker

Runtime checks (NULL or non-finite scores, bad bounds) must run over every input row whenever the DECIDE result is
consumed, even if `x` is unused or the capacity is zero. A standalone filter is not enough: it can skip a late bad row
under `LIMIT 1`. S1's rank window reads every row before it emits one, so it is already a pipeline breaker. A rule whose
plan is pipelined (each row flows straight through) adds one with `DirectValidationBreaker`: a projected truth-or-error
column plus a whole-input window. That holds the rows in memory, so memory grows with the input.

### Memory of holding rows

Peak memory of one query in a fresh `decidb -readonly` process with `decide_direct_solve='require'`, on a stored
database with a narrow table `src` (id, t, cap, score, f) and `src_wide` (the same plus `repeat('p', 512) AS payload`).
The shapes are those of `benchmark/decide/profile_direct_a1.py`: A1 is `x(REAL) ... BETWEEN 0 AND cap MINIMIZE
SUM(POWER(x - t, 2))`; S1 is `x(BOOL) ... SUM(x) <= rows/10 MAXIMIZE SUM(score * x)`. Peak is the macOS memory
footprint from `/usr/bin/time -l` (maximum resident size reads lower once the system compresses pages). The payload is
488 MiB per 1,000,000 rows.

| Peak footprint, MiB | Rows | No DECIDE | Payload unused (control) | Payload held in the breaker | Payload late-materialized |
|---|---:|---:|---:|---:|---:|
| A1 | 1,000,000 | 7 | 32 | 1,049 | 73 |
| A1 | 5,000,000 | 14 | 131 | 5,212 | 353 |
| S1 | 1,000,000 | 6 | 99 | 651 | 145 to 149 |
| S1 | 5,000,000 | 14 | 725 | 3,351 to 3,362 | 811 to 829 |

"Payload unused" reads `COUNT(*), SUM(x)`, so the payload is pruned. "Held" and "late-materialized" read
`SUM(length(payload))` as well; held means the payload travels through the breaker or the rank, late-materialized means
it is read by a second scan (next section). "No DECIDE" is `SELECT COUNT(*), SUM(length(payload)) FROM src_wide`.

- Held rows cost memory in proportion to the input. Over the control, A1 adds about two payload copies per row and S1
  about one (the rank's sort buffer is the likely reason, not isolated).
- Late materialization stays near the control: 4.0 to 14.8 times less memory than holding at 5,000,000 rows.
- The wide column is the cause. Without it nothing grows, and a plain scan of the same payload is pipelined in 6 to
  14 MiB.
- The solver path holds more than direct solve on the same query (A1 at 1,000,000 rows: 1,655 MiB against 1,049; S1:
  1,012 MiB against 652).

Only peak memory is measured here; wall times were taken on a busy machine and are not recorded.

### Late materialization of a stored table

Late materialization is the column-store technique of reading the narrow columns first and fetching the wide ones by
row id after the plan has chosen its rows. DuckDB's own `LATE_MATERIALIZATION` pass uses it for Top-N; direct solve
has a separate step, `ApplyDirectLateMaterialization`, for the reason at the end of this section.

When the source is one DuckDB base table under non-volatile projections and filters, the plan carries the table's
`rowid` as a hidden column. After unused-column removal, when it is known what the parent reads, each parent-read output
that is a plain stored column the rule's plan only forwards comes from a second scan of the table, joined (inner, on
`rowid`) under the adapter. That column never enters the breaker or the rank, and the first read still checks every row,
so a bad last row under `LIMIT 1` still raises.

- **Only variable-width columns move** (`VARCHAR`, `BLOB`, `BIT`, lists, arrays, structs, maps, unions: a physical type
  that is not constant-size). Holding those is what costs memory. A fixed-width column stays in the single read, and an
  adapter with nothing to move plans one scan. For a fixed-width `BIGINT` key the choice is close: A1 took 0.12 s moved
  against 0.13 s held at 1,000,000 rows and 0.44 against 0.52 s at 5,000,000; S1's global top-K at 5,000,000 rows took
  0.28 to 0.33 s moved against 0.21 to 0.25 s held, with the same memory. Staying in the single read gives up A1's small
  gain to avoid S1's 30% loss, and needs no tuning number.
- **The single read is kept** for a join, an aggregate, a volatile or computed column, or a source that already reads
  `rowid` (no stable row key, or reading twice could change the rows), and for a pipelined plan (A1's constant problem),
  which holds nothing.
- **Where it runs.** Inside the `UNUSED_COLUMNS` pass, right after removal, not beside DuckDB's `LATE_MATERIALIZATION`.
  The passes in between add projection maps that unused-column removal does not keep up to date, so pruning again after
  them is unsafe. With that pass disabled the row id is dropped and the plan holds rows.
- **Other sources hold their rows**, wide columns included: 5.2 GiB at 5,000,000 rows for A1 with a 512-byte payload.

## Result types

A declared `BOOL` decision has a 0/1 domain but returns SQL `INTEGER`, as on the solver path.

Details: `direct_result_adapter.cpp` (map checks, serialization, pruning hook), `direct_late_materialization.cpp` (late
materialization) and `direct_plan_factory.cpp` (breaker and pruning proof). The optimizer passes that run after DECIDE
are covered for parent filter, join, aggregate, nested, and serializer cases by the parent-context rows in
`test_direct_s1_uniform_matroid.py` and the pruning tests in `test_direct_user_facing.py`.
