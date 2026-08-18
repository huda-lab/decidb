-- Q9  Hard-direction MIN/MAX objective: MAXIMIZE MAX (row-limited, pending a re-tier)
-- NOTE: was the least scalable query in the suite until 2026-08-18, when the MIN/MAX
--       auxiliary stopped being declared free and got its derived box. The old curve
--       (5K 1.7s, 15K 29.8s, 30K >60s) was the root simplex crawling without a box, not
--       the hard-MAX indicators. Re-measured boxed: 15K 0.24s, 30K 0.45s, 60K 0.95s,
--       120K 2.8s. The limit below is still the old one — raising it is a suite-wide
--       tiering decision, not a measurement question any more.
-- TAGS: type=BOOLEAN; class=ILP; obj=MAXIMIZE-MAX(hard,Big-M); func=MAX; cons=equality(=)
SELECT l_orderkey, l_linenumber, l_extendedprice, keep
FROM (SELECT l_orderkey, l_linenumber, l_extendedprice FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT ${Q9_ROW_LIMIT}) lineitem
DECIDE keep(BOOL)
SUCH THAT SUM(keep) = 100
    AND SUM(keep * l_extendedprice) <= 5000000
MAXIMIZE MAX(keep * l_extendedprice);
