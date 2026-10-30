# Result adapter: keeping DECIDE's output intact

The replacement plan must look exactly like the DECIDE node it replaces: every source column, then one column per
decision, with the same types. A small **logical-only** operator wraps the generated plan to guarantee
that. It has no problem-specific logic and lowers to ordinary projections, so there is no new physical operator.

It is three standard ideas in one operator. It is a GoF **Adapter**: the substitute presents the interface (columns and
types) of the DECIDE node it replaces. It is a **binding boundary**, as a DuckDB projection is: it owns a table index
and its outputs are `(index, i)`. It is also an **optimization fence**: parent filters and limits cannot cross it.
In `EXPLAIN` it shows as `DIRECT_RESULT_ADAPTER`; the code is `LogicalDirectResultAdapter`, built by
`AdaptDirectResult`.

## What it does

- **Explicit slot map.** Each DECIDE output maps to one slot of the generated child (`DirectSubstitute::slot_map`).
- **Owns its bindings.** When a rule applies, the rule driver gives the adapter a new table index and rewrites every
  reference above the DECIDE from DECIDE's bindings (its source's and `(decide_index, j)`) to the adapter's, once, with
  DuckDB's `ColumnBindingReplacer` run from the plan root and stopped at the adapter (`RebindAboveAdapter`). A DuckDB
  pass that later moves bindings below the adapter rewrites the adapter's own expressions and stops there, so parents
  never see it. Compressed materialization does this: it puts a decompressing projection with a new table index above
  an `ORDER BY`, a `DISTINCT` or a join in the source and rewrites the references above it. A copy of DECIDE's
  bindings kept in the adapter would go stale and leave the parent's references unbound.
- **Blocks parent work from crossing it.** A filter or limit above the DECIDE result cannot move into the rank input,
  since that would change which rows are ranked. Optimizing inside the child is fine.
- **Carries the `EXPLAIN` annotation**: the mode, rule and condition lines that `EXPLAIN` and profiling show when a
  rule applies (see `policy.md`).

## Why it is explicit

The adapter declares one dependency per output, plus the rule's validated columns, so the optimizer cannot prune the
child below what a parent still reads. A transparent wrapper does not work: the optimizer prunes the child to the
columns it can see used, and a parent that reads another column gets wrong data. The one rewrite above is limited to
DECIDE's own bindings, which no operator outside its ancestors reads; never substitute other table indexes or re-resolve
bindings while lowering.

## Pruning unused columns

An unreferenced output is replaced by a typed NULL placeholder only when skipping it is proved safe: stored columns,
constants, and passthrough aliases through projections, filters, or inner comparison joins. A computed column that could
raise an error stays live, because the solver would have raised it. This is what lets a wide unused payload leave the
rank sort. The validated columns stay live even when no parent reads any output.

The plan holds materialized CTEs: a copy of the source, a bound's side table, a gate's verdict. DuckDB's
`RemoveUnusedColumns` prunes a definition to the union of the columns its references read and narrows each reference to
that union at the end of the pass. This is projection pushdown across a CTE boundary, the column twin of DuckDB's
`CTEFilterPusher` and what Spark's `PushdownPredicatesAndPruneColumnsForCTEDef` does. DuckDB kept a definition whole,
because the pass does not follow a reference back to its definition. A definition with a reference the pass does not
reach (one read through a table in-out function) is still kept whole.

## Every row is read

