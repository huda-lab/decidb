-- A bound no assignment can reach: the clause is named, with no finite edit to offer (unreachable_bound).
SELECT l_orderkey, l_linenumber, x
DECIDE x(INT)
FROM lineitem
WHERE l_orderkey <= 3
SUCH THAT x <= 6 AND SUM(x) >= 'Infinity'::DOUBLE
MAXIMIZE SUM(x)
