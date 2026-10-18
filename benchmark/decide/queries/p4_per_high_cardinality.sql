-- P4  High-cardinality PER: exactly one row selected per order key
-- TAGS: type=BOOLEAN; class=ILP; obj=MAXIMIZE-SUM; per=PER-single-column; cons=equality(=)
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, keep
FROM lineitem
DECIDE keep(BOOL)
SUCH THAT per l_orderkey: sum(keep) by (l_orderkey) = 1
MAXIMIZE SUM(l_extendedprice * keep);
