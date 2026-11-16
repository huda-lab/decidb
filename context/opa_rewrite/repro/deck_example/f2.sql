SET threads=1; SET decide_direct_solve='off';
SELECT tid, inventory FROM (VALUES (1,'A',1,5),(2,'A',2,3),(3,'B',1,1),(4,'B',2,4),(5,'B',3,8),(6,'B',4,2)) t(tid,product,period,cap)
DECIDE inventory(INT)
SUCH THAT inventory <= t.cap AND
  inventory >= at(previous: inventory) over (t.period within t.product)
MAXIMIZE sum(inventory)
ORDER BY tid;
