-- Q8  Feasibility + join + entity-scoped variable (large scale)
-- TAGS: type=BOOLEAN,entity-scoped; class=feasibility; obj=none(feasibility);
--       input=join; per=PER-single,PER-multi-column; when+per=WHEN+PER; func=IS-NULL-in-WHEN
SELECT l.l_orderkey, l.l_linenumber, o.o_orderpriority, o.o_orderstatus, assign
FROM lineitem l JOIN orders o ON l.l_orderkey = o.o_orderkey
DECIDE PER o: assign(BOOL)
SUCH THAT PER o_orderpriority: SUM(assign) BY (o_orderpriority) >= 20
    AND PER o_orderpriority, o_orderstatus: SUM(assign * l.l_quantity) BY (o_orderpriority, o_orderstatus) <= 5000000
    AND WHEN (o_comment IS NOT NULL) PER o_orderstatus: SUM(assign) BY (o_orderstatus) <= 5000000;
