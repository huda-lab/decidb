-- N1 (Euclidean ball with a weight per row: the ellipsoid support function).
-- Ship along each offer's cost, under one limit on the sum of squared amounts, each weighted by the offer's stock.
-- decision: ship
-- objective: SUM(ps_supplycost * ship)
SELECT ps_partkey, ps_suppkey, ps_supplycost, ship
DECIDE ship(REAL)
FROM partsupp
SUCH THAT SUM(POWER(SQRT(ps_availqty) * ship, 2)) <= 1000000
MAXIMIZE SUM(ps_supplycost * ship)
