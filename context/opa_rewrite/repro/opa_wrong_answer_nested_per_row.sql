-- FIXED on the prepared Sami-OPA branch (f56845f560); wrong at Sami's tip 7837e300be. See handoff.md, A4 problem 4.
-- Every query here is infeasible. The tip answered the first two.
SET threads=1;
SET decide_direct_solve='off';
-- Three groups of one row each, so sum(max(x)) = x1 + x2 + x3. Row 2 needs >= 11, but x <= 3 gives at most 9: infeasible.
SELECT id, x FROM (VALUES (1,'q',3,7),(2,'p',6,12),(3,'p',5,5)) t(id,h,wv,cap)
DECIDE x(INT)
SUCH THAT x <= 3 AND per row: sum(per (): max(x) by (t.h, t.wv)) >= t.cap - 1
MINIMIZE sum(x) ORDER BY id;
-- Same with a plain sum inside, for comparison.
SELECT id, x FROM (VALUES (1,'q',3,7),(2,'p',6,12),(3,'p',5,5)) t(id,h,wv,cap)
DECIDE x(INT)
SUCH THAT x <= 3 AND per row: sum(per (): sum(x) by (t.h, t.wv)) >= t.cap - 1
MINIMIZE sum(x) ORDER BY id;
-- Same without `per row:` on the clause.
SELECT id, x FROM (VALUES (1,'q',3,7),(2,'p',6,12),(3,'p',5,5)) t(id,h,wv,cap)
DECIDE x(INT)
SUCH THAT x <= 3 AND sum(per (): max(x) by (t.h, t.wv)) >= 11
MINIMIZE sum(x) ORDER BY id;
