SET threads=1;
CREATE TABLE t AS SELECT * FROM (VALUES (1,1,2.0),(2,1,3.0),(3,2,1.5),(4,2,4.0),(5,3,1.0)) t(id,g,w);
EXPLAIN SELECT id, b, x FROM t DECIDE b(BOOL), x(INT)
SUCH THAT x >= 0 AND x <= 5
AND abs(sum(x) by (g) - 4) <= 2
AND sum(abs(x - w)) <= 9
AND abs(max(x * b) - min(x)) <= 3
AND sum(x) >= 2 AND sum(b * x) by (g) >= 1
MINIMIZE sum(abs(x - 2 * w)) + max(x);
SELECT id, b, x FROM t DECIDE b(BOOL), x(INT)
SUCH THAT x >= 0 AND x <= 5
AND abs(sum(x) by (g) - 4) <= 2
AND sum(abs(x - w)) <= 9
AND abs(max(x * b) - min(x)) <= 3
AND sum(x) >= 2 AND sum(b * x) by (g) >= 1
MINIMIZE sum(abs(x - 2 * w)) + max(x);
