-- Q6  Bilinear McCormick, MILP (moderate scale, row-limited)
-- TAGS: type=BOOLEAN,REAL,multiple-vars; class=MILP,bilinear-McCormick;
--       obj=MAXIMIZE-bilinear(b*x); cons=bilinear-constraint; when+per=WHEN+PER-composition
SELECT o_orderkey, o_totalprice, o_orderpriority, pick, boost
FROM (SELECT o_orderkey, o_totalprice, o_orderpriority FROM orders ORDER BY o_orderkey LIMIT ${Q6_ROW_LIMIT}) orders
DECIDE pick IS BOOLEAN, boost IS REAL
SUCH THAT boost <= 100
    AND SUM(pick) <= ${Q6_PICK_CAP}
    AND SUM(pick * boost) <= ${Q6_BILIN_CAP}
    AND SUM(pick * boost) <= ${Q6_GRP_CAP} WHEN o_totalprice > 50000 PER o_orderpriority
MAXIMIZE SUM(pick * boost + 0.1 * pick * o_totalprice);
