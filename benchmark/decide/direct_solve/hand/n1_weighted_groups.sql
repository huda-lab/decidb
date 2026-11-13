-- Hand-written comparator for n1_weighted_groups: n1_weighted with one ball for each supplier, joined back to the rows
-- by the key. Every obligation of n1_weighted holds per group.
WITH r AS (
    SELECT *, CASE WHEN q > 0.0 THEN value / q END AS point
    FROM (SELECT ps_partkey, ps_suppkey, ps_supplycost, slope, 1.0 * (slope * slope) AS q,
                 greatest(ps_supplycost::DOUBLE, 0.0) AS value
          FROM (SELECT *, sqrt(ps_availqty) AS slope FROM partsupp))
), g AS (
    SELECT ps_suppkey, sqrt(h) AS length, h = 0.0 AS still, abs(h) < 2.2250738585072014e-308 AS below,
           CASE WHEN valid THEN CASE WHEN isfinite(h) THEN true
                                     ELSE error('computes a number too large for a DOUBLE')::BOOLEAN END
                ELSE false END AS ok
    FROM (SELECT ps_suppkey, sum(share) AS h,
                 bool_and(CASE WHEN isfinite(slope) AND isfinite(q) AND isfinite(value)
                                    AND (share IS NULL OR isfinite(share)) THEN true
                               ELSE error('computes a number too large for a DOUBLE')::BOOLEAN END
                          AND CASE WHEN q > 0.0 OR value = 0.0 THEN true
                                   ELSE error('optimization is unbounded')::BOOLEAN END) AS valid
          FROM (SELECT ps_suppkey, slope, q, value, CASE WHEN q > 0.0 THEN (q * point) * point END AS share FROM r)
          GROUP BY ps_suppkey)
)
SELECT ps_partkey, ps_suppkey, ps_supplycost,
       CASE WHEN g.still OR point IS NULL THEN 0.0 ELSE point / g.length * 1000.0 END + 0.0 AS ship
FROM r JOIN g USING (ps_suppkey)
WHERE g.ok AND CASE WHEN g.below AND value <> 0.0 THEN __internal_decide_warn_numbers() ELSE true END
