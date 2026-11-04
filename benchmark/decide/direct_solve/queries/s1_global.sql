-- S1 (uniform matroid: choose K rows by score), one bound over every row.
-- Keep the tenth of all part offers with the most stock available.
-- decision: pick
-- objective: SUM(ps_availqty * pick)
SELECT ps_partkey, ps_suppkey, ps_availqty, pick
DECIDE pick(BOOL)
FROM partsupp
SUCH THAT SUM(pick) <= 0.1 * COUNT(*)
MAXIMIZE SUM(ps_availqty * pick)
