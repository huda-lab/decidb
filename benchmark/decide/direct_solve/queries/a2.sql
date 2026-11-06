-- A2 (single-facility location: one shared value closest to every row's value).
-- Pick one reference price closest, in squared distance, to every offer's supply cost.
-- decision: center
-- objective: SUM(POWER(center - ps_supplycost, 2))
SELECT ps_partkey, ps_suppkey, ps_supplycost, center
DECIDE per (): center(REAL)
FROM partsupp
SUCH THAT center BETWEEN 0 AND 2000
MINIMIZE SUM(POWER(center - ps_supplycost, 2))
