-- Hand-written comparator for n1: the point of the ball sqrt(h) <= 1000 along the direction of the (non-negative) supply
-- costs. Each row's target and its square must be finite while h is summed, h itself must be finite, and the scale is
-- (value / sqrt(h)) * radius; an h below the normal range warns on a row that has a value. A group with h = 0 gives 0.
SELECT ps_partkey, ps_suppkey, ps_supplycost,
       CASE WHEN g.still THEN 0.0 ELSE greatest(ps_supplycost::DOUBLE, 0.0) / g.length * 1000.0 END + 0.0 AS ship
FROM partsupp, (
    SELECT sqrt(h) AS length, h = 0.0 AS still, abs(h) < 2.2250738585072014e-308 AS below,
           CASE WHEN valid THEN CASE WHEN isfinite(h) THEN true
                                     ELSE error('computes a number too large for a DOUBLE')::BOOLEAN END
                ELSE false END AS ok
    FROM (SELECT sum(square) AS h,
                 bool_and(CASE WHEN isfinite(value) AND isfinite(square) THEN true
                               ELSE error('computes a number too large for a DOUBLE')::BOOLEAN END) AS valid
          FROM (SELECT value, value * value AS square
                FROM (SELECT greatest(ps_supplycost::DOUBLE, 0.0) AS value FROM partsupp)))
) g
WHERE g.ok AND CASE WHEN g.below AND ps_supplycost::DOUBLE <> 0.0 THEN __internal_decide_warn_numbers() ELSE true END
