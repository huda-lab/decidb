-- An escape confined to one category: only AIR shipments are uncapped, so the edit caps that slice (runaway_+inf with a
-- `group`).
SELECT l_orderkey, l_linenumber, buy
DECIDE buy(REAL)
FROM lineitem
WHERE l_orderkey <= 300
SUCH THAT when l_shipmode <> 'AIR': buy <= 100
MAXIMIZE SUM(buy * l_extendedprice)
