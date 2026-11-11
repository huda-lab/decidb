# S3 (maximum-cardinality knapsack) — open work

S3 selects as many `BOOL` items as fit one budget by sorting them by weight and taking the longest affordable prefix.
The class is in `definition.md`. How to work is in `../../framework/todo.md` § How to work any task. A task runs only
the focused tests its **Done when** names; the full gate runs once at combine. The standard tests are in
`../../architecture/rules.md` § The standard tests every rule ships.

## S3-5 — S3's Performance is measured

- **Needs.** A quiet machine; the owner starts the run.
- **Problem.** `done.md` has no Performance section, which every working class has.
- **Change.** Measure the queries `s3` and `s3_global` (`benchmark/decide/direct_solve/queries/`) against their
  comparators in `hand/` (`run.py --compare --sf 1 --queries s3 s3_global`) and against Gurobi and HiGHS (the sweep,
  `run.py --queries s3 s3_global --modes direct gurobi highs`), as `../../../performance/benchmarking/direct_solve.md`
  describes. The comparators drop the rank and the largest fitting rank, which TPC-H's positive costs make unnecessary,
  so the CPU ratio is what S3 pays for admitting negative weights.
- **Done when.** `done.md` § Performance gives CPU against the hand plan and speed against both solvers.
