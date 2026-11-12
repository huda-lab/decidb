-- S2 (laminar matroid: nested count quotas over one Boolean decision).
-- Each nation takes at most 2 customers per market segment and at most 5 in all, and the whole book at most 100;
-- keep the customers with the highest account balance. At sf 1 a (nation, segment) group has about 1,200 customers
-- (cap 2), the 5 groups of a nation would take 10 (cap 5), and the 25 nations would take 125 (cap 100), so every
-- level binds.
-- decision: pick
-- objective: SUM(c_acctbal * pick)
SELECT c_custkey, c_nationkey, c_mktsegment, c_acctbal, pick
DECIDE pick(BOOL)
FROM customer
SUCH THAT SUM(pick) by (c_nationkey, c_mktsegment) <= 2
      AND SUM(pick) by (c_nationkey) <= 5
      AND SUM(pick) <= 100
MAXIMIZE SUM(c_acctbal * pick)
