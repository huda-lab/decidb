-- =====================================================================
-- PROBLEM CLASSES — one or more representative query per class
-- =====================================================================
-- Each class is inferred automatically from variable types + obj/constraint
-- shapes. These queries exercise the classifier + solver-routing branches.

-- --- P1: LP — all REAL, linear -----------------------------------------
-- Continuous vars, linear constraint, linear objective.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 10
DECIDE qty IS REAL
SUCH THAT qty <= 100
  AND SUM(qty) <= 500
MAXIMIZE SUM(p_retailprice * qty);

-- --- P2: LP — multi-variable continuous -------------------------------
-- Two continuous vars, linking constraint.
SELECT p_partkey, p_retailprice, qty, stock
FROM part
WHERE p_size < 5
DECIDE qty IS REAL, stock IS REAL
SUCH THAT qty <= 50 AND stock <= 50
  AND SUM(qty + stock) <= 200
MAXIMIZE SUM(p_retailprice * qty + 0.5 * p_retailprice * stock);

-- --- P3: ILP — all INTEGER/BOOLEAN, linear ----------------------------
-- Pure integer program (knapsack).
SELECT p_partkey, p_retailprice, pick
FROM part
WHERE p_size < 10
DECIDE pick IS BOOLEAN
SUCH THAT SUM(p_retailprice * pick) <= 100000
MAXIMIZE SUM(p_retailprice * pick);

-- --- P4: ILP — INTEGER vars with per-row bound ------------------------
-- Integer quantities.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND SUM(qty) <= 100
MAXIMIZE SUM(p_retailprice * qty);

-- --- P5: MILP — mix of REAL + BOOLEAN ---------------------------------
-- Mixed continuous + binary.
SELECT p_partkey, p_retailprice, pick, stock
FROM part
WHERE p_size < 5
DECIDE pick IS BOOLEAN, stock IS REAL
SUCH THAT stock <= 100
  AND SUM(pick) <= 20
MAXIMIZE SUM(p_retailprice * pick + 0.1 * stock);

-- --- P6: QP — continuous quadratic objective (both solvers) -----------
-- Convex QP.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) = 50
MINIMIZE SUM(POWER(qty - 3, 2));

-- --- P7: MIQP — integer + quadratic (Gurobi only) ---------------------
-- Quadratic objective with integer vars. Kept small (~22 vars, qty<=5,
-- SUM=10) so the integer search closes the LP/IP gap quickly. Larger
-- formulations of the same shape (e.g. p_size<5, qty<=10, SUM=30) are a
-- genuinely hard MIQP whose B&B explores millions of nodes without
-- closing the gap; PackDB's 300s solver TimeLimit will return a feasible
-- (not necessarily optimal) result on such inputs.
SELECT p_partkey, qty
FROM part
WHERE p_size = 1 AND p_partkey < 1000
DECIDE qty IS INTEGER
SUCH THAT qty <= 5 AND SUM(qty) = 10
MINIMIZE SUM(POWER(qty - 2, 2));

-- --- P8: QCQP — quadratic constraint (Gurobi only) --------------------
-- Linear obj, quadratic constraint.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size BETWEEN 1 AND 5
DECIDE qty IS REAL
SUCH THAT qty <= 10
  AND SUM(POWER(qty, 2)) <= 500
MAXIMIZE SUM(p_retailprice * qty);

-- --- P9: Bilinear — bool × real (McCormick, both solvers) -------------
-- Linearizable bilinear.
SELECT p_partkey, p_retailprice, pick, qty
FROM part
WHERE p_size < 5
DECIDE pick IS BOOLEAN, qty IS REAL
SUCH THAT qty <= 50
  AND SUM(pick * qty) <= 200
MAXIMIZE SUM(p_retailprice * pick * qty);

-- --- P10: Bilinear — int × real general (Gurobi only) -----------------
-- Non-convex bilinear.
SELECT p_partkey, p_retailprice, qty, disc
FROM part
WHERE p_size < 3
DECIDE qty IS INTEGER, disc IS REAL
SUCH THAT qty <= 20 AND disc <= 5
MAXIMIZE SUM(p_retailprice * qty - qty * disc);

-- --- P11: Feasibility — no MAXIMIZE/MINIMIZE --------------------------
-- Find any satisfying assignment.
SELECT s_suppkey, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) = 10;

-- --- P12: Feasibility — continuous --------------------------------------
-- Continuous LP-feasibility with no objective.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 10 AND SUM(qty) = 50;

-- --- P13: ILP table-scoped variables ----------------------------------
-- Entity-keyed variable (one per unique supplier) across join.
SELECT l.l_suppkey, l.l_extendedprice, supplier.pick
FROM lineitem l
JOIN supplier ON l.l_suppkey = supplier.s_suppkey
WHERE l.l_orderkey < 500
DECIDE supplier.pick IS BOOLEAN
SUCH THAT SUM(supplier.pick) <= 20
MAXIMIZE SUM(l.l_extendedprice * supplier.pick);

-- --- P14: MILP with table-scoped + row-scoped mix ---------------------
-- One supplier var + per-row quantity var.
SELECT l.l_orderkey, l.l_linenumber, l.l_suppkey, l.l_extendedprice,
       supplier.pick, qty
FROM lineitem l
JOIN supplier ON l.l_suppkey = supplier.s_suppkey
WHERE l.l_orderkey < 200
DECIDE supplier.pick IS BOOLEAN, qty IS INTEGER
SUCH THAT qty <= 5
  AND SUM(supplier.pick) <= 30
  AND SUM(qty) <= 200
MAXIMIZE SUM(l.l_extendedprice * qty * supplier.pick);

-- --- P15: MIQP with table-scoped variable -----------------------------
-- Class: MIQP × table-scoped. P6/P7/P8 cover QP/MIQP without table-
-- scoping; P13/P14 cover table-scoped + linear. This combination
-- exercises the per-entity variable indexer under a quadratic objective.
SELECT s_suppkey, s_acctbal, supplier.qty
FROM supplier
DECIDE supplier.qty IS INTEGER
SUCH THAT supplier.qty <= 10
MINIMIZE SUM(POWER(supplier.qty - 5, 2));
