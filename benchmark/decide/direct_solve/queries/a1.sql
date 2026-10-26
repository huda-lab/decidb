-- A1 (separable box: each row takes its best value in its own range).
-- Ship each part offer as close to 5,000 units as its available stock allows.
-- decision: ship
-- objective: SUM(POWER(ship - 5000, 2))
SELECT ps_partkey, ps_suppkey, ship
DECIDE ship(REAL)
FROM partsupp
SUCH THAT ship BETWEEN 0 AND ps_availqty
MINIMIZE SUM(POWER(ship - 5000, 2))
