-- S3 (maximum-cardinality knapsack: as many rows as fit a budget).
-- Each supplier takes as many of its part offers as fit a supply-cost budget of 5,000. Offers cost 1 to 1,000 and each
-- supplier has 80, so a budget of 5,000 fits its 23 to 35 cheapest offers at the sizes checked.
-- decision: pick
-- objective: SUM(pick)
SELECT ps_partkey, ps_suppkey, ps_supplycost, pick
DECIDE pick(BOOL)
FROM partsupp
SUCH THAT SUM(ps_supplycost * pick) by (ps_suppkey) <= 5000
MAXIMIZE SUM(pick)
