-- P4  High-cardinality PER: exactly one row selected per order key
-- TAGS: type=BOOLEAN; class=ILP; obj=MAXIMIZE-SUM; per=PER-single-column; cons=equality(=)
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, keep
FROM lineitem
DECIDE keep(BOOL)
SUCH THAT SUM(keep) = 1 PER l_orderkey
MAXIMIZE SUM(l_extendedprice * keep);
