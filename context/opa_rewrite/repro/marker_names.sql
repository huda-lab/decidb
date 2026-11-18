-- CU-01: an internal marker is a substring of an expression's name, and a user can write that name.
-- Each pair runs one query twice; only an identifier's spelling differs, so the two answers must match.
SET decide_direct_solve = 'off';

-- 1. A decision named like the L0 indicator. Plain: sx = 12, sq = 0. Named: sx = 0, sq = 12.
CREATE TABLE t AS SELECT i AS id FROM range(1, 13) r(i);
SELECT 'plain' AS q, sum(x) AS sx, sum(q) AS sq FROM (
  SELECT id, x, q FROM t DECIDE x(INT), q(BOOL)
  SUCH THAT x <= 5 AND x >= 0 AND x - 3 * q >= 1 MINIMIZE sum(x + q));
SELECT 'named' AS q, sum(x) AS sx, sum("__l0auto_ind_q") AS sq FROM (
  SELECT id, x, "__l0auto_ind_q" FROM t DECIDE x(INT), "__l0auto_ind_q"(BOOL)
  SUCH THAT x <= 5 AND x >= 0 AND x - 3 * "__l0auto_ind_q" >= 1 MINIMIZE sum(x + "__l0auto_ind_q"));

-- 2. A data column named like the query-wide-value tag. Row 3 has factor 3, so 3 * SUM(x) <= 2 forces SUM(x) = 0.
--    Plain: 0, 0, 0. Named: 0, 1, 0.
CREATE TABLE w(id INT, "w__other__" DOUBLE, "w__query_wide_value__" DOUBLE, v DOUBLE);
INSERT INTO w VALUES (1,1,1,5),(2,2,2,6),(3,3,3,7);
SELECT 'plain' AS q, id, x FROM w DECIDE x(BOOL) SUCH THAT "w__other__" * SUM(x) <= 2 MAXIMIZE SUM(x * v);
SELECT 'named' AS q, id, x FROM w DECIDE x(BOOL) SUCH THAT "w__query_wide_value__" * SUM(x) <= 2 MAXIMIZE SUM(x * v);

-- 3. A data column named like the definition-function tag. Plain: 23. Named: INTERNAL Error
--    "DECIDE tree: nonlinear body helper 0 has no function node".
CREATE TABLE c AS SELECT i AS id, (i * 7 % 5 + 1) AS a, (i * 7 % 5 + 1) AS "__definition_function_0__"
  FROM range(1, 13) r(i);
SELECT 'plain' AS q, sum(x) FROM (
  SELECT id, x FROM c DECIDE x(INT), y(INT) SUCH THAT x <= 5 AND y <= 1 AND sum(a * x + y) <= 40 MAXIMIZE sum(x));
SELECT 'named' AS q, sum(x) FROM (
  SELECT id, x FROM c DECIDE x(INT), y(INT)
  SUCH THAT x <= 5 AND y <= 1 AND sum("__definition_function_0__" * x + y) <= 40 MAXIMIZE sum(x));
