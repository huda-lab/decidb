SET threads=1;
CREATE TABLE t AS SELECT * FROM (VALUES (1,1,2.0),(2,1,3.0),(3,2,1.5),(4,2,4.0)) t(id,g,w);
EXPLAIN SELECT id, b, x, y FROM t DECIDE b(BOOL), x(INT), y(INT)
SUCH THAT x >= 0 AND x <= 5 AND y >= 0 AND y <= 5 AND max((x + 1) * (y + 2)) >= 12 AND max(b * x) + sum(b * y * 2) <= 9 AND min(b * (x + y)) by (g) >= 0 AND sum(b) >= 2
MAXIMIZE sum(w * x + y) - max(b * y);
SELECT id, b, x, y FROM t DECIDE b(BOOL), x(INT), y(INT)
SUCH THAT x >= 0 AND x <= 5 AND y >= 0 AND y <= 5 AND max((x + 1) * (y + 2)) >= 12 AND max(b * x) + sum(b * y * 2) <= 9 AND min(b * (x + y)) by (g) >= 0 AND sum(b) >= 2
MAXIMIZE sum(w * x + y) - max(b * y);
