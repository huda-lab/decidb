-- Q1  ILP selection kitchen-sink (large scale)
-- TAGS: type=BOOLEAN; class=ILP; obj=MAXIMIZE-SUM,WHEN-on-objective;
--       func=SUM,AVG; when=WHEN-constraint,aggregate-local-WHEN; per=PER-multi-column
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_returnflag, l_linestatus, keep
FROM lineitem
DECIDE keep IS BOOLEAN
SUCH THAT SUM(keep * l_quantity) <= ${Q1_QTY_CAP}
    AND AVG(keep * l_discount) <= 0.06
    AND SUM(keep * l_quantity) <= ${Q1_R_QTY_CAP} WHEN l_returnflag = 'R'
    AND SUM(keep) <= ${Q1_GRP_CAP} PER (l_returnflag, l_linestatus)
    AND SUM(keep * l_extendedprice) WHEN (l_returnflag = 'A') + SUM(keep * l_extendedprice) WHEN (l_returnflag = 'N') <= ${Q1_LOCAL_CAP}
MAXIMIZE SUM(keep * l_extendedprice) WHEN l_linestatus = 'F';
