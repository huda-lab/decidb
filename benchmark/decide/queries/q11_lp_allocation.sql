-- Q11  Continuous allocation, pure LP (large scale)
-- TAGS: type=REAL; class=LP; obj=MAXIMIZE-AVG; func=AVG; cons=per-row-bound,aggregate-budget
SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, alloc
FROM partsupp
DECIDE alloc IS REAL
SUCH THAT alloc <= ps_availqty
    AND SUM(alloc * ps_supplycost) <= ${Q11_BUDGET}
MAXIMIZE AVG(alloc);
