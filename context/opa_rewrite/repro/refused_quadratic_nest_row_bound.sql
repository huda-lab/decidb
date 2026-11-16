-- Known limitation on the prepared Sami-OPA branch (handoff.md, A7 item 10).
--
-- A product or square over a nested reducer, under a bound that reads the rows.
--   36f12dd089 (Nov 13):   answers correctly (a helper decision holds the outer sum).
--   7837e300be (Sami tip): answers wrongly (one row, with the first row's bound).
--   f56845f560 (prepared): refuses, "squares or multiplies decisions inside nested reducers".
--
-- Query 1 is refused. Query 2, the same clause with a constant bound, answers.
SET threads=1;
SET decide_direct_solve='off';
SELECT id, x FROM (VALUES (1,'q',3,7),(2,'p',6,12),(3,'p',5,5)) t(id,h,wv,cap)
DECIDE x(INT)
SUCH THAT x <= 3 AND sum((x - 1) * max(x) by (t.h)) >= t.cap - 1
MINIMIZE sum(x) ORDER BY id;
SELECT id, x FROM (VALUES (1,'q',3,7),(2,'p',6,12),(3,'p',5,5)) t(id,h,wv,cap)
DECIDE x(INT)
SUCH THAT x <= 3 AND sum((x - 1) * max(x) by (t.h)) >= 6
MINIMIZE sum(x) ORDER BY id;
