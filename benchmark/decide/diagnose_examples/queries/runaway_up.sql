-- An unbounded query: every row's `buy` grows without bound (runaway_+inf, counted per row).
SELECT l_orderkey, l_linenumber, buy
DECIDE buy(REAL)
FROM lineitem
WHERE l_orderkey <= 1000
SUCH THAT buy >= 0
MAXIMIZE SUM(buy * l_extendedprice)
