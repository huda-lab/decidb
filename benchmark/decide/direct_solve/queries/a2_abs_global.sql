-- A2 (single-facility location) under the absolute loss: one query-wide median.
-- Pick one reference price closest in absolute distance to every offer's supply cost.
-- decision: center
-- objective: SUM(ABS(center - ps_supplycost))
SELECT ps_partkey, ps_suppkey, ps_supplycost, center
DECIDE per (): center(REAL)
FROM partsupp
SUCH THAT center BETWEEN 0 AND 2000
MINIMIZE SUM(ABS(center - ps_supplycost))
