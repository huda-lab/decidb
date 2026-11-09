-- Hand-written comparator for s1: each supplier keeps the two offers with the most stock. A bound of 2 is feasible
-- (0 <= 2), so the rule's check is a constant here; the ranking is one window.
SELECT ps_partkey, ps_suppkey, ps_availqty, CASE WHEN rn <= 2 THEN 1 ELSE 0 END AS pick
FROM (SELECT *, row_number() OVER (PARTITION BY ps_suppkey ORDER BY ps_availqty DESC) AS rn FROM partsupp)
