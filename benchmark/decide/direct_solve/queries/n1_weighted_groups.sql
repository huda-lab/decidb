-- N1 (Euclidean ball with a weight per row, one limit per group).
-- The same problem with a limit for each supplier, over its 80 offers.
-- decision: ship
-- objective: SUM(ps_supplycost * ship)
SELECT ps_partkey, ps_suppkey, ps_supplycost, ship
DECIDE ship(REAL)
FROM partsupp
SUCH THAT SUM(POWER(SQRT(ps_availqty) * ship, 2)) by (ps_suppkey) <= 1000000
MAXIMIZE SUM(ps_supplycost * ship)
