-- Hand-written comparator for n1_groups: n1 with one ball for each supplier, joined back to the rows by the key. Every
-- obligation of n1 holds per group.
WITH g AS (
    SELECT ps_suppkey, sqrt(h) AS length, h = 0.0 AS still, abs(h) < 2.2250738585072014e-308 AS below,
           CASE WHEN valid THEN CASE WHEN isfinite(h) THEN true
                                     ELSE error('computes a number too large for a DOUBLE')::BOOLEAN END
                ELSE false END AS ok
    FROM (SELECT ps_suppkey, sum(square) AS h,
                 bool_and(CASE WHEN isfinite(value) AND isfinite(square) THEN true
                               ELSE error('computes a number too large for a DOUBLE')::BOOLEAN END) AS valid
          FROM (SELECT ps_suppkey, value, value * value AS square
                FROM (SELECT ps_suppkey, greatest(ps_supplycost::DOUBLE, 0.0) AS value FROM partsupp))
          GROUP BY ps_suppkey)
)
SELECT ps_partkey, ps_suppkey, ps_supplycost,
       CASE WHEN g.still THEN 0.0 ELSE greatest(ps_supplycost::DOUBLE, 0.0) / g.length * 1000.0 END + 0.0 AS ship
FROM partsupp JOIN g USING (ps_suppkey)
WHERE g.ok AND CASE WHEN g.below AND ps_supplycost::DOUBLE <> 0.0 THEN __internal_decide_warn_numbers() ELSE true END
