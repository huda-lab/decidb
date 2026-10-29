# Known bugs

## BUG-2 — One matrix run of `DECIDB_TEST_DIRECT_SOLVE=off` reported one outcome fewer

- **Observed:** on 2026-10-27 `test/decide/run_direct_matrix.sh` printed `off: 4520 passed, 335 xfailed, 0 failed`
  (4,855 outcomes) where `auto` and `serializer` printed 4,521 + 335. The rerun of the off suite gave 4,521 passed and 0
  skipped. The matrix wrote its logs to a `mktemp -d` folder that was gone, so the missing test is unknown.
- **Next step:** the matrix now keeps its logs (`test/decide/results/matrix_<time>/`) and prints each mode's outcome
  total; when the totals differ again, find the missing test from the logs.

## BUG-16 — A row's range over its variables' boxes overflows: internal errors and wrong refusals

Found by the overflow review (2026-10-29). The fix for this entry and BUG-18 to BUG-22 is one design,
[code_quality.md § CQ-5](code_quality.md#cq-5--one-reach-calculator-and-checked-arithmetic-for-derived-numbers).
Probes use `VALUES` tables, b = 1e308, `y` in [0, 5]; `decide_direct_solve` off and auto agree.

- **Observed:** the smallest and largest value a row can take over its variables' boxes is computed in seven places
  (`linearization_bigm.cpp:54,127,245`, `ilp_linearization.hpp:347` `Span`, `:392` `CoverRow`,
  `linearization_minmax.cpp:544,924`, `linearization_reducer_value.cpp:43`, `linearization_bilinear_abs.cpp:274`), which
  mark an open end two ways (a flag and an infinite value). An overflowing product reads as an open variable:
  - `SUM(x) + MAX(b*y) <= 10`: "Column bounds invalid … [0, inf]" on both solvers (`CoverRow`'s check is debug-only).
  - `MAX(y + c) >= 0`, c = ±1e308: InternalException "coefficient not finite" on both.
  - `MAXIMIZE SUM(ABS(b*y))` and `norm(b*y, 0) <= 1`: InternalException "index -1" on HiGHS (`var_names[INVALID]`,
    `linearization_bilinear_abs.cpp:305`); Gurobi says "unbounded" or TOO_LARGE.
  - `MAX(b*y) >= 1`: HiGHS says "one of them is unbounded" though `y` is bounded; `MINIMIZE MAX(b*y)` fails in HiGHS's
    row scaling. Gurobi solves both.
- **Expected:** TOO_LARGE naming the clause; an open end only from a variable with no bound, named in the refusal.

## BUG-18 — Literal factors merged past a DOUBLE silently drop a constraint

- **Observed:** `MergeLikeAtoms` (`decide_canonicalizer.cpp:900-910, 941-948`) folds literal factors with plain `+`.
  `1e308*y + 1e308*y <= 1e308`, `y` in [0, 5], returns y = 5 on both solvers (the maximum is 0.5): the factor becomes
  inf (EXPLAIN shows `1e1000 * y`). `SUM(y)*1e308 + SUM(y)*1e308 <= 10` raises "1e1000 * 1 is NaN or infinite", a text
  the user never wrote; singleton folding then evaluates `inf*n` in SQL (`singleton_row_folding.cpp:82-130`).
- **Expected:** TOO_LARGE naming the clause. Whether the clause text is available at bind time is unchecked.

## BUG-19 — Direct solve's R1 SQL overflows to an infinite room: wrong answer

- **Observed:** R1's generated ratio `v/w` and rooms `w·(high − a)` (`r1_continuous_knapsack_rule.cpp:1111, 1134-1135`)
  and its window sums (`:435-436`) are DuckDB DOUBLE arithmetic, which returns inf silently. `SUM(1e308*x) <= 1`, `x`
  in [0, 5], under direct solve (require and auto) returns 5, 5; the answer is about 1e-308.
- **Expected:** TOO_LARGE, as A1's answers already check (`a1_separable_box_rule.cpp:793`).

## BUG-20 — A derived number beyond the solver's infinity reads as infinite: wrong "infeasible"

- **Observed:** a finite derived coefficient, box end or bound of 1e100 or more is read as infinite by the backend.
  `ABS(y + b) >= 1` (feasible) gives "infeasible" on Gurobi and TOO_LARGE on HiGHS; `MAXIMIZE SUM(ABS(y - b))` with
  b = ±1e308, a box-only query, gives "infeasible" on Gurobi.
- **Expected:** TOO_LARGE from the model's own infinity (`DECIDE_EFFECTIVE_INFINITY`), not the DOUBLE limit.

## BUG-21 — One overflow, different messages

- **Observed:** a computed sum is checked with the COEFFICIENT role in direct solve (`s1_uniform_matroid_rule.cpp:797`,
  `a1_separable_box_rule.cpp:685`), so `MAXIMIZE SUM(a*x) + SUM(b*x)` says "a + b is NaN or infinite" on require and
  TOO_LARGE naming the objective on off. Like terms merged in SQL (`decide_term_split.cpp:40`, `b*y + b*y <= b`) and a
  reciprocal scale (`physical_decide.cpp:2610`, `SUM(y)/1e-310 <= 10`) quote `b + b` and `1 / 1e-310`.
- **Expected:** one TOO_LARGE text naming the clause on every path.

## BUG-22 — DIAGNOSE's row norm overflows above 1.3e154

- **Observed:** `rms_norm` (`decide_diagnostic_engines.cpp:192, 228`) sums squares, so a coefficient above about
  1.3e154 gives a weight of 0 (a free slack) or inf/inf = NaN. Probes at 1e200 are blocked first by the solvers'
  large-number behavior (Gurobi reports a violated model "feasible"; that part is the vendor's).
- **Expected:** a norm that cannot overflow (scale by the row's largest entry), or TOO_LARGE.

## Limitations (not bugs)

None.

Resolved behavior is documented by its owning `done.md`.
