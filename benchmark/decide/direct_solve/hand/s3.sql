-- Hand-written comparator for s3: each supplier takes as many offers as fit a cost budget of 5,000, cheapest first. The
-- rule sorts a group's rows by weight in one window holding the running total, then takes k, the group's largest rank
-- whose total fits, and selects rank <= k. Here the one window is the whole answer: a TPC-H cost is at least 1.00, so a
-- running total only rises and a row's prefix fits exactly when its own total does, with no rank and no k. The total
-- is a DOUBLE sum over a ROWS frame, compared with the budget as the rule compares it (a DECIMAL sum would be exact).
-- Tied costs sit in any order and give the same count and the same totals. Left out because the data has nothing to
-- raise: a constant budget of 0 or more fits the empty prefix (no infeasible check), costs are finite (no NaN or
-- infinity check), a total of at most 80 * 1,000 does not pass a DOUBLE, and no cost is NULL or negative.
SELECT ps_partkey, ps_suppkey, ps_supplycost,
       CASE WHEN total <= 5000.0 THEN 1 ELSE 0 END AS pick
FROM (SELECT *, sum(ps_supplycost::DOUBLE) OVER (PARTITION BY ps_suppkey ORDER BY ps_supplycost
                                                  ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS total
      FROM partsupp)
