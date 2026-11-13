-- R4 (weighted max-min fair allocation, a decision shared by the rows of a key).
-- r4 with one shipped share for each part, over its 4 offers.
-- decision: ship
-- objective: MIN(ship / ps_availqty)
SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, ship
DECIDE per ps_partkey: ship(REAL)
FROM partsupp
SUCH THAT ship <= 2 * ps_availqty
AND SUM(ps_supplycost * ship) <= 1000000000000
MAXIMIZE MIN(ship / ps_availqty)
