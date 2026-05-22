-- Basic EXPLAIN
-- EXPLAIN SELECT o_orderkey, o_totalprice, x
-- FROM orders
-- WHERE o_orderdate >= '1995-01-01' AND o_orderdate < '1995-02-01'
-- DECIDE x IS BOOLEAN
-- SUCH THAT SUM(x) <= 10
-- MAXIMIZE SUM(x * o_totalprice);

-- -- EXPLAIN ANALYZE (runs the query, shows timing)
EXPLAIN ANALYZE SELECT o_orderkey, o_totalprice, x
FROM orders
WHERE o_orderdate >= '1995-01-01' AND o_orderdate < '1995-02-01'
DECIDE x IS BOOLEAN
SUCH THAT SUM(x) <= 10
MAXIMIZE SUM(x * o_totalprice);

-- -- WHEN clause
-- EXPLAIN SELECT l_orderkey, l_extendedprice, x
-- FROM lineitem WHERE l_orderkey < 100
-- DECIDE x IS BOOLEAN
-- SUCH THAT SUM(x * l_quantity) <= 50 WHEN l_returnflag = 'R'
-- MAXIMIZE SUM(x * l_extendedprice);

-- -- PER clause
-- EXPLAIN SELECT s_suppkey, s_acctbal, x
-- FROM supplier
-- DECIDE x IS BOOLEAN
-- SUCH THAT SUM(x) <= 5 PER s_nationkey
-- MAXIMIZE SUM(x * s_acctbal);

-- -- Multiple variables
-- EXPLAIN SELECT l_orderkey, x, y
-- FROM lineitem WHERE l_orderkey < 50
-- DECIDE x IS BOOLEAN, y IS INTEGER
-- SUCH THAT SUM(x * l_quantity) <= 50 AND y <= 3 AND SUM(y) <= 10
-- MAXIMIZE SUM(x * l_extendedprice + y);
