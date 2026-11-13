-- R4 (weighted max-min fair allocation, one budget).
-- Each offer receives a share of its available quantity, at most twice it; one budget on the total cost of what is
-- shipped caps the common share.
-- decision: ship
-- objective: MIN(ship / ps_availqty)
SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, ship
DECIDE ship(REAL)
FROM partsupp
SUCH THAT ship <= 2 * ps_availqty
AND SUM(ps_supplycost * ship) <= 1000000000000
MAXIMIZE MIN(ship / ps_availqty)
