-- A per-order floor most orders cannot meet: in the query slack scope the clause's one literal is loosened to what
-- every order can reach (source_literal).
SELECT l_orderkey, l_linenumber, x
DECIDE x(BOOL)
FROM lineitem
WHERE l_orderkey <= 40
SUCH THAT per l_orderkey: SUM(x) by (l_orderkey) >= 5
MAXIMIZE SUM(x)
