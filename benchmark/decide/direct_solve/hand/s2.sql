-- Hand-written comparator for s2: each nation takes at most 2 customers per segment, at most 5 per nation, and at most
-- 100 overall, highest balance first. The rule ranks each level over the survivors of the finer one and chooses the
-- survivors of the last level whose score improves the objective. Here each stage is one window that orders the
-- previous stage's survivors first, so a row's rank is its rank among them, and keeps rank <= cap among survivors; the
-- balance must be positive to be taken. Constant caps of 2, 5 and 100 are never below 0 (no infeasible check), and a
-- TPC-H balance is finite and not NULL (no score check). Tied balances sit in any order and give the same objective.
SELECT c_custkey, c_nationkey, c_mktsegment, c_acctbal, CASE WHEN k2 AND rn3 <= 100 THEN 1 ELSE 0 END AS pick
FROM (SELECT *, row_number() OVER (ORDER BY k2 DESC, c_acctbal DESC) AS rn3
      FROM (SELECT *, k1 AND row_number() OVER (PARTITION BY c_nationkey ORDER BY k1 DESC, c_acctbal DESC) <= 5 AS k2
            FROM (SELECT *, c_acctbal > 0 AND row_number() OVER (PARTITION BY c_nationkey, c_mktsegment
                                                                  ORDER BY c_acctbal DESC) <= 2 AS k1
                  FROM customer)))
