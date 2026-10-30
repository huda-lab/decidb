-- Direct-solve class S1 (choose K rows by score), made infeasible: two offers per supplier cannot fill 100,000 picks,
-- more than there are offers.
SELECT ps_partkey, ps_suppkey, pick
DECIDE pick(BOOL)
FROM partsupp
SUCH THAT SUM(pick) by (ps_suppkey) <= 2 AND SUM(pick) >= 100000
MAXIMIZE SUM(ps_availqty * pick)
