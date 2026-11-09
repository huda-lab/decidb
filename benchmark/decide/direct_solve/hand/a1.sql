-- Hand-written comparator for a1: the plan an expert writes that carries what the A1 rule owes.
-- The verdict comes first (an empty box raises "infeasible", an answer that is not finite raises "unbounded"), and no
-- row is released before every row has passed it, so the table is read twice: once to check, once to stream.
-- Each row's answer is its target clamped into its box, with -0.0 turned into 0.0.
WITH verdict AS MATERIALIZED (
    SELECT bool_and(
        CASE WHEN 0.0 > ps_availqty::DOUBLE THEN error('infeasible')::BOOLEAN
             WHEN NOT isfinite(least(greatest(5000.0, 0.0), ps_availqty::DOUBLE)) THEN error('unbounded')::BOOLEAN
             ELSE true END) AS ok
    FROM partsupp
)
SELECT ps_partkey, ps_suppkey, least(greatest(5000.0, 0.0), ps_availqty::DOUBLE) + 0.0 AS ship
FROM partsupp, verdict
WHERE verdict.ok
