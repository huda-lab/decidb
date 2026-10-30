-- A spending cap no selection of 30 line items fits: the cap is the clause to loosen (source_literal).
SELECT l_orderkey, l_linenumber, buy
DECIDE buy(BOOL)
FROM lineitem
WHERE l_orderkey <= 100
SUCH THAT SUM(buy) >= 30 AND SUM(buy * l_extendedprice) <= 100
MAXIMIZE SUM(buy)
