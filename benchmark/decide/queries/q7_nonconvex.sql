-- Q7  Non-convex bundle (small scale, Gurobi NonConvex=2)
-- TAGS: type=BOOLEAN,REAL,multiple-vars; class=MIQP,QP-nonconvex,bilinear-nonconvex;
--       obj=MAXIMIZE-POWER(nonconvex); cons=per-row-quadratic-constraint,nonconvex-bilinear-constraint
SELECT o_orderkey, o_totalprice, pick, x, y
FROM (SELECT o_orderkey, o_totalprice FROM orders ORDER BY o_orderkey LIMIT ${Q7_ROW_LIMIT}) orders
DECIDE pick IS BOOLEAN, x IS REAL, y IS REAL
SUCH THAT x <= 10
    AND y <= 10
    AND POWER(x - 5, 2) <= 16
    AND SUM(x * y) <= ${Q7_BILIN_CAP}
    AND SUM(pick) <= ${Q7_PICK_CAP}
MAXIMIZE SUM(POWER(x - 5, 2)) + 0.0001 * SUM(pick * o_totalprice);
