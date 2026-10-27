-- R1 (continuous knapsack: one budget per group over REAL decisions).
-- A1 with a cap: each part takes at most 5,000 units in total across its suppliers.
-- decision: ship
-- objective: SUM(ps_supplycost * ship)
SELECT ps_partkey, ps_suppkey, ps_supplycost, ship
DECIDE ship(REAL)
FROM partsupp
SUCH THAT ship BETWEEN 0 AND ps_availqty AND SUM(ship) by (ps_partkey) <= 5000
MAXIMIZE SUM(ps_supplycost * ship)
