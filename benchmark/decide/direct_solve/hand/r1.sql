-- Hand-written comparator for r1: each part spends at most 5,000 units across its suppliers, best price first. Offers
-- with the same price share what the budget leaves them in proportion to their room, so the answer is the same on
-- every run (R1's rule; `ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING` would split a tie by the order the rows
-- come in). `through` is the room of the offers priced at least as high, `tied` the room of the offers at this price:
-- an offer is full when everything through its price fits, empty when the offers above it fill the budget, and takes its
-- share of what is left in between. On this data every box is finite and starts at 0, and a spend of 0 is within the
-- budget, so the rule's infeasible and unbounded checks have nothing to raise.
SELECT ps_partkey, ps_suppkey, ps_supplycost,
       least(greatest(CASE WHEN through <= 5000.0 THEN room
                           WHEN through - tied >= 5000.0 THEN 0.0
                           ELSE room * ((5000.0 - (through - tied)) / tied) END, 0.0), room) + 0.0 AS ship
FROM (
    SELECT ps_partkey, ps_suppkey, ps_supplycost, room,
           sum(room) OVER (PARTITION BY ps_partkey ORDER BY ps_supplycost DESC
                           RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS through,
           sum(room) OVER (PARTITION BY ps_partkey, ps_supplycost) AS tied
    FROM (SELECT ps_partkey, ps_suppkey, ps_supplycost, ps_availqty::DOUBLE AS room FROM partsupp)
)
