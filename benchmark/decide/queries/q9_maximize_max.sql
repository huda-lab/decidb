-- Q9  Hard-direction MIN/MAX objective: MAXIMIZE MAX (row-limited: Big-M binaries scale with rows)
-- TAGS: type=BOOLEAN; class=ILP; obj=MAXIMIZE-MAX(hard,Big-M); func=MAX; cons=equality(=)
SELECT l_orderkey, l_linenumber, l_extendedprice, keep
FROM (SELECT l_orderkey, l_linenumber, l_extendedprice FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT ${Q9_ROW_LIMIT}) lineitem
DECIDE keep IS BOOLEAN
SUCH THAT SUM(keep) = 100
    AND SUM(keep * l_extendedprice) <= 5000000
MAXIMIZE MAX(keep * l_extendedprice);
