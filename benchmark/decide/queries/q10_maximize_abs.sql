-- Q10  MAXIMIZE ABS + ABS Path-B constraint (Big-M; row-limited: sign binaries scale with rows)
-- TAGS: type=REAL; class=MILP; obj=MAXIMIZE-SUM(ABS);
--       func=ABS-in-MAXIMIZE(Big-M),ABS-PathB(Big-M >= constraint)
SELECT l_orderkey, l_linenumber, l_quantity, adj
FROM (SELECT l_orderkey, l_linenumber, l_quantity FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT ${Q10_ROW_LIMIT}) lineitem
DECIDE adj IS REAL
SUCH THAT adj BETWEEN 0 AND 30
    AND SUM(adj) <= ${Q10_SUM_CAP}
    AND SUM(ABS(adj - 15)) >= ${Q10_ABS_FLOOR}
MAXIMIZE SUM(ABS(adj - l_quantity));
