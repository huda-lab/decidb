-- Hand-written comparator for s3_global: as many offers as fit a cost budget of 1,000,000 across every row, cheapest
-- first. One window over every row holds the running total, and an offer is taken when its total fits (see s3 for why a
-- rank and k are not needed, how ties and the DOUBLE total behave, and which of the rule's checks have nothing to raise
-- on this data).
SELECT ps_partkey, ps_suppkey, ps_supplycost,
       CASE WHEN total <= 1000000.0 THEN 1 ELSE 0 END AS pick
FROM (SELECT *, sum(ps_supplycost::DOUBLE) OVER (ORDER BY ps_supplycost
                                                  ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS total
      FROM partsupp)
