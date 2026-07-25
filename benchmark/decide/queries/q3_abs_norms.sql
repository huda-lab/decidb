-- Q3  ABS & norms, signed domain (moderate scale: L0 adds one binary per row)
-- TAGS: type=REAL,signed-domain; class=MILP; obj=MINIMIZE;
--       func=ABS-PathA(<=),ABS-in-MINIMIZE,norm-L1,norm-Linf,norm-L0
-- NOTE: L0 (a covering-type MILP) and the ABS Big-M hard-direction (>=) explode when
--       combined, so the ABS Path-B constraint lives in Q10 instead. Keep L0 alone here.
SELECT l_orderkey, l_linenumber, l_quantity, adj
FROM (SELECT l_orderkey, l_linenumber, l_quantity FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT ${Q3_ROW_LIMIT}) lineitem
DECIDE adj IS REAL
SUCH THAT adj BETWEEN -20 AND 20
    AND SUM(ABS(adj)) <= ${Q3_ABS_CAP}
    AND norm(adj, 'inf') <= 15
MINIMIZE SUM(ABS(adj - 0.01 * l_quantity)) + 0.5 * norm(adj, 1) + 0.3 * norm(adj, 0, 40);
