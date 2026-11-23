SET threads=1;
CREATE TABLE t AS SELECT * FROM (VALUES (1,1,1,2.0),(2,1,2,3.0),(3,1,3,1.5),(4,2,1,4.0),(5,2,2,2.5),(6,2,3,1.0)) t(id,g,period,w);
EXPLAIN SELECT id, x, y FROM t DECIDE x(INT), y(INT)
SUCH THAT x >= 0 AND x <= 6 AND y >= 0 AND y <= 6
AND abs(x - at(previous else 0: x) over (t.period within (t.g))) <= 2
AND max(from 1 previous to 1 next: x) over (t.period within (t.g)) <= 5
AND sum(abs(y - at(previous else 1: y) over (t.period within (t.g)))) by (g) <= 4
AND x + y <= coalesce(at(previous: t.w) over (t.period within (t.g)), 3) + 4
MAXIMIZE sum(w * x + y);
SELECT id, x, y FROM t DECIDE x(INT), y(INT)
SUCH THAT x >= 0 AND x <= 6 AND y >= 0 AND y <= 6
AND abs(x - at(previous else 0: x) over (t.period within (t.g))) <= 2
AND max(from 1 previous to 1 next: x) over (t.period within (t.g)) <= 5
AND sum(abs(y - at(previous else 1: y) over (t.period within (t.g)))) by (g) <= 4
AND x + y <= coalesce(at(previous: t.w) over (t.period within (t.g)), 3) + 4
MAXIMIZE sum(w * x + y);
