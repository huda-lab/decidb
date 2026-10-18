-- Q1  ILP selection kitchen-sink (large scale)
-- TAGS: type=BOOLEAN; class=ILP; obj=MAXIMIZE-SUM,WHEN-on-objective;
--       func=SUM,AVG; when=WHEN-constraint,aggregate-local-WHEN; per=PER-multi-column
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_returnflag, l_linestatus, keep
FROM lineitem
DECIDE keep(BOOL)
SUCH THAT per (): sum(keep * l_quantity) <= ${Q1_QTY_CAP}
    AND per (): avg(keep * l_discount) <= 0.06
    AND when l_returnflag = 'R' per (): sum(keep * l_quantity) <= ${Q1_R_QTY_CAP}
    AND per l_returnflag, l_linestatus: sum(keep) by (l_returnflag, l_linestatus) <= ${Q1_GRP_CAP}
    AND per (): sum(when l_returnflag = 'A': keep * l_extendedprice) + sum(when l_returnflag = 'N': keep * l_extendedprice) <= ${Q1_LOCAL_CAP}
MAXIMIZE when l_linestatus = 'F': sum(keep * l_extendedprice);
