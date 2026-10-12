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
in memory, and the rows include any wide column the parent reads. The section below records what that costs. Whether
to read the input twice instead is open (`../rules/_harness/todo.md`, H-14).

### Memory of holding rows

**Setup** (measured 2026-10-09 with the binary at `de90502e4b`). A stored database with a narrow table `src` (id, t, cap, score, f) and
`src_wide` (the same plus `repeat('p', 512) AS payload`), the same shapes as `benchmark/decide/profile_direct_a1.py`.
Each cell is a fresh `decidb -readonly` process with `decide_direct_solve='require'`, peak memory from `/usr/bin/time -l`,
one aggregate row read back. A1 is `x(REAL) ... BETWEEN 0 AND cap MINIMIZE SUM(POWER(x - t, 2))`; S1 is
`x(BOOL) ... SUM(x) <= rows/10 MAXIMIZE SUM(score * x)`. "Unused" reads `COUNT(*), SUM(x)` (payload pruned, the control).
"Held" adds `SUM(length(payload))`. "Read twice" joins `src_wide` to the DECIDE result of the narrow `src` on `id`, so the
payload never enters the barrier or the rank. "No DECIDE" is `SELECT COUNT(*), SUM(length(payload)) FROM src_wide`.
The footprint column is the macOS peak memory footprint; maximum resident size can read lower once the system compresses
pages (A1 held at 5,000,000 rows gave 2,524 and 3,320 MiB resident in two runs against 5,212 MiB footprint both times),
so the footprint is the number to read.

| Peak footprint, MiB | Rows | No DECIDE | Unused (control) | Held | Read twice |
|---|---:|---:|---:|---:|---:|
| A1 | 1,000,000 | 7 | 32 | 1,049 | 80 |
| A1 | 5,000,000 | 14 | 131 | 5,212 | 316 |
| S1 | 1,000,000 | 6 | 99 | 651 and 661 | 153 and 156 |
| S1 | 5,000,000 | 14 | 725 | 3,362 and 3,351 | 945 and 993 |

Where two values are given they are two runs. The 512-byte payload is 488 MiB per 1,000,000 rows and 2,441 MiB per
5,000,000.

**Reading.**

- Held rows cost memory in proportion to the input, for both rules. Over the control, A1 adds 1,017 MiB at 1,000,000
  rows and 5,081 MiB at 5,000,000 (about two payload copies per row), and S1 adds about 560 MiB and about 2,630 MiB
  (about one copy; the rank's sort buffer is the likely reason, not isolated). The control itself grows only with the narrow columns.
- Reading the source twice stays near the control: 80 and 316 MiB for A1, 153 to 156 and 945 to 993 MiB for S1, which is
  the narrow plan plus the join. That is 3.4 to 16.5 times less than holding at 5,000,000 rows.
- The wide column is the cause. Without it nothing grows (control), and a plain scan of the same payload streams
  in 6 to 14 MiB.
- Peak memory is the only thing measured here. The machine was busy with a build, so the wall times (0.3 to 1.9 s) are
  indicative only. Why a wide result is slow to collect is not isolated.

**Current behavior.** Holding rows costs memory in proportion to the input for wide parent-read columns: 5.2 GiB at
5,000,000 rows for A1. Reading the source twice would cost a few hundred MiB. The solver path holds more than direct solve
on the same query (A1 at 1,000,000 rows: 1,655 MiB against 1,049; S1: 1,012 MiB against 652).

**Planned for stored tables.** Read the source twice when the parent reads a column the rule does not need and the source can be
read twice safely, and keep holding otherwise. The hand-written join above used a unique `id`; the plan would carry its own
row key. A stored table has `rowid`; a join, a subquery with `random()` or an aggregate has none, and reading it twice
could change the rows or double the cost, so those keep holding. The check must still run over every input row in the
first read, which the narrow DECIDE query already does. Building it is H-14 in `../rules/_harness/todo.md`.

## Result types

A declared `BOOL` decision has a 0/1 domain but returns SQL `INTEGER`, as on the solver path.

Details: `direct_result_boundary.cpp` (map checks, serialization, pruning hook) and `direct_builder.cpp` (barrier and
pruning proof). The optimizer passes that run after DECIDE were audited for parent filter, join, aggregate, nested, and
serializer cases; the parent-context rows in `test_direct_three_way.py` and the pruning tests in
`test_direct_user_facing.py` keep those guarded.
