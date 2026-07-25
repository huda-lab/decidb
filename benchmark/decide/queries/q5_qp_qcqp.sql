-- Q5  Convex QP + QCQP (moderate scale: QCQP barrier cost grows with rows, Gurobi)
-- TAGS: type=REAL; class=QP-convex,QCQP; obj=MINIMIZE-quadratic,mixed-linear+quadratic;
--       func=POWER,norm-L2; cons=aggregate-quadratic-constraint
-- NOTE: norm-L2 (F10) rides in the objective (= squared deviation, the natural QP form);
--       as a <= constraint it drove Gurobi to a suboptimal status. The QCQP class comes
--       from the plain SUM(POWER(qty,2)) <= K constraint.
SELECT l_orderkey, l_linenumber, l_quantity, qty
FROM (SELECT l_orderkey, l_linenumber, l_quantity FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT ${Q5_ROW_LIMIT}) lineitem
DECIDE qty IS REAL
SUCH THAT qty >= 0
    AND qty <= 50
    AND SUM(POWER(qty, 2)) <= ${Q5_SSE_CAP}
MINIMIZE norm(qty - l_quantity, 2) + 0.01 * SUM(qty);
