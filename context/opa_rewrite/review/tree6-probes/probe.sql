SET threads=1;
CREATE TABLE t AS SELECT * FROM (VALUES (1,'a',3.0),(2,'a',5.0),(3,'b',2.0),(4,'b',7.0)) v(id,g,w);
EXPLAIN SELECT id, x, y FROM t DECIDE x(INT), y(REAL)
SUCH THAT x <= 10 AND x >= -10 AND y <= 5 AND y >= -5
 AND norm(x - w, 0) <= 3
 AND sum(abs(x - w)) by (g) + y <= 20
 AND max(abs(y) * abs(y)) <= 30
MINIMIZE sum(abs(y - w)) + sum(x);
SELECT id, x, y FROM t DECIDE x(INT), y(REAL)
SUCH THAT x <= 10 AND x >= -10 AND y <= 5 AND y >= -5
 AND norm(x - w, 0) <= 3
 AND sum(abs(x - w)) by (g) + y <= 20
 AND max(abs(y) * abs(y)) <= 30
MINIMIZE sum(abs(y - w)) + sum(x);
