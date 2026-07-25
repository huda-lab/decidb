-- Q7  Non-convex bundle (full orders scale, Gurobi NonConvex=2)
-- NOTE: previously pinned to 1024 rows on the assumption that NonConvex=2 explodes with
--       row count. Measured 2026-07-26: it does not — the non-convex structure here is
--       per-row independent, so cost stays near-linear (127.5K rows: 3.3s, 255K: 7.1s).
--       Now runs the full orders table; the ROW_LIMIT is retained as a no-op knob.
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
