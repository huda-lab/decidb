-- P2  Separable convex QP: no constraint couples the rows, so each one minimises alone
-- TAGS: type=REAL; class=QP-convex; obj=MINIMIZE-quadratic; cons=per-row-bound
SELECT l_orderkey, l_linenumber, l_quantity, amount
FROM lineitem
DECIDE amount(REAL)
SUCH THAT amount <= 30
MINIMIZE SUM(POWER(amount - l_quantity, 2));
