# S1 (top-k and cardinality intervals) — open work

What S1 does today is in `done.md`; the class is in `definition.md`. How to work and the checks are in
`../_harness/todo.md` § How to work any task. Every widening also meets the exit gate at the end of this file.

## S1-12 — S1 reads a per-row bound on `x` in its own way, and so does A1

- **Needs.** A third rule that reads per-row bounds (A3, A5 or A6). S1 is heavily tested, so changing its pins has to earn
  its risk.
- **Problem.** S1 decides "which of {0, 1} may this row take" for a per-row bound on `x` with its own code (`PinDomain`,
  `ConstantPinDomain`, `SourceBooleanPinValue`, and the numeric-pin code in `ProvePins`, about 140 lines). A1 decides
  "what interval may this row take" for the same kind of bound with different code (`ClauseBounds` in `a1_rule.cpp`).
  They also treat the solver's reading of a bare constant bound differently: S1 declines some spellings by inspecting
  them, A1 asks the `variable_bound` fact, which S1 never reads.
  Two readings of one rule can drift apart, and a third per-row rule would write a third.
- **Change.** Move A1's per-row bound algebra to a shared builder that yields a lower and an upper bound per row; give S1's
  pins that builder with the domain {0, 1}; keep S1's rank and count logic as it is.
- **Done when.** S1's pin code is a call to the shared builder, S1's suites (three-way, contract, fuzz) pass unchanged, and
  the A1 suites still pass. About 1 day.

## S1-13 — Refresh S1's performance table

- **Problem.** The performance table in `done.md` was measured on 2026-10-05, and `done.md` says the cost of the per-row
  NULL message on clean data is not measured. The 15 to 100 times speedups are part of the reason `auto` is the default.
- **Change.** After `make release`, on a quiet machine with a Gurobi licence, run the sweep under "Reproduce" in `done.md`
  (about 4.5 minutes; it builds the 5,000,000-row and wide databases, about 3 GiB), plus one run with and without NULLs in
  the score column to measure the message cost, and a constraints-only query (no objective) at each size. Rewrite the table and its date.
- **Done when.** `done.md` has numbers from a run you made, with their date, and says what the NULL message costs and how a
  constraints-only query compares with the solver. Machine
  load changes timings, so run nothing else meanwhile.

## S1-14 — Admit a bound on `x` with no `WHEN` outside {0, 1}

- **Decision needed.**
- **Problem.** S1 declines a constant bound on `x` with no `WHEN` outside {0, 1} (`done.md` § Explicitly out of scope,
  `S1_SOLVER_ONLY_ROW_BOUNDS` in `test/decide/tests/_direct_rule_fixtures.py`). The solver reads these as Boolean
  arithmetic: one that always holds (`x >= -3`, `x > -3`) changes nothing, and one that cannot hold (`x >= 2`, `x <= -1`,
  `x < 0`, `x > 1.5`, `x = -1`) raises DECIDE's usual infeasible error before it reads a row, on an empty source too. So a
  query like `x >= -3 AND SUM(x) <= 3` goes to the solver for no reason.
- **Options.**
  1. Admit only the bounds that always hold, as no constraint. Bounds that cannot hold stay declined through
     `DecideBoxContradictsDomain`, and the solver raises. Small; no new error path in S1.
  2. Also admit the bounds that cannot hold, with S1 raising the same error before it reads a row. Needs a plan-time
     error matching the solver's text and `DIAGNOSE` skip. No speed to win on a query that only errors.
- **Recommendation.** Option 1.
- **Change.** Read a lower bound below 0 as "both values allowed" in S1's pins. Check whether A1 declines the same
  bounds on a `BOOL` and give it the same reading, or record why not.
- **Done when.** The always-hold spellings leave `S1_SOLVER_ONLY_ROW_BOUNDS`, S1's three-way, contract and fuzz suites
  cover them against the oracle, and the S1 exit gate below is met.

## Exit gate for any S1 widening

An independent proof, behavior tests (the standard tests in `../../architecture/rules.md`), and end-to-end measurement that
does not count timeouts, solver limits or unsupported cases as wins. Finding a relational expression in the catalogue is
not enough.
