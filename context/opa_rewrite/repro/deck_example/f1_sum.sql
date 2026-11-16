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
  per t.id: sum(when t.p per t.cid: sum(ship) by (t.cid)) by (t.rid)
            + 0.10 * sum(t.d) by (t.m) <= t.c
MAXIMIZE sum(ship)
ORDER BY tid;
