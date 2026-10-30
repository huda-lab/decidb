-- Direct-solve class A1 (separable box), made infeasible: most offers' upper end `ps_availqty - 9000` is below the
-- floor of 100. Quadratic, so it runs on the 800-row database.
-- database: tpch_sf0.00125
SELECT ps_partkey, ps_suppkey, ship
DECIDE ship(REAL)
FROM partsupp
SUCH THAT ship BETWEEN 100 AND ps_availqty - 9000
MINIMIZE SUM(POWER(ship - 5000, 2))
