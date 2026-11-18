-- CU-20: one clause, two models. Both answer sum(x) = 17.
-- Run each statement with DECIDB_DUMP_MODEL=<file> and read num_vars and num_rows.
--   literal bound  (composed route): 13 variables, 13 rows
--   subquery bound (value route):    14 variables, 25 rows (12 identical rows)
SET decide_direct_solve = 'off';
SET threads = 1;
CREATE TABLE t AS SELECT i AS id, (i * 7 % 5 + 1) AS a, (i * 3 % 4 + 1) AS b FROM range(1, 13) r(i);
SELECT sum(x) AS s FROM (
  SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= 5 AND sum(a * x) + max(b * x) <= 40 MAXIMIZE sum(x));
SELECT sum(x) AS s FROM (
  SELECT id, x FROM t DECIDE x(INT)
  SUCH THAT x <= 5 AND sum(a * x) + max(b * x) <= (SELECT max(id) + 28 FROM t) MAXIMIZE sum(x));
