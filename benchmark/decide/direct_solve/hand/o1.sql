-- Hand-written comparator for o1: each side ranked by balance, the left ascending and the right descending, and the pair
-- of equal ranks selected. Every supplier row is distinct and both sides are the same table, so the rule's distinct
-- classes, its one-copy window and its class-count check have nothing to do here; every product of two DECIMAL balances
-- is finite, so its coefficient check has nothing to raise.
WITH l AS (SELECT s_suppkey, s_acctbal, row_number() OVER (ORDER BY s_acctbal) AS rk FROM supplier),
     r AS (SELECT s_suppkey, s_acctbal, row_number() OVER (ORDER BY s_acctbal DESC) AS rk FROM supplier)
SELECT l.s_suppkey AS left_supplier, l.s_acctbal AS left_balance, r.s_suppkey AS right_supplier,
       r.s_acctbal AS right_balance, (l.rk = r.rk)::INTEGER AS x
FROM l CROSS JOIN r
