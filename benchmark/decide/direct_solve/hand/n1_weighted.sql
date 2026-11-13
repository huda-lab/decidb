-- Hand-written comparator for n1_weighted: n1 with the factor q = SQRT(ps_availqty)^2 of each row. The row's point is its
-- direction over q, its share of h is (q * point) * point, and the answer is (point / sqrt(h)) * radius. The slope, q,
-- the point and the share must be finite; a row whose q is not positive is in no sum and, with a direction, unbounded.
WITH r AS (
    SELECT *, CASE WHEN q > 0.0 THEN value / q END AS point
    FROM (SELECT ps_partkey, ps_suppkey, ps_supplycost, slope, 1.0 * (slope * slope) AS q,
                 greatest(ps_supplycost::DOUBLE, 0.0) AS value
          FROM (SELECT *, sqrt(ps_availqty) AS slope FROM partsupp))
), g AS (
    SELECT sqrt(h) AS length, h = 0.0 AS still, abs(h) < 2.2250738585072014e-308 AS below,
           CASE WHEN valid THEN CASE WHEN isfinite(h) THEN true
                                     ELSE error('computes a number too large for a DOUBLE')::BOOLEAN END
                ELSE false END AS ok
    FROM (SELECT sum(share) AS h,
                 bool_and(CASE WHEN isfinite(slope) AND isfinite(q) AND isfinite(value)
                                    AND (share IS NULL OR isfinite(share)) THEN true
                               ELSE error('computes a number too large for a DOUBLE')::BOOLEAN END
                          AND CASE WHEN q > 0.0 OR value = 0.0 THEN true
                                   ELSE error('optimization is unbounded')::BOOLEAN END) AS valid
          FROM (SELECT slope, q, value, CASE WHEN q > 0.0 THEN (q * point) * point END AS share FROM r))
)
SELECT ps_partkey, ps_suppkey, ps_supplycost,
       CASE WHEN g.still OR point IS NULL THEN 0.0 ELSE point / g.length * 1000.0 END + 0.0 AS ship
FROM r, g
WHERE g.ok AND CASE WHEN g.below AND value <> 0.0 THEN __internal_decide_warn_numbers() ELSE true END
