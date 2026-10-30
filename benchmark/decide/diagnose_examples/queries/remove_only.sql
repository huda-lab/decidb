-- An exclusion that leaves a BOOL no value: an `<>` clause cannot be loosened, only removed (remove_only).
SELECT l_orderkey, l_linenumber, x
DECIDE x(BOOL)
FROM lineitem
WHERE l_orderkey = 1
SUCH THAT x <> 0 AND x <> 1
MINIMIZE SUM(x)
