-- =====================================================================
-- CONSTRAINT SHAPES — stress-test queries against TPC-H
-- Each query is preceded by a comment block describing which branch it covers.
-- =====================================================================

-- --- C1: Per-row linear constraint ------------------------------------
-- Branch: per-row constraint, linear, with table/column data coefficients.
-- Scenario: decide how many of each part to order; per-row cap on quantity.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_retailprice < 1500
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
MAXIMIZE SUM(p_retailprice * qty);

-- --- C2: Aggregate SUM constraint -------------------------------------
-- Branch: aggregate SUM(dec_var) constraint with a scalar RHS.
-- Scenario: supplier selection with a total-count budget.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 20
MAXIMIZE SUM(s_acctbal * pick);

-- --- C3: Aggregate AVG constraint -------------------------------------
-- Branch: AVG(dec_var) rewritten to SUM with RHS scaling.
-- Scenario: limit average quantity picked across chosen parts.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 10
DECIDE qty IS INTEGER
SUCH THAT qty <= 20
  AND AVG(qty) <= 5
MAXIMIZE SUM(p_retailprice * qty);

-- --- C4: MAX(expr) <= K (easy, no Big-M) ------------------------------
-- Branch: MAX aggregate, easy direction, stripped to per-row bound.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MAX(s_acctbal * pick) <= 8000
  AND SUM(pick) >= 5
MAXIMIZE SUM(pick);

-- --- C5: MIN(expr) >= K (easy, no Big-M) ------------------------------
-- Branch: MIN aggregate, easy direction, stripped to per-row bound.
SELECT s_suppkey, s_acctbal, level
FROM supplier
WHERE s_acctbal > 0
DECIDE level IS INTEGER
SUCH THAT level <= 5
  AND MIN(s_acctbal * level) >= 100
MAXIMIZE SUM(level);

-- --- C6: MIN(expr) <= K (hard, Big-M indicators) ----------------------
-- Branch: MIN aggregate, hard direction, binary indicator per row.
-- Inner expression `s_acctbal * pick + 1000 * (1 - pick)` is linear in
-- pick after distribution: `(s_acctbal - 1000) * pick + 1000`. The
-- multiply-over-add distributor + per-row LHS-constant-to-RHS handling
-- now make this work end-to-end.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MIN(s_acctbal * pick + 1000 * (1 - pick)) <= 500
  AND SUM(pick) >= 3
MAXIMIZE SUM(s_acctbal * pick);

-- --- C7: MAX(expr) >= K (hard, Big-M indicators) ----------------------
-- Branch: MAX aggregate, hard direction.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MAX(s_acctbal * pick) >= 9000
  AND SUM(pick) <= 10
MAXIMIZE SUM(pick);

-- --- C8: Aggregate equality (hard) ------------------------------------
-- Branch: SUM(...) = K.
SELECT n_nationkey, flag
FROM nation
DECIDE flag IS BOOLEAN
SUCH THAT SUM(flag) = 7
MAXIMIZE SUM(n_nationkey * flag);

-- --- C9: Composed MIN with SUM inside ---------------------------------
-- Branch: composed aggregate — MIN(linear-in-pick) >= K (easy direction,
-- strips to per-row) + SUM constraint. Same shape as C6 inside the MIN;
-- works after distribution of `100000 * (1 - pick)`.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MAX(pick) <= 1
  AND SUM(pick) = 10
  AND MIN(s_acctbal * pick + 100000 * (1 - pick)) >= 1000
MAXIMIZE SUM(s_acctbal * pick);

-- --- C10: Quadratic constraint POWER(expr, 2) (QCQP, Gurobi only) -----
-- Branch: quadratic in SUCH THAT.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size BETWEEN 1 AND 5
DECIDE qty IS REAL
SUCH THAT qty <= 10
  AND SUM(POWER(qty, 2)) <= 500
MAXIMIZE SUM(p_retailprice * qty);

-- --- C11: Bilinear constraint — Boolean × anything (McCormick) --------
-- Branch: bool * real/int in SUCH THAT, linearized with McCormick.
SELECT p_partkey, p_retailprice, pick, qty
FROM part
WHERE p_size < 5
DECIDE pick IS BOOLEAN, qty IS INTEGER
SUCH THAT qty <= 20
  AND SUM(pick * qty) <= 50
MAXIMIZE SUM(p_retailprice * pick * qty);

-- --- C12: Bilinear constraint — general x*y (Gurobi only) -------------
-- Branch: int * real bilinear in SUCH THAT.
SELECT p_partkey, p_retailprice, qty, disc
FROM part
WHERE p_size < 3
DECIDE qty IS INTEGER, disc IS REAL
SUCH THAT qty <= 20 AND disc <= 5
  AND SUM(qty * disc) <= 100
