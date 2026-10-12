# Result boundary: keeping DECIDE's output intact

The replacement plan must look exactly like the DECIDE node it replaces: every source column, then one column per
decision, with the same bindings and types. A small **logical-only** operator wraps the generated plan to guarantee
that. It has no problem-specific logic and lowers to ordinary projections, so there is no new physical operator.

## What it does

- **Explicit slot map.** Each original output slot maps to one slot of the generated child. Parents keep reading the
  old bindings.
- **Blocks parent work from crossing it.** A filter or limit above the DECIDE result cannot move into the rank input,
  since that would change which rows are ranked. Optimizing inside the child is fine.
- **Carries the decision record** that `EXPLAIN` and profiling show on a hit (see `policy.md`).

## Why it is explicit

An early spike used a transparent wrapper. The optimizer then pruned the child to one column while a parent still read
column 2, and the query returned wrong data. Declaring one dependency per output, plus the rule's validation slots,
fixed it. Never substitute table indexes globally or re-resolve bindings while lowering.

## Pruning unused columns

An unreferenced output is replaced by a typed NULL placeholder only when skipping it is proved safe: stored columns,
constants, and passthrough aliases through projections, filters, or inner comparison joins. A computed column that could
raise an error stays live, because the solver would have raised it. This is what lets a wide unused payload leave the
rank sort. The validation slots stay live even when no parent reads any output.

## Validation barrier

Runtime checks (NULL or non-finite scores, bad bounds) must run over every input row whenever the DECIDE result is
consumed, even if `x` is unused or the capacity is zero. A standalone filter is not enough: it can
skip a late bad row under `LIMIT 1`. S1's rank window reads every row before it emits one. A rule whose plan could
stream uses `DirectValidationBarrier`: a projected truth-or-error column plus a whole-input window. That holds the rows
in memory. For a stored table, a wide column the parent reads is kept out of those rows by a second read (below); for
any other source the held rows include it. The section below records what holding costs.

### Memory of holding rows

**Setup** (measured 2026-10-09 with the binary at `de90502e4b`; "Read twice" 2026-10-13 at `7a480a1a71`). A stored database with a narrow table `src` (id, t, cap, score, f) and
`src_wide` (the same plus `repeat('p', 512) AS payload`), the same shapes as `benchmark/decide/profile_direct_a1.py`.
Each cell is a fresh `decidb -readonly` process with `decide_direct_solve='require'`, peak memory from `/usr/bin/time -l`,
one aggregate row read back. A1 is `x(REAL) ... BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))`; S1 is
`x(BOOL) ... SUM(x) <= rows/10 MAXIMIZE SUM(score * x)`. "Unused" reads `COUNT(*), SUM(x)` (payload pruned, the control).
"Held" adds `SUM(length(payload))`, measured before the second read existed, so the rows are held. "Read twice" is the same query on the built plan, which
reads the payload in a second scan of `src_wide` (two scans in `EXPLAIN`), so it never enters the barrier or the rank. "No DECIDE" is `SELECT COUNT(*), SUM(length(payload)) FROM src_wide`.
The footprint column is the macOS peak memory footprint; maximum resident size can read lower once the system compresses
pages (A1 held at 5,000,000 rows gave 2,524 and 3,320 MiB resident in two runs against 5,212 MiB footprint both times),
so the footprint is the number to read.

| Peak footprint, MiB | Rows | No DECIDE | Unused (control) | Held | Read twice |
|---|---:|---:|---:|---:|---:|
| A1 | 1,000,000 | 7 | 32 | 1,049 | 73 and 73 |
| A1 | 5,000,000 | 14 | 131 | 5,212 | 353 and 353 |
| S1 | 1,000,000 | 6 | 99 | 651 and 661 | 145 and 149 |
| S1 | 5,000,000 | 14 | 725 | 3,362 and 3,351 | 811 and 829 |

