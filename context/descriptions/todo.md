# Work queue: direct solve — finished

The queue is empty. What this round did, in plain words:

- **Tests trimmed.** 475 direct-solve test cases that checked nothing another test did not already check were removed,
  after measuring both which code each test runs and which planted mistakes it catches. Four new tests cover edges no
  test checked before. The suite went from 3,119 to about 2,700 tests per run with no loss of what it checks.
- **One way to build expressions.** All direct-solve code now builds its plan expressions through one helper
  (`ExpressionBuilder`), so a new rule has one style to copy.
- **Three wrong-answer or crash bugs fixed:**
  - a `WHEN` or `PER` on a `BETWEEN` or a bracketed `AND` was ignored by the solver;
  - an `OR` between constraints was silently treated as `AND` (it is now an error that says so);
  - direct solve crashed when the input ended in `ORDER BY`, `DISTINCT` or a large join and the outer query read a
    source column (the result operator now owns its column addresses, as a projection does).

Next: build the queue for the next problem class (R1, continuous knapsack) with `/make-queue`.
