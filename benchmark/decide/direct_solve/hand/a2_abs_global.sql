-- Hand-written comparator for a2_abs_global: the median of the supply costs, clamped into [0, 2000]. With equal weights
-- every point between the lower median and the upper median is optimal, and the rule takes the one nearest 0: the lower
-- median clamped above by the upper. The class value must be finite; -0.0 becomes 0.0.
SELECT ps_partkey, ps_suppkey, ps_supplycost, g.c AS center
FROM partsupp, (
    SELECT CASE WHEN isfinite(c) THEN c ELSE error('computes a number too large for a DOUBLE') END AS c
    FROM (SELECT least(greatest(least(greatest(0.0, quantile_disc(ps_supplycost::DOUBLE, 0.5)),
                                      -quantile_disc(-ps_supplycost::DOUBLE, 0.5)), 0.0), 2000.0) + 0.0 AS c
          FROM partsupp)
) g
