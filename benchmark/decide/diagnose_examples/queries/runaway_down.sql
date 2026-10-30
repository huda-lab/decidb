-- A signed decision falling without bound: `buy >= -1e30` is no floor, and MINIMIZE drives `buy` down (the case
-- runaway_-inf names).
SELECT l_orderkey, l_linenumber, buy
DECIDE buy(REAL)
FROM lineitem
WHERE l_orderkey <= 100
SUCH THAT buy >= -1e30 AND buy <= 100
MINIMIZE SUM(buy * l_extendedprice)
