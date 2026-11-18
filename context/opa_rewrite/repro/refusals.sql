-- OPT-23, FORM-7, FORM-6, BIND-4, EXEC-17, BIND-5 and one known limit: wrong refusals, a missing one, and a late one. Solver path, three rows.
SET decide_direct_solve = 'off';
CREATE TABLE t(id INT, g INT, score DOUBLE, v DOUBLE);
INSERT INTO t VALUES (1,1,2.0,5),(2,1,4.0,6),(3,2,6.0,7);

.print == OPT-23: INTERNAL Error "DECIDE linear expression split into a non-linear term"
SELECT id, round(x, 3) FROM t DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 10
MINIMIZE SUM(POWER(x - avg(t.score) by (t.g), 2));
.print == OPT-23 control: the same body as a constraint answers
SELECT id, round(x, 3) FROM t DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 10
  AND SUM(POWER(x - avg(t.score) by (t.g), 2)) <= 1 MAXIMIZE SUM(x);

.print == FORM-7: the refusal names the internal column __abs_aux_0__
SELECT id, x, b FROM t DECIDE x(REAL), b(BOOL) SUCH THAT x BETWEEN 0 AND 10 MINIMIZE SUM(b * ABS(x - score));

.print == FORM-6: answers 2, 4, 6 on Gurobi; under DECIDB_FORCE_SOLVER=highs it is refused as a product of two decisions
SELECT id, round(x, 3) FROM t DECIDE x(REAL) SUCH THAT x BETWEEN 0 AND 10 MAXIMIZE sum((x - score) * (score - x));

.print == BIND-4: an objective with no decision is accepted (a warning, then an answer)
SELECT id, x FROM t DECIDE x(BOOL) SUCH THAT SUM(x) <= 2 MAXIMIZE sum(v) + 1;
.print == BIND-4: the same algebra solves as a constraint and is refused as an objective
SELECT id, x FROM t DECIDE x(BOOL) SUCH THAT abs(sum(x) - 2) <= 1 MAXIMIZE SUM(x * v);
SELECT id, x FROM t DECIDE x(BOOL) SUCH THAT SUM(x) <= 3 MINIMIZE abs(sum(x) - 2);
SELECT id, x FROM t DECIDE x(BOOL) SUCH THAT SUM(x) <= 3 MINIMIZE power(sum(x) - 3, 2);
.print == BIND-4: an objective with no decision gets a message about '+'
SELECT id, x FROM t DECIDE x(BOOL) SUCH THAT SUM(x) <= 3 MAXIMIZE v + 1;

.print == EXEC-17: a refusal that reads only the plan is raised at execution, over an empty input too; EXPLAIN is silent
CREATE TABLE e AS SELECT * FROM t WHERE false;
SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= -1 MAXIMIZE sum(x);
SELECT id, x FROM e DECIDE x(INT) SUCH THAT x <= -1 MAXIMIZE sum(x);
EXPLAIN SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= -1 MAXIMIZE sum(x);

.print == Known limit: a product over a nested reducer under a bound that reads the rows is refused; with a constant bound it answers
SELECT id, x FROM (VALUES (1,'q',3,7),(2,'p',6,12),(3,'p',5,5)) u(id,h,wv,cap) DECIDE x(INT)
SUCH THAT x <= 3 AND sum((x - 1) * max(x) by (u.h)) >= u.cap - 1 MINIMIZE sum(x) ORDER BY id;
SELECT id, x FROM (VALUES (1,'q',3,7),(2,'p',6,12),(3,'p',5,5)) u(id,h,wv,cap) DECIDE x(INT)
SUCH THAT x <= 3 AND sum((x - 1) * max(x) by (u.h)) >= 6 MINIMIZE sum(x) ORDER BY id;

.print == BIND-5: a data reducer as a factor solves inside a reducer and is refused outside one
SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= 5 AND sum(x * avg(v)) <= 8 MAXIMIZE SUM(x);
SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= 5 AND x * avg(v) <= 8 MAXIMIZE SUM(x);