MAXIMIZE SUM(p_retailprice * qty - qty * disc);

-- --- C13: Uncorrelated scalar subquery in constraint ------------------
-- Branch: scalar subquery RHS, bound once.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(s_acctbal * pick) <= (SELECT AVG(s_acctbal) * 20 FROM supplier)
MAXIMIZE SUM(pick);

-- --- C14: Correlated scalar subquery in constraint --------------------
-- Branch: per-row correlated subquery RHS, decision variable on both
-- sides. Rearranges to `(s_acctbal - subq) * pick >= 0` per row. Two
-- terms reference the same `pick` variable; the per-row LHS coefficient
-- aggregator merges them into a single Gurobi column entry.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT s_acctbal * pick >= (
    SELECT MIN(s2.s_acctbal) FROM supplier s2 WHERE s2.s_nationkey = supplier.s_nationkey
  ) * pick
  AND SUM(pick) >= 5
MAXIMIZE SUM(pick);

-- --- C15: BETWEEN constraint ------------------------------------------
-- Branch: BETWEEN desugars to two comparison constraints.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty BETWEEN 2 AND 9
MAXIMIZE SUM(qty);

-- --- C16: IN on column values (filter only) ---------------------------
-- Branch: IN on data column (not decision var).
SELECT n_nationkey, n_name, pick
FROM nation
WHERE n_regionkey IN (0, 1, 3)
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 10
MAXIMIZE SUM(pick);

-- --- C17: IN on decision variable -------------------------------------
-- Branch: dec_var IN (v1,v2,...) → auxiliary binary indicators.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty IN (0, 2, 5, 9)
  AND SUM(qty) >= 200
MAXIMIZE SUM(qty);

-- --- C18: <> (not equal) on decision variable -------------------------
-- Branch: Big-M disjunction, integer LHS required.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND qty <> 7
MAXIMIZE SUM(qty);

-- --- C19: Strict < on decision variable -------------------------------
-- Branch: strict inequality, requires integer-valued LHS.
SELECT n_nationkey, v
FROM nation
DECIDE v IS INTEGER
SUCH THAT v < 5
MAXIMIZE SUM(v);

-- --- C20: Strict > on decision variable -------------------------------
-- Branch: strict inequality >.
SELECT n_nationkey, v
FROM nation
DECIDE v IS INTEGER
SUCH THAT v > 2
  AND v <= 10
MAXIMIZE SUM(v);

-- --- C21: ABS in per-row constraint (lower-envelope) ------------------
-- Branch: ABS linearization for constraint LHS, |expr| <= K.
SELECT s_suppkey, s_acctbal, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 20
  AND ABS(qty - 5) <= 3
MAXIMIZE SUM(s_acctbal * qty);

-- --- C22: MAX(ABS(expr)) <= K constraint (Path A: lower-envelope only) -
-- Branch: ABS inside MAX aggregate, easy direction MAX(...) <= K stripped
-- to per-row ABS(...) <= K. The constraint upper-bounds the ABS auxiliary
-- directly, so no Big-M sign-indicator is allocated — Path A. The hard-
-- direction MIN(ABS) >= K shape is exercised separately in C35 via Path B.
SELECT s_suppkey, s_acctbal, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND MAX(ABS(qty - 4)) <= 3
MAXIMIZE SUM(s_acctbal * qty);

-- --- C23: SUM(ABS(expr)) <= K aggregate (Path A: lower-envelope only) --
-- Branch: SUM of ABS — solver naturally picks aux_i = |e_i| because each
-- aux is lower-bounded individually and the SUM is upper-bounded. Path A,
-- no Big-M. The hard-direction SUM(ABS) >= K shape is in C36 via Path B.
SELECT s_suppkey, s_acctbal, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND SUM(ABS(qty - 5)) <= 50
MAXIMIZE SUM(s_acctbal * qty);

-- --- C24: Bool × Bool bilinear constraint (AND-linearization) ---------
-- Branch: distinct from McCormick — uses simpler AND linearization.
SELECT s_suppkey, s_acctbal, pick, premium
FROM supplier
DECIDE pick IS BOOLEAN, premium IS BOOLEAN
SUCH THAT SUM(pick) <= 20
  AND SUM(pick * premium) <= 5
MAXIMIZE SUM(s_acctbal * pick * premium);

-- --- C25: Strict > on aggregate ---------------------------------------
-- Branch: SUM(...) > K, integer-valued LHS.
SELECT n_nationkey, pick
FROM nation
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) > 5
MAXIMIZE SUM(n_nationkey * pick);

