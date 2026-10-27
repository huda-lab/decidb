-- S1 (uniform matroid: choose K rows by score).
-- Each supplier fills at most two of its part offers; keep the offers with the most stock available.
-- decision: pick
-- objective: SUM(ps_availqty * pick)
SELECT ps_partkey, ps_suppkey, ps_availqty, pick
DECIDE pick(BOOL)
FROM partsupp
SUCH THAT SUM(pick) by (ps_suppkey) <= 2
MAXIMIZE SUM(ps_availqty * pick)
