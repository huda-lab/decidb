-- N1 (Euclidean ball: one limit on a sum of squares over REAL decisions).
-- Ship along each offer's cost, under one limit on the sum of squared amounts over every offer.
-- decision: ship
-- objective: SUM(ps_supplycost * ship)
SELECT ps_partkey, ps_suppkey, ps_supplycost, ship
DECIDE ship(REAL)
FROM partsupp
SUCH THAT SUM(POWER(ship, 2)) <= 1000000
MAXIMIZE SUM(ps_supplycost * ship)
