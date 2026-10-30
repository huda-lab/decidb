-- An unbounded quadratic objective: no ray names the variable, so the diagnosis says why it cannot (undiagnosed).
-- HiGHS refuses the non-convex objective before solving.
SELECT l_orderkey, l_linenumber, x
DECIDE x(REAL)
FROM lineitem
WHERE l_orderkey <= 3
SUCH THAT x >= 0
MAXIMIZE SUM(POWER(x, 2))
