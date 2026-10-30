-- Direct-solve class R1 (one budget per group), made infeasible: four suppliers at 10 units each exceed a part's
-- budget of 20.
SELECT ps_partkey, ps_suppkey, ship
DECIDE ship(REAL)
FROM partsupp
SUCH THAT ship BETWEEN 10 AND ps_availqty AND SUM(ship) by (ps_partkey) <= 20
MAXIMIZE SUM(ps_supplycost * ship)
