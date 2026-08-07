-- Q9  Hard-direction MIN/MAX objective: MAXIMIZE MAX (row-limited: Big-M binaries scale with rows)
-- NOTE: the least scalable query in the suite — the hard-MAX indicators couple globally, so
--       cost is sharply superlinear. Measured 2026-07-26: 5K 1.7s, 7.5K 5.3s, 15K 29.8s,
--       30K >60s. 15K (large) is the practical ceiling; do not raise without re-measuring.
-- TAGS: type=BOOLEAN; class=ILP; obj=MAXIMIZE-MAX(hard,Big-M); func=MAX; cons=equality(=)
SELECT l_orderkey, l_linenumber, l_extendedprice, keep
FROM (SELECT l_orderkey, l_linenumber, l_extendedprice FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT ${Q9_ROW_LIMIT}) lineitem
DECIDE keep(BOOL)
SUCH THAT SUM(keep) = 100
    AND SUM(keep * l_extendedprice) <= 5000000
MAXIMIZE MAX(keep * l_extendedprice);
