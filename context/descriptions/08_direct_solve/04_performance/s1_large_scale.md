# S1 direct-solve performance measurements

These CTAS sweeps predate selective output pruning. They request every source
column in the output, so the pruning change does not remove their wide payload.
For a parent that requests only aggregates, see the [API phase and pruning
report](s1_api_phase.md#selective-output-pruning).

Measured on 2026-10-01 with the uncommitted direct-solve prototype over base
commit `616145dec10547a2a89313a65fc7b059de29f6d6`, macOS arm64, 18 GiB RAM.
The release CLI used four DuckDB threads. Each run used a fresh process and
materialized every DECIDE source column and `x` into a temporary table. A final
aggregate checked row count, selected count, and primary objective. The query
timer covers the `CREATE TEMP TABLE AS` statement, including planning, solving
or relational execution, and output materialization; process startup and the
final aggregate are outside it. Peak RSS is for the whole process. The original
range and stored-scan comparisons all finished; the joined-source HiGHS
five-million-row trials below reached the process limit and are excluded from
completed comparisons.

The reproducible runner is [`benchmark/decide/profile_direct_s1.py`](../../../../benchmark/decide/profile_direct_s1.py).
The recorded raw observations are [the scale sweep](raw/s1_scale_raw.csv),
[the large narrow sweep](raw/s1_large_narrow_raw.csv), [one-million-row wide runs](raw/s1_million_wide_raw.csv),
and [five-million-row wide runs](raw/s1_five_million_wide_raw.csv), plus the
[large capacity extremes](raw/s1_large_extremes_raw.csv),
[stored-table sweep](raw/s1_stored_large_raw.csv) and
[stored-table wide runs](raw/s1_stored_wide_raw.csv), plus the
[stored-join sweep](raw/s1_stored_join_raw.csv). Run from the
repository root after `make release`:

```sh
python3 benchmark/decide/profile_direct_s1.py --rows 1000 10000 100000 --widths 0 512 --fractions 0 0.1 1 --patterns mixed tied --repeats 2 --timeout 120
python3 benchmark/decide/profile_direct_s1.py --rows 1000000 5000000 --widths 0 --fractions 0.1 --patterns mixed tied --repeats 3 --timeout 90
python3 benchmark/decide/profile_direct_s1.py --rows 1000000 --widths 512 --fractions 0.1 --patterns mixed --repeats 2 --timeout 90
python3 benchmark/decide/profile_direct_s1.py --rows 5000000 --widths 512 --fractions 0.1 --patterns mixed --repeats 1 --timeout 90
python3 benchmark/decide/profile_direct_s1.py --rows 1000000 5000000 --widths 0 --fractions 0.1 --patterns mixed tied --repeats 2 --timeout 120 --source-kind stored
python3 benchmark/decide/profile_direct_s1.py --rows 1000000 5000000 --widths 512 --fractions 0.1 --patterns mixed --repeats 1 --timeout 150 --source-kind stored
python3 benchmark/decide/profile_direct_s1.py --rows 5000000 --widths 0 --fractions 0 1 --patterns mixed --repeats 1 --timeout 120
python3 benchmark/decide/profile_direct_s1.py --rows 1000000 5000000 --widths 0 --fractions 0.1 --patterns mixed --repeats 2 --timeout 150 --source-kind stored_join
```

## Large narrow input

Each input row had an integer ID and a computed DOUBLE score. Capacity was
10% of row count. Times and peak RSS below are medians of three complete runs.
The score pattern `mixed` varies across roughly 10,000 values; `tied` has 17
distinct values. Every mode returned the specified number of rows, respected
capacity, and attained the same primary objective within floating-point
summation error.

| Rows | Pattern | Direct time / RSS | Gurobi time / RSS | HiGHS time / RSS |
|---:|---|---:|---:|---:|
| 1,000,000 | mixed | 0.084 s / 161 MiB | 0.505 s / 500 MiB | 8.527 s / 788 MiB |
| 1,000,000 | tied | 0.077 s / 161 MiB | 0.499 s / 487 MiB | 1.117 s / 819 MiB |
| 5,000,000 | mixed | 0.403 s / 717 MiB | 2.853 s / 2,271 MiB | 42.978 s / 2,808 MiB |
| 5,000,000 | tied | 0.394 s / 717 MiB | 2.541 s / 2,244 MiB | 6.268 s / 2,952 MiB |

At five million rows, direct solve was about 6.5–7.1 times faster than Gurobi
and 15.9–106.6 times faster than HiGHS on these patterns. The raw phase spans
attribute the solver path mostly to backend optimization; direct analysis and
construction each took less than 0.04 ms per run. These ratios apply to the
admitted global upper-cardinality shape and this computed source, not to every
DECIDE problem.

## Stored-table input

The stored-table runs first created a temporary DuckDB table containing the
same ID and DOUBLE score, then timed only the DECIDE-to-result CTAS. Table
creation was outside the query timer; the process-wide peak RSS still includes
the source table. Capacity was 10% of row count. The figures are medians of two
complete runs. Every mode returned all rows, selected exactly capacity, and
matched the primary objective within floating-point summation error.

| Rows | Pattern | Direct time / RSS | Gurobi time / RSS | HiGHS time / RSS |
|---:|---|---:|---:|---:|
| 1,000,000 | mixed | 0.043 s / 227 MiB | 0.519 s / 521 MiB | 8.473 s / 783 MiB |
| 1,000,000 | tied | 0.032 s / 228 MiB | 0.494 s / 507 MiB | 1.077 s / 826 MiB |
| 5,000,000 | mixed | 0.204 s / 1,032 MiB | 2.833 s / 2,369 MiB | 42.652 s / 2,669 MiB |
| 5,000,000 | tied | 0.171 s / 1,035 MiB | 2.558 s / 2,346 MiB | 6.340 s / 2,903 MiB |

At five million rows this stored-table query was about 14 times faster than
Gurobi on both score patterns. This ratio differs from the computed-source
ratio because the score was already stored; the process memory figures also
have a different baseline from the computed-source sweep.

An `EXPLAIN ANALYZE` check on a 100,000-row, 10%-capacity direct query with a
parent `COUNT(*), SUM(x)` showed 100,000 rows at the source scan, score
projection, guard filter, global window, result projection, and output
boundary. The parent aggregate alone reduced the stream to one row. This
confirms the plan keeps all source rows through the rank on that fixture; it
does not measure the bytes materialized by each operator.

```sql
SET decide_direct_solve='require';
EXPLAIN ANALYZE SELECT COUNT(*), SUM(x) FROM (
  FROM (SELECT i, (i % 101)::DOUBLE AS score FROM range(100000) t(i)) s
  DECIDE x(BOOL) SUCH THAT SUM(x)<=10000 MAXIMIZE SUM(score*x)
) q;
```

One wide stored-table run per mode retained a 512-byte source value. At one
million rows, direct/Gurobi/HiGHS times were 0.283/0.753/8.750 seconds, with
peak RSS 1,514/1,990/2,227 MiB. At five million rows, times were
2.451/4.818/44.515 seconds and RSS 3,536/3,890/3,687 MiB. All modes returned
the same row and selected counts and matched the primary objective within
floating-point summation error. These single observations show the wide-row
cost remains substantial; they are not stable medians.

## Stored-table join input

The joined workload stores the source rows and joins each one to exactly one
of 97 dimension rows. The coefficient is the stored score multiplied by a
positive dimension weight. Source and dimension creation are outside the CTAS
timer; the join, coefficient evaluation, assignment, and result
materialization are inside it. Capacity is 10% of source rows. Figures below
are medians of two trials; all completed modes returned all rows and matched
the primary objective within floating-point summation error.

| Rows | Direct time / RSS | Gurobi time / RSS | HiGHS time / RSS |
|---:|---:|---:|---:|
| 1,000,000 | 0.062 s / 231 MiB | 1.182 s / 561 MiB | 38.158 s / 749 MiB |
| 5,000,000 | 0.215 s / 1,039 MiB | 3.423 s / 2,015 MiB | process timeout at 150 s, twice |

The five-million-row direct/Gurobi comparison shows about a 16-fold time
advantage on this joined input. The HiGHS timeout is incomplete evidence, not
a successful comparison. The changed score distribution makes this a harder
solver workload than the stored scan; these numbers do not isolate join cost.

## Width limit and counterexamples

With a 512-byte source value retained in the output, the one-million-row mixed
case had median times of 1.795 s direct, 2.088 s Gurobi, and 10.239 s HiGHS
(two runs). Direct peak RSS was 1,179 MiB. At five million wide rows, one run
took 8.431 s direct, 10.981 s Gurobi, and 51.026 s HiGHS. Direct peak RSS was
3,631 MiB, above Gurobi's 3,245 MiB and HiGHS's 3,371 MiB. The width cost is
therefore material even while the direct time remained lower here. The stored
wide run had lower direct peak RSS than either solver path, whereas the
computed-source wide run had higher direct peak RSS; source evaluation and
storage layout affect the memory result.

The smaller sweep gives cases where direct is not the fastest option. At
100,000 rows with 512-byte values and zero capacity, the mixed pattern's
two-run median was 196.5 ms direct, 184.0 ms Gurobi, and 186.5 ms HiGHS;
direct peak RSS was about 187 MiB versus 140–144 MiB for the solver paths.
At capacity equal to row count, the same pattern was 193.5 ms direct versus
188.5 ms Gurobi and 189.0 ms HiGHS. Millisecond timer resolution also limits
comparisons at 1,000 rows.

At five million narrow computed rows, one additional run at capacity zero
took 0.393 seconds direct, 1.119 seconds Gurobi, and 1.818 seconds HiGHS.
At capacity equal to row count, times were 0.387, 1.303, and 1.684 seconds.
Every mode returned all five million rows, with the same selected count and
primary objective within floating-point summation error. Direct still won
these large extremes, though their solver gaps were smaller than at 10%
capacity. These are single runs; the 100,000-row wide counterexamples still
show that a universal speedup claim is unsupported.

## Decision and remaining measurement work

Keep `decide_direct_solve=off` by default. These measurements establish a large
benefit for the narrow S1 shape on large, narrow inputs, plus a source-width
memory limit and small-workload counterexamples. `auto` remains an explicit
opt-in: a cost estimate must never establish eligibility, and a production
policy needs broader source plans and output-width cases before it can choose
between a proved direct plan and the solver automatically.

The runner records direct analysis/construction and solver model-build,
backend-load, solve, and solver-readback spans alongside total query time and
peak RSS. It does not independently isolate relational execution from CTAS
materialization or measure client streaming of all result rows. That phase
accounting and broader stored-table workloads remain part of PERF-01 and
PERF-02; these observations should not be described as the final production
performance gate.
