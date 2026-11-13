-- Hand-written comparator for r4_groups: r4 with one budget for each supplier, joined back to the rows by the key.
WITH r AS (
    SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, 1.0::DOUBLE / ps_availqty AS c,
           (2 * ps_availqty)::DOUBLE AS cap, ps_supplycost::DOUBLE AS w
    FROM partsupp
), a AS (
    SELECT ps_suppkey, min(c) AS cmin,
           sum(CASE WHEN w > 0.0 AND c > 0.0 THEN w / c END) AS n,
           coalesce(sum(CASE WHEN w < 0.0 AND cap < 1e20 THEN -w * cap END), 0.0) AS f,
           coalesce(sum(CASE WHEN w < 0.0 AND NOT (cap < 1e20) THEN -w END), 0.0) AS uw,
           min(CASE WHEN c > 0.0 THEN cap * c END) AS u,
           bool_and(CASE WHEN isfinite(c) AND isfinite(w) THEN true
                         ELSE error('1 / ps_availqty is NaN or infinite')::BOOLEAN END
                    AND CASE WHEN cap >= 0.0 THEN true ELSE error('infeasible')::BOOLEAN END) AS ok
    FROM r
    GROUP BY ps_suppkey
), l AS (
    SELECT ps_suppkey, n, uw, 100000000.0 + f AS b,
           CASE WHEN cmin <= 0.0 THEN 0.0 ELSE least(CASE WHEN uw > 0.0 THEN NULL ELSE (100000000.0 + f) / n END, u) END AS s,
           ok AND CASE WHEN uw = 0.0 AND 100000000.0 + f < 0.0 THEN error('infeasible')::BOOLEAN ELSE true END AS ok
    FROM a
)
SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost,
       CASE WHEN w < 0.0 THEN CASE WHEN cap < 1e20 THEN cap
                                   ELSE greatest(CASE WHEN c > 0.0 THEN l.s / c ELSE 0.0 END,
                                                 greatest(0.0, coalesce(l.n * l.s, 0.0) - l.b) / l.uw) END
            ELSE CASE WHEN c > 0.0 THEN l.s / c ELSE 0.0 END END AS ship
FROM r JOIN l USING (ps_suppkey)
WHERE l.ok
