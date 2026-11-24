-- Writing the model scales with what the clauses keep, not with clauses times rows (pipeline/08_execution/done.md).
-- 2,000,000 rows; each clause keeps 2,000. Run with DECIDB_PROFILE=1 and read execution.tree_model and
-- gurobi.optimize; /usr/bin/time -l gives peak memory.
--   1 clause:   tree_model 29 ms, optimize 168 ms, peak 0.99 GB
--   40 clauses: tree_model 32 ms, optimize 207 ms, peak 1.03 GB
-- The first statement is the 1-clause run; comment it out to time the second alone.
SET decide_direct_solve = 'off';
CREATE TABLE t AS SELECT i AS id, (i % 1000) AS g, (i * 7 % 5 + 1)::DOUBLE AS w, (i % 4 + 1)::DOUBLE AS v
  FROM range(2000000) r(i);
SELECT sum(x) FROM (
  SELECT id, g, x FROM t DECIDE x(REAL) SUCH THAT x <= 1 AND when g = 0: sum(w * x) <= 10 MAXIMIZE sum(v * x));
SELECT sum(x) FROM (
  SELECT id, g, x FROM t DECIDE x(REAL) SUCH THAT x <= 1 AND
  when g = 0: sum(w * x) <= 10 AND when g = 1: sum(w * x) <= 10 AND when g = 2: sum(w * x) <= 10 AND when g = 3: sum(w * x) <= 10 AND when g = 4: sum(w * x) <= 10 AND when g = 5: sum(w * x) <= 10 AND when g = 6: sum(w * x) <= 10 AND when g = 7: sum(w * x) <= 10 AND when g = 8: sum(w * x) <= 10 AND when g = 9: sum(w * x) <= 10 AND when g = 10: sum(w * x) <= 10 AND when g = 11: sum(w * x) <= 10 AND when g = 12: sum(w * x) <= 10 AND when g = 13: sum(w * x) <= 10 AND when g = 14: sum(w * x) <= 10 AND when g = 15: sum(w * x) <= 10 AND when g = 16: sum(w * x) <= 10 AND when g = 17: sum(w * x) <= 10 AND when g = 18: sum(w * x) <= 10 AND when g = 19: sum(w * x) <= 10 AND when g = 20: sum(w * x) <= 10 AND when g = 21: sum(w * x) <= 10 AND when g = 22: sum(w * x) <= 10 AND when g = 23: sum(w * x) <= 10 AND when g = 24: sum(w * x) <= 10 AND when g = 25: sum(w * x) <= 10 AND when g = 26: sum(w * x) <= 10 AND when g = 27: sum(w * x) <= 10 AND when g = 28: sum(w * x) <= 10 AND when g = 29: sum(w * x) <= 10 AND when g = 30: sum(w * x) <= 10 AND when g = 31: sum(w * x) <= 10 AND when g = 32: sum(w * x) <= 10 AND when g = 33: sum(w * x) <= 10 AND when g = 34: sum(w * x) <= 10 AND when g = 35: sum(w * x) <= 10 AND when g = 36: sum(w * x) <= 10 AND when g = 37: sum(w * x) <= 10 AND when g = 38: sum(w * x) <= 10 AND when g = 39: sum(w * x) <= 10
  MAXIMIZE sum(v * x));
