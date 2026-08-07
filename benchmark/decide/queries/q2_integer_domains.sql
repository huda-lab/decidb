-- Q2  Integer domains & disjunctions (large scale)
-- TAGS: type=INTEGER-explicit,default-type,multiple-vars; class=ILP; obj=MAXIMIZE-SUM;
--       cons=BETWEEN,IN-var,<>-per-row,<>-aggregate,division,per-row-linear-LHS,unary-minus,
--            strict->,uncorrelated-subquery,correlated-subquery,data-only-aggregate-RHS,data-only-op-fold(%)
SELECT o_orderkey, o_custkey, o_totalprice, o_orderpriority, n, units
FROM orders
DECIDE n(INT), units(INT)
SUCH THAT n BETWEEN 0 AND 3
    AND n IN (0, 1, 3)
    AND n <> 2
    AND -n <= 0
    AND n / 2 <= 2
    AND units <= 5
    AND SUM(units) > 100
    AND SUM(n) <> ${Q2_NE_SUM}
    AND SUM(n * o_totalprice) <= ${Q2_PRICE_CAP}
    AND SUM((o_orderkey % 7) * n) <= ${Q2_MOD_CAP}
    AND n <= (SELECT max(o_shippriority) + 3 FROM orders)
    AND n <= (SELECT c_nationkey FROM customer WHERE c_custkey = o_custkey)
    AND SUM(n * o_totalprice) <= SUM(o_totalprice)
MAXIMIZE SUM(n * o_totalprice + units * 10);
