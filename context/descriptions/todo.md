# Report: direct solve for R1, the continuous knapsack (2026-10-22 to 2026-10-23)

Scratchpad. This file replaced the work queue when its goal finished; delete it once the work is reviewed and
committed. Technical detail lives in the `done.md` files listed at the end.

## The problem

A continuous knapsack is a budget shared among amounts that can be fractional: "spend at most B on materials, taking
as much of each as is worth it". Before this work DeciDB sent every such query to an external solver (Gurobi or
HiGHS). The answer can be computed with ordinary SQL instead: put the items in order of value per unit of budget and
fill the budget from the top. The goal was to make DeciDB do that itself, for every shape the design document admits,
with exactly the solver's answers and errors.

## What was done, in order

1. **The method and its proof** (committed `cf64948648`). The textbook method assumes every item costs a positive
   amount. Real queries can have items that cost nothing, items that free budget (negative cost), items with no upper
   or lower limit, budgets written as "at least" or "exactly", and several budgets, one per group. The design document
   now describes one method that covers all of these: find a single price per unit of budget, then every item goes to
   whichever end of its range that price favours. The proof shows the result is optimal and says exactly when a query
   has no answer (infeasible) or an endlessly improving one (unbounded). A script checked the method against an
   independent solver on 20,000 random problems with no mismatch.
2. **Shared code moved out of S1** (committed `9736f8bdca`). Helpers that only the S1 rule used, and R1 also needed,
   now live in the shared files. No behaviour changed.
3. **The old random-query test suite removed** (committed `aa9ee8fe6b`). It caught nothing the named tests do not.
   Before deleting it, a deliberately broken check proved that seven remaining tests still catch that breakage.
4. **The core rule** (not committed). One fractional decision and one "at most B" budget are answered by plain SQL
   steps: one sort plus running totals. The price is found by a test that cannot be fooled by rounding, and items with
   no upper or lower limit are counted apart so the arithmetic never meets "infinity minus infinity".
5. **Every budget direction** (not committed). "At least", "exactly" and "between" use the same search; the same sum
   written twice counts as one budget. R1 also joined the suite of checks every rule must pass.
6. **Budgets per group and under a condition** (not committed). `PER region` gives each region its own price, and a
   `WHEN` on the budget leaves rows out of it. Another helper that S1 had a private copy of was made shared.
7. **Several decisions in one budget** (not committed). Each row is split into one item per decision, the same steps
   run on items, and the items are gathered back into one row per source row. This is how DuckDB itself handles
   `UNPIVOT`.
8. **Budgets read from the data** (not committed). A budget can be a column, an expression or an aggregate such as
   `0.3 * SUM(cost)`, read per group exactly as S1 reads its data-valued limits.
9. **Speed measured** (not committed). See the negative result below.

All five checks pass on the final tree: 3,089 Python tests in each of the three main modes, 3,088 plus the one known
Gurobi-only failure with HiGHS forced, and 2,302 C++ assertions in 59 cases. Every branch of the R1 code is reached by a
test.

## Decisions made while the owner was away

| Where | Decision | Why |
|---|---|---|
| Core rule | The shared coefficient reader is renamed `DirectReadLinearPart` (and its types) | It now reads budgets as well as objectives |
| Core rule | A constant budget of 1e20 or more stays on the solver | The solver's reading of such a number depends on the rows |
| Directions | Same-sum constraints merge into one budget (largest lower, smallest upper limit) | Covers `BETWEEN` however the parser spells it |
| Groups | A row outside the budget behaves like a zero-cost row | Matches the solver; no extra code path |
| Groups | S1's and R1's identical scope helper became `DirectIRConstraint::Membership` | The shared-code rule says the second user moves it |
| Several decisions | Split rows with `UNNEST`, gather back with a hash aggregate | DuckDB's own pattern; reading the source twice would cost more |
| Data budgets | A data value of 1e20 or more is read as the number it is | Gurobi loads it as a number; a data value cannot be declined in advance |
| Tests | One group of six test cases cut to two | Each case must reach its own branch |

## What was not done, and doubts

- **Negative result: R1 is not faster than Gurobi on one global budget.** At 5,000,000 rows it takes 0.7 to 1.2 times
  Gurobi's time and as much memory (3 to 5 GiB); two decisions are about 4 times slower than Gurobi. It wins clearly
  only with `PER` groups (2.6 to 2.8 times faster), and beats HiGHS on everything but two decisions. Profiling shows
  why: half the time is one sort over the whole input, and every step carries every source column. A fix is written
  up as a decision for you: `rules/r1_continuous_knapsack/todo.md` § R1-11 (recommended: search for the price over a
  narrow copy of the data and join the result back, about one day).
- **One known difference from Gurobi.** A data-valued "at most" limit between 1e21 and 1e25 beside an item with no cap
  makes Gurobi report "unbounded" for a problem that is bounded; R1 returns the real optimum.
- **Tests that reach no unique branch.** About 60 R1 cases (parent queries, random rows, boundary values) check
  answers rather than code branches, as A1's tables do. The same reasoning removed the fuzz suite, so you may want to
  trim them.
- **Not tested:** parent-query wrappers around grouped budgets (they use the same wrapper as the ungrouped ones).
- **Nothing after step 3 is committed.** The test oracle cache (`test/decide/results/oracle_cache.json`, untracked)
  gained about 130 entries from new Gurobi solves.

## Still open

- R1-11, the plan-cost decision above — `08_direct_solve/rules/r1_continuous_knapsack/todo.md` § R1-11
- H-02, the front end for the new language (blocked on a stable language branch) — `08_direct_solve/framework/todo.md`
  § H-02

## Where the detail is

| Topic | File |
|---|---|
| The method and its proof | `08_direct_solve/rules/r1_continuous_knapsack/definition.md` § Relational construction, § Correctness, § Admitted class |
| What R1 admits, declines, and where it differs from the solver | `08_direct_solve/rules/r1_continuous_knapsack/done.md` |
| The plan, step by step | the same file, § How it works |
| The tests | the same file, § Verified by |
| Speed and memory | the same file, § Performance |
| Shared helpers and test counts | `08_direct_solve/framework/done.md` |
