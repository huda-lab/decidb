-- Q8  Feasibility + join + entity-scoped variable (large scale)
-- TAGS: type=BOOLEAN,entity-scoped; class=feasibility; obj=none(feasibility);
--       input=join; per=PER-single,PER-multi-column; when+per=WHEN+PER; func=IS-NULL-in-WHEN
SELECT l.l_orderkey, l.l_linenumber, o.o_orderpriority, o.o_orderstatus, assign
FROM lineitem l JOIN orders o ON l.l_orderkey = o.o_orderkey
DECIDE per o: assign(BOOL)
SUCH THAT per o_orderpriority: sum(assign) by (o_orderpriority) >= 20
    AND per o_orderpriority, o_orderstatus: sum(assign * l.l_quantity) by (o_orderpriority, o_orderstatus) <= 5000000
    AND when o_comment IS NOT NULL per o_orderstatus: sum(assign) by (o_orderstatus) <= 5000000;
