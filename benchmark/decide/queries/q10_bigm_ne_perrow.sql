-- Big-M PERFORMANCE probe: per-row not-equal (<>) creates one binary indicator
-- and a Big-M disjunction PER ROW (~60K indicators on decidb.db). x<=20 so the
-- tight data-driven Big-M is ~32, vs the legacy 1e6 floor (~30000x). Measures
-- whether a tight per-row M speeds the solve (or whether Gurobi presolve already
-- neutralizes it). Optimum must be identical to the legacy build.
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, x
FROM lineitem
DECIDE x IS INTEGER
SUCH THAT x <= 20
    AND x <> 11
    AND SUM(x * l_quantity) <= 5000000
MAXIMIZE SUM(x * l_extendedprice);
