-- N1 (Euclidean ball: one limit on a sum of squares per group, over REAL decisions).
-- The same problem with a limit for each supplier, over its 80 offers.
-- decision: ship
-- objective: SUM(ps_supplycost * ship)
SELECT ps_partkey, ps_suppkey, ps_supplycost, ship
DECIDE ship(REAL)
FROM partsupp
SUCH THAT SUM(POWER(ship, 2)) by (ps_suppkey) <= 1000000
MAXIMIZE SUM(ps_supplycost * ship)
