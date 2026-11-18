-- CU-10: one failing `when` over 200,000 rows. Measured 3.1 s and 264 MB to print the error.
-- The same predicate in a WHERE fails in 0.02 s (second statement).
-- Time each: /usr/bin/time -l build/release/decidb :memory: < this file, with one statement commented out.
SET decide_direct_solve = 'off';
CREATE TABLE t AS SELECT i AS id, (i % 1000) AS g, (i % 5)::DOUBLE AS w FROM range(200000) r(i);
SELECT sum(x) FROM (
  SELECT id, x FROM t DECIDE x(REAL) SUCH THAT x <= 1 AND when ln(w - 10) > 0: sum(x) <= 10 MAXIMIZE sum(x));
SELECT count(*) FROM t WHERE ln(w - 10) > 0;
