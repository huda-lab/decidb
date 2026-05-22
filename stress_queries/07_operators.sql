-- =====================================================================
-- CONSTRAINT OPERATORS — relational/composition operators in SUCH THAT
-- =====================================================================
-- Focused, minimal queries that exercise each comparison operator branch
-- on its own. Shape coverage (per-row vs aggregate, MIN/MAX, quadratic,
-- bilinear, subqueries) lives in 01_constraints.sql.

-- --- OP1: Equality (=) on per-row ------------------------------------
-- Branch: per-row equality pins variables to a scalar.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty = 4
MAXIMIZE SUM(s_acctbal * qty);

-- --- OP2: Less-than-or-equal (<=) per-row ----------------------------
-- Branch: per-row upper bound; canonical linear inequality.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS INTEGER
SUCH THAT qty <= 8
MAXIMIZE SUM(p_retailprice * qty);

-- --- OP3: Greater-than-or-equal (>=) per-row -------------------------
-- Branch: per-row lower bound on a real var.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 10
  AND qty >= 2
MAXIMIZE SUM(p_retailprice * qty);

-- --- OP4: Strict less-than (<) per-row -------------------------------
-- Branch: < requires integer-valued LHS; encodes as <= K-1.
SELECT n_nationkey, v
FROM nation
DECIDE v IS INTEGER
SUCH THAT v < 5
MAXIMIZE SUM(v);

-- --- OP5: Strict greater-than (>) per-row ----------------------------
-- Branch: > requires integer-valued LHS; encodes as >= K+1.
SELECT n_nationkey, v
FROM nation
DECIDE v IS INTEGER
SUCH THAT v > 2
  AND v <= 10
MAXIMIZE SUM(v);

-- --- OP6: Not-equal (<>) per-row -------------------------------------
-- Branch: Big-M disjunction; integer LHS required.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND qty <> 7
MAXIMIZE SUM(qty);

-- --- OP7: BETWEEN per-row --------------------------------------------
-- Branch: BETWEEN K1 AND K2 desugars to two inequality constraints.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty BETWEEN 2 AND 9
MAXIMIZE SUM(qty);

-- --- OP8: BETWEEN on aggregate ---------------------------------------
-- Branch: SUM(...) BETWEEN K1 AND K2 — two aggregate bounds.
SELECT s_suppkey, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) BETWEEN 5 AND 12
MAXIMIZE SUM(s_acctbal * pick);

-- --- OP9: IN on data column (filter) ---------------------------------
-- Branch: IN over data column — handled in WHERE/predicate path.
SELECT n_nationkey, n_name, pick
FROM nation
WHERE n_regionkey IN (0, 1, 3)
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 10
MAXIMIZE SUM(pick);

-- --- OP10: IN on decision variable -----------------------------------
-- Branch: dec_var IN (v1,v2,...) — auxiliary binary indicators.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty IN (0, 2, 5, 9)
  AND SUM(qty) >= 100
MAXIMIZE SUM(qty);

-- --- OP11: AND of multiple per-row constraints -----------------------
-- Branch: AND chains constraints — each becomes an independent linear row.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS INTEGER
SUCH THAT qty >= 2 AND qty <= 8 AND qty <> 5
MAXIMIZE SUM(p_retailprice * qty);

-- --- OP12: AND mixing per-row and aggregate constraints --------------
-- Branch: per-row + aggregate combined under one SUCH THAT via AND.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT pick <= 1
  AND SUM(pick) >= 5
  AND SUM(s_acctbal * pick) <= 50000
MAXIMIZE SUM(s_acctbal * pick);

-- --- OP13: Equality on aggregate (=) ---------------------------------
-- Branch: SUM(...) = K — equality on aggregate.
SELECT n_nationkey, flag
FROM nation
DECIDE flag IS BOOLEAN
SUCH THAT SUM(flag) = 7
MAXIMIZE SUM(n_nationkey * flag);

-- --- OP14: Strict > on aggregate -------------------------------------
-- Branch: SUM(...) > K — integer-valued LHS check.
SELECT n_nationkey, pick
FROM nation
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) > 5
MAXIMIZE SUM(n_nationkey * pick);

-- --- OP15: Strict < on aggregate -------------------------------------
-- Branch: SUM(...) < K.
SELECT n_nationkey, pick
FROM nation
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) < 10
MAXIMIZE SUM(n_nationkey * pick);

-- --- OP16: <> on aggregate -------------------------------------------
-- Branch: SUM(...) <> K — Big-M disjunction at aggregate level.
SELECT n_nationkey, pick
FROM nation
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 12
  AND SUM(pick) <> 7
MAXIMIZE SUM(n_nationkey * pick);

-- --- OP17: <> on AVG aggregate ---------------------------------------
-- Branch: AVG(...) <> K — distinct from OP16 (SUM <>) because AVG with
-- <> uses the special-cased ne_avg_rhs_scale path: the <> rewrite
-- (x <> K → x <= K-1 OR x >= K+1) must scale by row count under AVG.
SELECT n_nationkey, pick
FROM nation
DECIDE pick IS BOOLEAN
SUCH THAT AVG(pick) <> 0
  AND SUM(pick) <= 10
MAXIMIZE SUM(n_nationkey * pick);
