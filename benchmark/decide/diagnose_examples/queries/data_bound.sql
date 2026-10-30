-- A bound read from a column no BOOL can reach: the edit is an offset over the column (virtual_offset), the query
-- slack scope's one edit for the clause.
SELECT l_orderkey, l_linenumber, x
DECIDE x(BOOL)
FROM lineitem
WHERE l_orderkey <= 20
SUCH THAT x >= l_quantity
MAXIMIZE SUM(x)
