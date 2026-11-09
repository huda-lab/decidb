-- Hand-written comparator for r1_global: one budget of a tenth of the total cost over every offer, spent on the offers
-- with the most stock per unit of cost first (see r1 for the ties). `through` is the budget the offers priced at least
-- as high use at their full amount, `tied` the budget the offers at this price use. The budget is read from the data
-- (0.1 * SUM of the exact costs), so it comes from a first read. On this data every box is [0, 1], so a spend of 0 is
-- within the budget, and the rule's infeasible and unbounded checks have nothing to raise.
SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost,
       least(greatest(CASE WHEN through <= budget THEN 1.0
                           WHEN through - tied >= budget THEN 0.0
                           ELSE 0.0 + (weight * ((budget - (through - tied)) / tied)) / weight END, 0.0), 1.0) + 0.0 AS ship
FROM (
    SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, weight, budget,
           sum(weight) OVER (ORDER BY price DESC RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS through,
           sum(weight) OVER (PARTITION BY price) AS tied
    FROM (SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, ps_supplycost::DOUBLE AS weight,
                 ps_availqty::DOUBLE / ps_supplycost::DOUBLE + 0.0 AS price
          FROM partsupp),
         (SELECT (0.1 * sum(ps_supplycost))::DOUBLE AS budget FROM partsupp)
)
