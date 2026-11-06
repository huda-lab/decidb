-- A2 (single-facility location, keyed: one shared value per class of a key).
-- The same problem with one reference price for each supplier, over its 80 offers.
-- decision: center
-- objective: SUM(POWER(center - ps_supplycost, 2))
SELECT ps_partkey, ps_suppkey, ps_supplycost, center
DECIDE per ps_suppkey: center(REAL)
FROM partsupp
SUCH THAT center BETWEEN 0 AND 2000
MINIMIZE SUM(POWER(center - ps_supplycost, 2))
