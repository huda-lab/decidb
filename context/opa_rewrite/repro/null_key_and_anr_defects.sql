.mode list
-- S1: the guide says a row whose `per` key is NULL gets no constraint, so row 2 should reach 10.
CREATE TABLE n(id INT, g VARCHAR, cap INT, v INT);
INSERT INTO n VALUES (1,'a',5,3),(2,NULL,7,5),(3,'b',4,6);
SELECT 'S1', id, g, x FROM n DECIDE x(INT) SUCH THAT x <= 10 AND per g: x <= 2 MAXIMIZE sum(v * x) ORDER BY id;
-- D3: group b has no row with a k, so `per t.k:` constrains nothing in it; rows 3 and 4 should reach 100.
CREATE TABLE w5(id INT, g VARCHAR, k INT, cap INT);
INSERT INTO w5 VALUES (1,'a',1,2),(2,'a',NULL,2),(3,'b',NULL,2),(4,'b',NULL,2);
SELECT 'D3', id, x FROM w5 t DECIDE x(INT) SUCH THAT x <= 100 AND per t.k: sum(x) by (t.g) <= 3 MAXIMIZE sum(x) ORDER BY id;
-- D5: sum(b*x) <= 2*y - 6 must hold in the answer.
SELECT 'D5', id, x, b, y FROM (VALUES (1), (2)) t(id) DECIDE x(INT), b(BOOL), per (): y(BOOL)
SUCH THAT x <= 3 AND when t.id = 1: x >= -5 AND sum(b * x) <= 2 * y - 6 MINIMIZE 10 * y + SUM(x) ORDER BY id;
