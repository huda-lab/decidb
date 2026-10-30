-- An infeasible query whose repair leaves the objective unbounded: `y` has no upper bound, so once the floor on `x` is
-- loosened the objective grows without limit (source_literal, unbounded_after_fix).
SELECT l_orderkey, l_linenumber, x, y
DECIDE x(BOOL), y(REAL)
FROM lineitem
WHERE l_orderkey <= 3
SUCH THAT SUM(x) >= 50
MAXIMIZE SUM(x + y)
