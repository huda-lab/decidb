-- EXEC-25: a coefficient that fails on every row over 200,000 rows. Measured 4.6 s to print the error; plain SQL fails
-- in milliseconds (`SELECT sum(ln(w - 10)) FROM t`).
-- Time: /usr/bin/time -l build/release/decidb :memory: < this file.
SET decide_direct_solve = 'off';
CREATE TABLE t AS SELECT i AS id, (i % 1000) AS g, (i % 5)::DOUBLE AS w FROM range(200000) r(i);
SELECT sum(x) FROM (
  SELECT id, x FROM t DECIDE x(REAL) SUCH THAT x <= 1 AND sum(ln(w - 10) * x) <= 10 MAXIMIZE sum(x));
