-- Q4  MIN/MAX (easy) + nested-PER objective (large scale)
-- TAGS: type=BOOLEAN; class=ILP; obj=MINIMIZE-MAX(easy),nested-PER-objective;
--       func=MAX(easy-constraint); cons=composed-MIN/MAX-in-LHS; per=PER-single-column
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, l_returnflag, keep
FROM lineitem
DECIDE keep(BOOL)
SUCH THAT PER l_returnflag: SUM(keep) BY (l_returnflag) >= 2
    AND PER l_returnflag: SUM(keep * l_quantity) BY (l_returnflag) <= ${Q4_QTY_CAP}
    AND MAX(keep * l_quantity) <= 45
    AND SUM(keep * l_extendedprice) + MAX(keep * l_extendedprice) <= 50000000
MINIMIZE MAX(PER l_returnflag: SUM(keep * l_extendedprice) BY (l_returnflag));
