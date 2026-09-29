-- P3  Coupled convex QP: P2 plus one aggregate budget row tying every decision together
-- TAGS: type=REAL; class=QP-convex; obj=MINIMIZE-quadratic; cons=per-row-bound,aggregate-budget
SELECT l_orderkey, l_linenumber, l_quantity, amount
FROM lineitem
DECIDE amount(REAL)
SUCH THAT amount <= 30
    AND SUM(amount) <= ${P3_BUDGET}
MINIMIZE SUM(POWER(amount - l_quantity, 2));
