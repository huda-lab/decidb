-- data_bound in the expanded slack scope: one edit per kept row, each with its own overshoot (expanded_row).
SET diagnose_decide_infeasible_slack_scope = 'expanded';
SELECT l_orderkey, l_linenumber, x
DECIDE x(BOOL)
FROM lineitem
WHERE l_orderkey <= 3
SUCH THAT x >= l_quantity
MAXIMIZE SUM(x)
