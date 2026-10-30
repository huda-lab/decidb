-- per_group in the expanded slack scope: one edit per order that falls short, named by its key (expanded_group).
SET diagnose_decide_infeasible_slack_scope = 'expanded';
SELECT l_orderkey, l_linenumber, x
DECIDE x(BOOL)
FROM lineitem
WHERE l_orderkey <= 40
SUCH THAT per l_orderkey: SUM(x) by (l_orderkey) >= 5
MAXIMIZE SUM(x)
