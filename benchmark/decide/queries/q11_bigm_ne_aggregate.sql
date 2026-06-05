-- Big-M CORRECTNESS probe: aggregate not-equal. SUM(x) over ~60K rows with
-- x<=20 has range [0, ~1.2M], so the valid Big-M for this aggregate is ~1.2M.
-- The legacy code computed M from a single per-row bound (=20) floored to 1e6,
-- which is BELOW the true range -> it silently caps SUM(x) at ~1e6 and returns a
-- wrong (suboptimal) optimum. The true optimum is all x=20 (SUM=1,203,500 != 100).
-- The data-driven M sums the range over the group and gets the correct answer.
-- Compare the objective against the legacy build: it should be strictly larger.
SELECT l_orderkey, l_linenumber, l_quantity, l_extendedprice, x
FROM lineitem
DECIDE x IS INTEGER
SUCH THAT x <= 20
    AND SUM(x) <> 100
MAXIMIZE SUM(x * l_extendedprice);
