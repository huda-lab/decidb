-- Hand-written comparator for r4: what R4 owes for a weight read from a column, whose sign the plan cannot see.
-- Each row takes the end of its range that uses the least budget: a row with a positive weight its level over its
-- coefficient, a row with a negative weight its capacity (or, with none, its share of what the others need beyond
-- the budget). Every value is checked before a row is released.
WITH r AS (
    SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, 1.0::DOUBLE / ps_availqty AS c,
           (2 * ps_availqty)::DOUBLE AS cap, ps_supplycost::DOUBLE AS w
    FROM partsupp
), a AS (
    SELECT min(c) AS cmin,
           sum(CASE WHEN w > 0.0 AND c > 0.0 THEN w / c END) AS n,
           coalesce(sum(CASE WHEN w < 0.0 AND cap < 1e20 THEN -w * cap END), 0.0) AS f,
           coalesce(sum(CASE WHEN w < 0.0 AND NOT (cap < 1e20) THEN -w END), 0.0) AS uw,
           min(CASE WHEN c > 0.0 THEN cap * c END) AS u,
           bool_and(CASE WHEN isfinite(c) AND isfinite(w) THEN true
                         ELSE error('1 / ps_availqty is NaN or infinite')::BOOLEAN END
                    AND CASE WHEN cap >= 0.0 THEN true ELSE error('infeasible')::BOOLEAN END) AS ok
    FROM r
), l AS (
    SELECT n, uw, 1000000000000.0 + f AS b,
           CASE WHEN cmin <= 0.0 THEN 0.0 ELSE least(CASE WHEN uw > 0.0 THEN NULL ELSE (1000000000000.0 + f) / n END, u) END AS s,
           ok AND CASE WHEN uw = 0.0 AND 1000000000000.0 + f < 0.0 THEN error('infeasible')::BOOLEAN ELSE true END AS ok
    FROM a
)
SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost,
       CASE WHEN w < 0.0 THEN CASE WHEN cap < 1e20 THEN cap
                                   ELSE greatest(CASE WHEN c > 0.0 THEN l.s / c ELSE 0.0 END,
                                                 greatest(0.0, coalesce(l.n * l.s, 0.0) - l.b) / l.uw) END
            ELSE CASE WHEN c > 0.0 THEN l.s / c ELSE 0.0 END END AS ship
FROM r, l
WHERE l.ok
