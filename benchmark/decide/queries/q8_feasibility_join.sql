-- Q8  Feasibility + join + entity-scoped variable (large scale)
-- TAGS: type=BOOLEAN,entity-scoped; class=feasibility; obj=none(feasibility);
--       input=join; per=PER-single,PER-multi-column; when+per=WHEN+PER; func=IS-NULL-in-WHEN
SELECT l.l_orderkey, l.l_linenumber, o.o_orderpriority, o.o_orderstatus, assign
FROM lineitem l JOIN orders o ON l.l_orderkey = o.o_orderkey
DECIDE o.assign IS BOOLEAN
SUCH THAT SUM(assign) >= 20 PER o_orderpriority
    AND SUM(assign * l.l_quantity) <= 5000000 PER (o_orderpriority, o_orderstatus)
    AND SUM(assign) <= 5000000 WHEN (o_comment IS NOT NULL) PER o_orderstatus;
