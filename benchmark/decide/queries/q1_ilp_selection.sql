-- Q1  ILP selection kitchen-sink (large scale)
-- TAGS: type=BOOLEAN; class=ILP; obj=MAXIMIZE-SUM,WHEN-on-objective;
--       func=SUM,AVG; when=WHEN-constraint,aggregate-local-WHEN; per=PER-multi-column
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_returnflag, l_linestatus, keep
FROM lineitem
DECIDE keep(BOOL)
SUCH THAT SUM(keep * l_quantity) <= ${Q1_QTY_CAP}
    AND AVG(keep * l_discount) <= 0.06
    AND WHEN l_returnflag = 'R': SUM(keep * l_quantity) <= ${Q1_R_QTY_CAP}
    AND PER l_returnflag, l_linestatus: SUM(keep) BY (l_returnflag, l_linestatus) <= ${Q1_GRP_CAP}
    AND SUM(WHEN (l_returnflag = 'A'): keep * l_extendedprice) + SUM(WHEN (l_returnflag = 'N'): keep * l_extendedprice) <= ${Q1_LOCAL_CAP}
MAXIMIZE SUM(WHEN l_linestatus = 'F': keep * l_extendedprice);
