-- Hand-written comparator for a2_abs: the median of each supplier's supply costs, clamped into [0, 2000] and joined back
-- to the rows (see a2_abs_global). Each class's value must be finite; -0.0 becomes 0.0.
WITH g AS (
    SELECT ps_suppkey,
           CASE WHEN isfinite(c) THEN c ELSE error('computes a number too large for a DOUBLE') END AS c
    FROM (SELECT ps_suppkey,
                 least(greatest(least(greatest(0.0, quantile_disc(ps_supplycost::DOUBLE, 0.5)),
                                      -quantile_disc(-ps_supplycost::DOUBLE, 0.5)), 0.0), 2000.0) + 0.0 AS c
          FROM partsupp GROUP BY ps_suppkey)
)
SELECT ps_partkey, ps_suppkey, ps_supplycost, g.c AS center
FROM partsupp JOIN g USING (ps_suppkey)
