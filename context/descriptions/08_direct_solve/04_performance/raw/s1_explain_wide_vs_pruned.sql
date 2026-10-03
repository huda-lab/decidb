SET decide_direct_solve = 'require';
SET threads = 4;

CREATE TEMP TABLE source AS
SELECT i,
       (((CAST(i AS BIGINT) * 37) % 10007) - 5000)::DOUBLE / 10.0 AS score,
       rpad(i::VARCHAR, 512, 'p') AS payload
FROM range(5000000) t(i);

EXPLAIN ANALYZE SELECT i, score, payload, x FROM (
  FROM source DECIDE x(BOOL)
  SUCH THAT SUM(x) <= 500000
  MAXIMIZE SUM(score * x)
) q;

EXPLAIN ANALYZE SELECT i, score, x FROM (
  FROM source DECIDE x(BOOL)
  SUCH THAT SUM(x) <= 500000
  MAXIMIZE SUM(score * x)
) q;
