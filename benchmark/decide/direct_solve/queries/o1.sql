-- O1 (Monge assignment: a one-to-one matching of two sides by position).
-- Pair every supplier with one partner supplier so that strong account balances back weak ones: the least total
-- product of the paired balances, so the richest pairs with the poorest.
-- decision: x
-- objective: SUM(left_balance * right_balance * x)
SELECT l.s_suppkey AS left_supplier, l.s_acctbal AS left_balance, r.s_suppkey AS right_supplier,
       r.s_acctbal AS right_balance, x
FROM supplier l CROSS JOIN supplier r
DECIDE x(BOOL)
SUCH THAT per l: SUM(x) by (l) = 1
AND per r: SUM(x) by (r) = 1
MINIMIZE SUM(l.s_acctbal * r.s_acctbal * x)