-- --- C26: Strict < on aggregate ---------------------------------------
-- Branch: SUM(...) < K.
SELECT n_nationkey, pick
FROM nation
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) < 10
MAXIMIZE SUM(n_nationkey * pick);

-- --- C27: <> on aggregate ---------------------------------------------
-- Branch: SUM(...) <> K, Big-M disjunction on aggregate.
SELECT n_nationkey, pick
FROM nation
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 12
  AND SUM(pick) <> 7
MAXIMIZE SUM(n_nationkey * pick);

-- --- C28: BETWEEN on aggregate ----------------------------------------
-- Branch: SUM(...) BETWEEN K1 AND K2 desugars to two aggregate bounds.
SELECT s_suppkey, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) BETWEEN 5 AND 12
MAXIMIZE SUM(s_acctbal * pick);

-- --- C29: Per-row equality on decision variable -----------------------
-- Branch: per-row qty = K — fully pins variables that match.
SELECT s_suppkey, s_nationkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND qty = 7 WHEN (s_nationkey <= 5)
MAXIMIZE SUM(qty);

-- --- C30: Per-row quadratic constraint POWER(linear, 2) <= K ----------
-- Branch: per-row QCQP — distinct from C10's aggregate SUM(POWER(...)).
-- The per-row form emits one quadratic constraint per data row.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 10
  AND POWER(qty - 5, 2) <= 9
MAXIMIZE SUM(p_retailprice * qty);

-- --- C31: Hard MAX(...) constraint with WHEN --------------------------
-- Branch: hard-direction MAX (Big-M indicators) composed with WHEN row
-- filter. M10 covers MIN-easy + WHEN; C7 covers hard MAX without WHEN.
-- The combination affects which rows participate in the indicator pool.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MAX(s_acctbal * pick) >= 8000 WHEN (s_nationkey <= 10)
  AND SUM(pick) <= 30
MAXIMIZE SUM(pick);

-- --- C32: Bilinear constraint with WHEN + PER -------------------------
-- Branch: McCormick bilinear (bool * int) under both WHEN row filter and
-- PER grouping. C11/P9 cover bilinear without filters; M5/M7 cover
-- WHEN/PER on linear. This combination exercises bilinear constraint
-- emission per group with row filtering.
SELECT p_partkey, p_size, p_retailprice, pick, qty
FROM part
WHERE p_size < 10
DECIDE pick IS BOOLEAN, qty IS INTEGER
SUCH THAT qty <= 20
  AND SUM(pick * qty) <= 30 WHEN (p_retailprice > 1000) PER p_size
MAXIMIZE SUM(p_retailprice * pick * qty);

-- --- C33: Per-row ABS hard direction (ABS >= K) -----------------------
-- Branch: hard-direction ABS in constraint, Big-M sign-indicator pair
-- (aux <= e + 2M(1-y), aux <= -e + 2M*y) on top of the lower envelope.
-- Was previously rejected (R26 in old 05_rejected.sql); now solves.
-- Expected: qty in {0,1,2,8,9,10} feasible per row, MAX picks 10. Sum=1000.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10 AND ABS(qty - 5) >= 3
MAXIMIZE SUM(qty);

-- --- C34: ABS equality (ABS = K) --------------------------------------
-- Branch: hard-direction ABS in equality. Big-M pins aux = |inner|
-- exactly; the equality then forces |inner| = K. qty in {2, 8}.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10 AND ABS(qty - 5) = 3
MAXIMIZE SUM(qty);

-- --- C35: MIN(ABS) >= K (easy-direction stripped to per-row hard ABS) -
-- Branch: MIN >= K rewrites under easy-MIN to per-row ABS >= K. Each
-- per-row ABS is hard-direction → Big-M envelope per row. qty != 4 for
-- every row. MAX picks qty=10. Sum=1000.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10 AND MIN(ABS(qty - 4)) >= 1
MAXIMIZE SUM(qty);

-- --- C36: SUM(ABS) >= K (aggregate hard direction) --------------------
-- Branch: aggregate hard direction over ABS. Each aux pinned via Big-M;
-- the SUM aggregate then operates on pinned auxes. With qty<=10, max
-- SUM(|qty-5|) = 100*5 = 500, so 200 is comfortably feasible.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10 AND SUM(ABS(qty - 5)) >= 200
MAXIMIZE SUM(qty);

-- --- C37: ABS in BETWEEN ----------------------------------------------
-- Branch: BETWEEN over ABS — desugars to two comparisons, both bounding
-- aux. Lower bound is hard direction; upper bound is sound. Big-M on
-- aux makes both comparisons exact. qty in {1,2,3,7,8,9}. MAX picks 9.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10 AND ABS(qty - 5) BETWEEN 2 AND 4
MAXIMIZE SUM(qty);
