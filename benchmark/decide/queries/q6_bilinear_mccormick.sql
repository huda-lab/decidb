-- Q6  Bilinear McCormick, MILP (full orders scale)
-- NOTE: bounded by the orders cardinality (127.5K medium / 255K large), not by solver
--       cost — measured 2026-07-26 at 15.2s / 39.9s. Reaching 500K/1M would require
--       moving off orders, which would change what the query tests.
-- TAGS: type=BOOLEAN,REAL,multiple-vars; class=MILP,bilinear-McCormick;
--       obj=MAXIMIZE-bilinear(b*x); cons=bilinear-constraint; when+per=WHEN+PER-composition
SELECT o_orderkey, o_totalprice, o_orderpriority, pick, boost
FROM (SELECT o_orderkey, o_totalprice, o_orderpriority FROM orders ORDER BY o_orderkey LIMIT ${Q6_ROW_LIMIT}) orders
DECIDE pick(BOOL), boost(REAL)
SUCH THAT boost <= 100
    AND SUM(pick) <= ${Q6_PICK_CAP}
    AND SUM(pick * boost) <= ${Q6_BILIN_CAP}
    AND SUM(pick * boost) <= ${Q6_GRP_CAP} WHEN o_totalprice > 50000 PER o_orderpriority
MAXIMIZE SUM(pick * boost + 0.1 * pick * o_totalprice);
