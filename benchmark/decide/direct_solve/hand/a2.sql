-- Hand-written comparator for a2: one shared value for every row, the mean of the supply costs clamped into [0, 2000].
-- The class value is computed once from a first read and must be finite (the rule's checks of the class's sums and of
-- its answer); -0.0 becomes 0.0.
SELECT ps_partkey, ps_suppkey, ps_supplycost, g.c AS center
FROM partsupp, (
    SELECT CASE WHEN isfinite(c) THEN c ELSE error('computes a number too large for a DOUBLE') END AS c
    FROM (SELECT least(greatest(sum(ps_supplycost::DOUBLE) / count(*), 0.0), 2000.0) + 0.0 AS c FROM partsupp)
) g
