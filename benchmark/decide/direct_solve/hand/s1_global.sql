-- Hand-written comparator for s1_global: the tenth of all offers with the most stock. The bound floor(0.1 * count) must
-- not be below the lower bound 0 (the rule's "infeasible" check); the ranking is one window over every row.
SELECT ps_partkey, ps_suppkey, ps_availqty, CASE WHEN rn <= g.k THEN 1 ELSE 0 END AS pick
FROM (SELECT *, row_number() OVER (ORDER BY ps_availqty DESC) AS rn FROM partsupp),
     (SELECT CASE WHEN k >= 0 THEN k ELSE error('infeasible') END AS k
      FROM (SELECT floor(0.1 * count(*))::BIGINT AS k FROM partsupp)) g
