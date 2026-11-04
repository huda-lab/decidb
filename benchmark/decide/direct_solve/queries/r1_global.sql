-- R1 (continuous knapsack: one budget over REAL decisions), the form of the paper's listing.
-- Take a fraction of each part offer, spending at most a tenth of the total cost, to secure the most stock.
-- decision: ship
-- objective: SUM(ps_availqty * ship)
SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, ship
DECIDE ship(REAL)
FROM partsupp
SUCH THAT ship BETWEEN 0 AND 1 AND SUM(ship * ps_supplycost) <= 0.1 * SUM(ps_supplycost)
MAXIMIZE SUM(ps_availqty * ship)
