-- Two literals on one decision contradict: the smaller one is loosened in place (source_literal), and the objective
-- after the edit is reported (achievable_objective).
SELECT l_orderkey, l_linenumber, x
DECIDE x(INT)
FROM lineitem
WHERE l_orderkey <= 3
SUCH THAT x <= 5 AND x >= 8
MAXIMIZE SUM(x)
