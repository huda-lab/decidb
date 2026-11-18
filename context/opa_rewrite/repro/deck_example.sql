-- The deck's running example (context/New_syntax.pdf, pp. 77-119), on the solver path.
-- F1 with MAX builds the model of p. 115, F1 with SUM the model of p. 119, F2 the frame model of p. 108.
-- SET decide_debug = 'tree' prints the tree of p. 81; 'containers' prints the containers of p. 91.

-- F1, MAX in SUM
SET threads=1; SET decide_direct_solve='off';
SELECT tid, ship FROM (VALUES
 (1,'d1','East','Air','c1',true,20,30),
 (2,'d1','East','Air','c1',true,15,30),
 (3,'d1','West','Ground','c2',true,10,50),
 (4,'d1','West','Ground','c3',false,5,50),
 (5,'d2','West','Air','c4',true,25,45),
 (6,'d2','West','Air','c4',true,15,45)) t(tid,id,rid,m,cid,p,d,c)
DECIDE ship(INT)
SUCH THAT ship <= 100 AND
  per t.id: sum(when t.p per t.cid: max(ship) by (t.cid)) by (t.rid)
            + 0.10 * sum(t.d) by (t.m) <= t.c
MAXIMIZE sum(ship)
ORDER BY tid;

-- F1, SUM in SUM
SELECT tid, ship FROM (VALUES
 (1,'d1','East','Air','c1',true,20,30),
 (2,'d1','East','Air','c1',true,15,30),
 (3,'d1','West','Ground','c2',true,10,50),
 (4,'d1','West','Ground','c3',false,5,50),
 (5,'d2','West','Air','c4',true,25,45),
 (6,'d2','West','Air','c4',true,15,45)) t(tid,id,rid,m,cid,p,d,c)
DECIDE ship(INT)
SUCH THAT ship <= 100 AND
  per t.id: sum(when t.p per t.cid: sum(ship) by (t.cid)) by (t.rid)
            + 0.10 * sum(t.d) by (t.m) <= t.c
MAXIMIZE sum(ship)
ORDER BY tid;

-- F2, a frame
SELECT tid, inventory FROM (VALUES (1,'A',1,5),(2,'A',2,3),(3,'B',1,1),(4,'B',2,4),(5,'B',3,8),(6,'B',4,2)) t(tid,product,period,cap)
DECIDE inventory(INT)
SUCH THAT inventory <= t.cap AND
  inventory >= at(previous: inventory) over (t.period within t.product)
MAXIMIZE sum(inventory)
ORDER BY tid;
