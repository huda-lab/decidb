-- R4 (weighted max-min fair allocation, one budget per group).
-- The same problem with a budget for each supplier, over its 80 offers.
-- decision: ship
-- objective: MIN(ship / ps_availqty)
SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, ship
DECIDE ship(REAL)
FROM partsupp
SUCH THAT ship <= 2 * ps_availqty
AND SUM(ps_supplycost * ship) by (ps_suppkey) <= 100000000
MAXIMIZE MIN(ship / ps_availqty)
