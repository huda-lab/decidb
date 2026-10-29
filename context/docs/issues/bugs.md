# Known bugs

## BUG-23 — HiGHS's presolve ignores the time limit while it finds nothing: Q3 at large stops 339 s late

- **Observed (2026-10-29, `q3_abs_norms.sql` at `large`: 840,000 columns, 1,560,001 rows; solver limit 300 s):**
  presolve starts at 0 s, reports the model unchanged at 638 s, then "Presolve: Time limit reached"; the query ends
  with DeciDB's time-limit error at 639 s. Peak memory 1.9 GiB, one core busy, so it is not a memory kill. HiGHS
  checks the clock in presolve once per 1,024 reductions (`../pipeline/07_solver/done.md`), and this presolve makes
  none. The 2026-09-09 sweep's three `process_timeout` rows for Q3 on HiGHS are this overrun.
- **Next step:** one run with a stack sample at about 200 s names the presolve routine; then a vendored check there,
  marked `DeciDB patch (BUG-23)`, as SOL-1's and BUG-3's are.
- **Reproduce:** Q3 with `${Q3_ROW_LIMIT}` = 120000 and `${Q3_ABS_CAP}` = 900000 on `large.db`,
  `DECIDB_FORCE_SOLVER=highs DECIDB_TIME_LIMIT=300 DECIDB_PROFILE_SOLVER_LOG=1`; about 11 minutes, 1.9 GiB.

## BUG-16 — An overflowed range reads as an open variable: internal errors, a crash, wrong "unbounded"

The overflow rule (owner, 2026-10-29), as DuckDB treats a DOUBLE: arithmetic stays IEEE and nothing prevents an
overflow. A large finite number warns and solves (a solver's verdict on numbers past its infinity stands); an infinite
bound is no bound (BUG-17); an infinite or NaN coefficient raises TOO_LARGE naming the clause where DeciDB reads it.
DIAGNOSE's row norm past 1.3e154 is accepted, since the warning has fired. Fixes for this entry, BUG-18, BUG-19 and
BUG-21 apply the rule; none builds new arithmetic. Probes: `VALUES` tables, b = 1e308, `y` in [0, 5].

- **Observed:** a row's range over its variables' boxes overflows, and the code takes the infinity for an open variable.
  - `SUM(x) + MAX(b*y) <= 10`: "Column bounds invalid … [0, inf]"; `MAX(y + c) >= 0`, c = ±1e308: "coefficient not
    finite". Both are the model's hand-off checks (`ilp_model_builder.cpp:~1396-1440`), raised as internal errors.
  - `MAXIMIZE SUM(ABS(b*y))`, `norm(b*y, 0) <= 1` on HiGHS: "index -1". The ABS refusal
    (`linearization_bilinear_abs.cpp:~291`) finds no open variable to name and reads `var_names[INVALID]`.
  - `MAX(b*y) >= 1` on HiGHS: "one of them is unbounded" from `ThrowUnboundedBigMNaming`
    (`linearization_bigm.cpp:~155`), which every other range refusal shares; no variable is open.
  - Not ours: Gurobi's "unbounded" on `MAXIMIZE SUM(ABS(b*y))` and HiGHS's load refusal on `MINIMIZE MAX(b*y)` follow
    the large-number warning or the vendor's limit.
- **Fix:** the hand-off checks raise TOO_LARGE for a non-finite coefficient, column bound, objective or Q entry (a
  column is named by a clause that reads it, else the objective); structural checks stay internal.
  `ThrowUnboundedBigMNaming` raises TOO_LARGE naming the clause when no column of the row is open, and the ABS refusal
  goes through it.

## BUG-18 — A literal infinite coefficient on one decision drops the clause

- **Observed:** `2e308*y <= 1e308` (or `1e308*y + 1e308*y <= 1e308`, merged by `MergeLikeAtoms`), `y` in [0, 5],
  returns y = 5 with no message on both solvers, direct solve off and on: the model has no row and `y`'s box is
  unchanged. With a second decision in the row (`2e308*y + 0.5*z <= 1e308`) the coefficient check raises, but quotes
  `1e1000` and advises `when isfinite(1e1000):`. The one-decision row goes through singleton folding
  (`decide_singleton_row_folding.cpp`, `FoldKnownBound` accepts an infinite `a`) and is lost after it; locate where.
- **Fix:** the fold reads `a` through the same coefficient check. A coefficient that reads no column raises TOO_LARGE
  naming the clause; the "skip those rows" advice stays for a column's data.

## BUG-19 — Direct solve's R1 SQL overflows to an infinite room: wrong answer

- **Observed:** R1's generated ratio `v/w` and rooms `w·(high − a)` (`r1_continuous_knapsack_rule.cpp:1111, 1134-1135`)
  and its window sums (`:435-436`) are DuckDB DOUBLE arithmetic, which returns inf silently. `SUM(1e308*x) <= 1`, `x`
  in [0, 5], under direct solve (require and auto) returns 5, 5; direct solve off warns and returns 0.
- **Fix:** `DirectValidNumberPredicate` with TOO_LARGE on each column R1 computes, as A1 checks its answers
  (`a1_separable_box_rule.cpp:793`). The answer alone is not enough: it is finite.

## BUG-21 — One overflow, different messages

- **Observed:** a sum DeciDB computes is checked with the COEFFICIENT role in direct solve
  (`s1_uniform_matroid_rule.cpp:797`, `a1_separable_box_rule.cpp:685`), so `MAXIMIZE SUM(a*x) + SUM(b*x)` says
  "a + b is NaN or infinite" on require and TOO_LARGE naming the objective on off. Like terms merged in SQL
  (`decide_term_split.cpp:40`, `b*y + b*y <= b`) and a reciprocal scale (`physical_decide.cpp:2610`,
  `SUM(y)/1e-310 <= 10`) quote `b + b` and `1 / 1e-310`.
- **Fix:** BUG-18's rule: a number DeciDB computes raises TOO_LARGE naming the clause; COEFFICIENT stays for a value
  read from a column.

## Limitations (not bugs)

None.

Resolved behavior is documented by its owning `done.md`.