Where two values are given they are two runs. The 512-byte payload is 488 MiB per 1,000,000 rows and 2,441 MiB per
5,000,000.

**Reading.**

- Held rows cost memory in proportion to the input, for both rules. Over the control, A1 adds 1,017 MiB at 1,000,000
  rows and 5,081 MiB at 5,000,000 (about two payload copies per row), and S1 adds about 560 MiB and about 2,630 MiB
  (about one copy; the rank's sort buffer is the likely reason, not isolated). The control itself grows only with the narrow columns.
- Reading the source twice stays near the control: 73 and 353 MiB for A1, 145 to 149 and 811 to 829 MiB for S1, which is
  the narrow plan plus the join. That is 4.0 to 14.8 times less than holding at 5,000,000 rows.
- The wide column is the cause. Without it nothing grows (control), and a plain scan of the same payload streams
  in 6 to 14 MiB.
- Peak memory is the only thing measured here. The machine was busy with a build, so the wall times (0.3 to 1.9 s) are
  indicative only. Why a wide result is slow to collect is not isolated.

**Current behavior.** For a stored table, a wide parent-read column comes from the second read (below), which costs a
few hundred MiB at 5,000,000 rows. Any other source holds its rows, which costs memory in proportion to the input: 5.2 GiB
at 5,000,000 rows for A1. The solver path holds more than direct solve
on the same query (A1 at 1,000,000 rows: 1,655 MiB against 1,049; S1: 1,012 MiB against 652).

### Second read of a stored table

When the source is one DuckDB base table under non-volatile projections and filters, the plan carries the table's
`rowid` as a hidden column. After unused-column removal, when it is known what the parent reads, each parent-read output
that is a plain stored column the rule's plan only forwards comes from a second scan of the table, joined on `rowid`
under the boundary. That column never enters the barrier or the rank, and the first read still checks every row, so a
bad last row under `LIMIT 1` still raises. Only a variable-width column moves (`VARCHAR`, `BLOB`, `BIT`, lists,
arrays, structs, maps, unions: a physical type that is not constant-size); a fixed-width one stays in the single read,
and a boundary with nothing to move plans one scan. Holding a variable-width column is what costs memory, so that is
where the second read pays. For a fixed-width column the effect depends on the rule, measured 2026-10-13 on tables that
hold only the columns in play, so that `SET disabled_optimizers = 'unused_columns'` (one read, rows held) keeps nothing
extra. With a parent that reads a `BIGINT` key, A1 took 0.12 s moved against 0.13 s held (71 against 80 MiB) at
1,000,000 rows and 0.44 against 0.52 s (346 against 370 MiB) at 5,000,000; S1's global top-K at 5,000,000 rows took
0.28 to 0.33 s moved against 0.21 to 0.25 s held, with the same memory. The fixed-width rule gives up A1's small gain to
avoid S1's 30% loss, and needs no tuning number. A join, an aggregate, a volatile or computed column, or a source that already reads `rowid` keeps the single
read: those have no stable row key, or reading them twice could change the rows. A plan that streams (A1's constant
problem) also keeps one read, since it holds nothing.

The step runs inside the `UNUSED_COLUMNS` pass, right after removal, not beside DuckDB's `LATE_MATERIALIZATION` (which
uses the same `rowid` join for Top-N). The passes in between add projection maps that unused-column removal does not keep
up to date, so pruning again after them is unsafe. With that pass disabled the row id is dropped and the plan holds rows.

## Result types

A declared `BOOL` decision has a 0/1 domain but returns SQL `INTEGER`, as on the solver path.

Details: `direct_result_boundary.cpp` (map checks, serialization, pruning hook), `direct_second_read.cpp` (second read) and `direct_builder.cpp` (barrier and
pruning proof). The optimizer passes that run after DECIDE were audited for parent filter, join, aggregate, nested, and
serializer cases; the parent-context rows in `test_direct_three_way.py` and the pruning tests in
`test_direct_user_facing.py` keep those guarded.
