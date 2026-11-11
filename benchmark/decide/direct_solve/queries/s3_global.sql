-- S3 (maximum-cardinality knapsack: as many rows as fit a budget), one budget over every row.
-- Take as many part offers as fit a supply-cost budget of 1,000,000 across all of them. The cheapest k offers cost
-- about 500 * k^2 / n in total, so the budget fits k = sqrt(2,000 * n): half of the table at sf 0.01, a smaller share
-- as it grows, and never all of it.
-- decision: pick
-- objective: SUM(pick)
SELECT ps_partkey, ps_suppkey, ps_supplycost, pick
DECIDE pick(BOOL)
FROM partsupp
SUCH THAT SUM(ps_supplycost * pick) <= 1000000
MAXIMIZE SUM(pick)
