-- Q4  MIN/MAX (easy) + two-level objective (large scale)
-- TAGS: type=BOOLEAN; class=ILP; obj=MINIMIZE-MAX(easy),two-level-objective;
--       func=MAX(easy-constraint); cons=composed-MIN/MAX-in-LHS; per=per-(),per-single-column,by-single-column
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, l_returnflag, keep
FROM lineitem
DECIDE keep(BOOL)
SUCH THAT per l_returnflag: sum(keep) by (l_returnflag) >= 2
    AND per l_returnflag: sum(keep * l_quantity) by (l_returnflag) <= ${Q4_QTY_CAP}
    AND per (): max(keep * l_quantity) <= 45
    AND per (): sum(keep * l_extendedprice) + max(keep * l_extendedprice) <= 50000000
MINIMIZE max(per l_returnflag: sum(keep * l_extendedprice) by (l_returnflag));
