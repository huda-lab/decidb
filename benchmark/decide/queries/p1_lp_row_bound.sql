-- P1  Bounded LP: each row independently pushed to its own upper bound
-- TAGS: type=REAL; class=LP; obj=MAXIMIZE-SUM; cons=per-row-bound
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, amount
FROM lineitem
DECIDE amount(REAL)
SUCH THAT amount <= l_quantity
MAXIMIZE SUM(l_extendedprice * amount);
