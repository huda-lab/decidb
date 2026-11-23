SET threads=1;
CREATE TABLE t AS SELECT * FROM (VALUES (1,1,2.0),(2,1,3.0),(3,2,1.5),(4,2,4.0)) t(id,g,w);
EXPLAIN SELECT id, b, x, y FROM t DECIDE b(BOOL), x(INT), y(INT)
SUCH THAT x >= 0 AND x <= 5 AND y >= 0 AND y <= 5 AND max((b + x) * y) >= 4 AND sum((b + x) * y) by (g) + min((b + y) * x) <= 30 AND sum(b) >= 2
MAXIMIZE sum(w * x + y);
SELECT id, b, x, y FROM t DECIDE b(BOOL), x(INT), y(INT)
SUCH THAT x >= 0 AND x <= 5 AND y >= 0 AND y <= 5 AND max((b + x) * y) >= 4 AND sum((b + x) * y) by (g) + min((b + y) * x) <= 30 AND sum(b) >= 2
MAXIMIZE sum(w * x + y);
