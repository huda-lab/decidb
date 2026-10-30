-- A query that solves: DIAGNOSE returns one row, state = 'feasible'.
-- Each supplier fills at most two of its part offers; keep the offers with the most stock available.
SELECT ps_partkey, ps_suppkey, pick
DECIDE pick(BOOL)
FROM partsupp
SUCH THAT SUM(pick) by (ps_suppkey) <= 2
MAXIMIZE SUM(ps_availqty * pick)