Runtime checks (NULL or non-finite scores, bad bounds) must run over every input row whenever the DECIDE result is
consumed, even if `x` is unused or the capacity is zero. A standalone filter is not enough: it can skip a late bad row
under `LIMIT 1`. S1's rank window reads every row before it emits one, so it is already a pipeline breaker, and holds
the rows in memory, so memory grows with the input. A rule whose plan streams (each row flows straight through)
verifies, then streams, and holds nothing: one read of the source computes a one-row verdict over every row
(`DirectAllRowsValid`: `bool_and` of each row's check, true or raising), and the source reader gates every later read on
it (`DirectSourceReader::Gate`), so the streamed read passes no row before every row is checked. This is DuckDB's plan
for an uncorrelated scalar subquery (PostgreSQL's InitPlan). It costs a second evaluation of the rule's per-row stages
in place of memory that grows with the input. A streaming plan whose rows all see the same values needs no verdict: the
first row raises what every row would.

### Memory of holding rows

Peak memory of one query in a fresh `decidb -readonly` process with `decide_direct_solve='require'`, on a stored
database with a narrow table `src` (id, t, cap, score, f) and `src_wide` (the same plus `repeat('p', 512) AS payload`).
S1 is `x(BOOL) ... SUM(x) <= rows/10 MAXIMIZE SUM(score * x)`. Peak is the macOS memory
footprint from `/usr/bin/time -l` (maximum resident size reads lower once the system compresses pages). The payload is
488 MiB per 1,000,000 rows.

| Peak footprint, MiB | Rows | No DECIDE | Payload unused (control) | Payload held in the rank | Payload late-materialized |
|---|---:|---:|---:|---:|---:|
| S1 | 1,000,000 | 6 | 99 | 651 | 145 to 149 |
| S1 | 5,000,000 | 14 | 725 | 3,351 to 3,362 | 811 to 829 |

"Payload unused" reads `COUNT(*), SUM(x)`, so the payload is pruned. "Held" and "late-materialized" read
`SUM(length(payload))` as well; held means the payload travels through the rank, late-materialized means
it is read by a second scan (next section). "No DECIDE" is `SELECT COUNT(*), SUM(length(payload)) FROM src_wide`.

- Held rows cost memory in proportion to the input. Over the control, S1 adds about one payload copy per row (the
  rank's sort buffer is the likely reason, not isolated).
- Late materialization stays near the control: about 4 times less memory than holding at 5,000,000 rows.
- The wide column is the cause. Without it nothing grows, and a plain scan of the same payload is pipelined in 6 to
  14 MiB.
- The solver path holds more than direct solve on the same query (S1 at 1,000,000 rows: 1,012 MiB against 652).

Only peak memory is measured here; wall times were taken on a busy machine and are not recorded.

### Late materialization of a stored table

Late materialization is the column-store technique of reading the narrow columns first and fetching the wide ones by
row id after the plan has chosen its rows. DuckDB's own `LATE_MATERIALIZATION` pass uses it for Top-N; direct solve
has a separate step, `ApplyDirectLateMaterialization`, for the reason at the end of this section.

When the source is one DuckDB base table under non-volatile projections and filters, the plan carries the table's
`rowid` as a hidden column, passed along like any source column through the rule's windows, the side tables' joins, the
gates and a copy of the source. The second scan is the table the row id's binding leads back to, through projections and
CTE references (`FindScanOfRowId`), so no list of operators limits which plans qualify. After unused-column removal,
when it is known what the parent reads, each parent-read output that is a plain stored column the rule's plan only
forwards comes from a second scan of the table, joined (inner, on `rowid`) under the adapter. That column never enters
the rank, and the first read still checks every row, so a bad last row under `LIMIT 1` still raises.

- **Only variable-width columns move** (`VARCHAR`, `BLOB`, `BIT`, lists, arrays, structs, maps, unions: a physical type
  that is not constant-size). Holding those is what costs memory. A fixed-width column stays in the single read, and an
  adapter with nothing to move plans one scan. For a fixed-width `BIGINT` key the choice is close: A1 took 0.12 s moved
  against 0.13 s held at 1,000,000 rows and 0.44 against 0.52 s at 5,000,000; S1's global top-K at 5,000,000 rows took
  0.28 to 0.33 s moved against 0.21 to 0.25 s held, with the same memory. Staying in the single read gives up A1's small
  gain to avoid S1's 30% loss, and needs no tuning number.
- **The single read is kept** for a join, an aggregate, a volatile or computed column, or a source that already reads
  `rowid` (no stable row key, or reading twice could change the rows), and for a pipelined plan (A1), which holds
  nothing.
- **Where it runs.** Inside the `UNUSED_COLUMNS` pass, right after removal, not beside DuckDB's `LATE_MATERIALIZATION`.
  The passes in between add projection maps that unused-column removal does not keep up to date, so pruning again after
  them is unsafe. With that pass disabled the row id is dropped and the plan holds rows.
- **Other sources hold their rows** in a plan that holds rows, wide columns included.
- **A copy of the source leaves the moved columns out.** The copy is pruned to what its reads use, so its row width in
  the copy decision (`CopyWidth`) counts every column except the variable-width stored ones the node does not read. A
  pipelined plan over a copy holds those columns in the copy, which this rule does not see yet (PERF-22).

## Result types

A declared `BOOL` decision has a 0/1 domain but returns SQL `INTEGER`, as on the solver path.

Details: `direct_result_adapter.cpp` (map checks, serialization, pruning hook), `direct_late_materialization.cpp` (late
materialization), `direct_plan_fragments.cpp` and `direct_source_reader.cpp` (the verdict and the gate) and
`direct_rule_driver.cpp`
(pruning proof). The optimizer passes that run after DECIDE
are covered for parent filter, join, aggregate, nested, and serializer cases by the parent-context rows in
`test_direct_s1_uniform_matroid.py` and the pruning tests in `test_direct_user_facing.py`.
