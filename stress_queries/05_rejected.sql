-- =====================================================================
-- REJECTED FEATURES — error-path coverage
-- Each query SHOULD fail with a binder/parser/optimizer error.
-- "RAN" means the query executed successfully (a bug — these are
-- documented as unsupported).
-- =====================================================================

-- --- R1: COUNT() inside DECIDE aggregate ------------------------------
-- Expected: rejected — COUNT is not allowed over decision vars.
SELECT s_suppkey, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT COUNT(pick) <= 10
MAXIMIZE SUM(pick);

-- --- R2: SQRT over decision variable ----------------------------------
-- Expected: rejected — non-linear scalar function.
-- Verified 2026-05-07: now returns clean
--   "Binder Error: Scalar function 'sqrt' over a DECIDE variable is not
--    supported: it would make the model non-linear. Only ABS() and
--    POWER(..., 2) can wrap a decision variable."
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT SUM(SQRT(qty)) <= 100
MAXIMIZE SUM(qty);

-- --- R3: EXP over decision variable -----------------------------------
-- Expected: rejected.
-- Verified 2026-05-07: now returns clean
--   "Binder Error: Scalar function 'exp' over a DECIDE variable is not supported..."
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT SUM(EXP(qty)) <= 100
MAXIMIZE SUM(qty);

-- --- R4: LN/LOG over decision variable --------------------------------
-- Expected: rejected.
-- Verified 2026-05-07: now returns clean
--   "Binder Error: Scalar function 'ln' over a DECIDE variable is not supported..."
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10
MAXIMIZE SUM(LN(qty + 1));

-- --- R5: Trig (SIN) over decision variable ----------------------------
-- Expected: rejected.
-- Verified 2026-05-07: now returns clean
--   "Binder Error: Scalar function 'sin' over a DECIDE variable is not supported..."
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10
MAXIMIZE SUM(SIN(qty));

-- --- R6: FLOOR over decision variable ---------------------------------
-- Expected: rejected.
-- Verified 2026-05-07: now returns clean
--   "Binder Error: Scalar function 'floor' over a DECIDE variable is not supported..."
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10
MAXIMIZE SUM(FLOOR(qty));

-- --- R7: ROUND over decision variable ---------------------------------
-- Expected: rejected.
-- Verified 2026-05-07: now returns clean
--   "Binder Error: Scalar function 'round' over a DECIDE variable is not supported..."
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10
MAXIMIZE SUM(ROUND(qty));

-- --- R8: POWER with exponent != 2 -------------------------------------
-- Expected: rejected — only ^2 is supported.
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10
MAXIMIZE SUM(POWER(qty, 3));

-- --- R9: POWER with fractional exponent -------------------------------
-- Expected: rejected.
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10
MAXIMIZE SUM(POWER(qty, 0.5));

-- --- R10: Degree > 2 — three-way product x*y*z ------------------------
-- Expected: rejected — degree 3.
SELECT p_partkey, x, y, z
FROM part
WHERE p_size < 3
DECIDE x IS REAL, y IS REAL, z IS REAL
SUCH THAT x <= 5 AND y <= 5 AND z <= 5
MAXIMIZE SUM(x * y * z);

-- --- R11: Degree > 2 — x * POWER(y, 2) --------------------------------
-- Expected: rejected — degree 3.
SELECT p_partkey, x, y
FROM part
WHERE p_size < 3
DECIDE x IS REAL, y IS REAL
SUCH THAT x <= 5 AND y <= 5
MAXIMIZE SUM(x * POWER(y, 2));

-- --- R12: Degree > 2 — POWER(x,2) * POWER(y,2) ------------------------
-- Expected: rejected — degree 4.
SELECT p_partkey, x, y
FROM part
WHERE p_size < 3
DECIDE x IS REAL, y IS REAL
SUCH THAT x <= 5 AND y <= 5
MAXIMIZE SUM(POWER(x, 2) * POWER(y, 2));

-- --- R13: Degree > 2 — POWER(POWER(x,2),2) ----------------------------
-- Expected: rejected — degree 4 / nested POWER.
SELECT p_partkey, x
FROM part
WHERE p_size < 3
DECIDE x IS REAL
SUCH THAT x <= 5
MAXIMIZE SUM(POWER(POWER(x, 2), 2));

-- --- R14: Decision variable in WHEN condition -------------------------
-- Expected: rejected — WHEN must filter on data, not unknowns.
SELECT s_suppkey, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 5 WHEN (pick = 1)
MAXIMIZE SUM(pick);

-- --- R15: Division by decision variable -------------------------------
-- Expected: rejected — non-linear.
-- Verified 2026-05-07: now returns clean
--   "Binder Error: Division by a DECIDE variable is not supported: it would
--    make the model non-linear. The divisor must not reference a decision
--    variable."
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10 AND qty >= 1
MAXIMIZE SUM(1 / qty);

-- --- R16: Division of one decision var by another ---------------------
-- Expected: rejected.
-- Verified 2026-05-07: same clean Binder Error as R15.
SELECT p_partkey, x, y
FROM part
WHERE p_size < 3
DECIDE x IS REAL, y IS REAL
SUCH THAT x <= 10 AND y >= 1
MAXIMIZE SUM(x / y);

-- --- R17: Negative-domain variable (no explicit bounds syntax) --------
-- Expected: rejected (or interpreted as non-negative).
-- ACTUAL: silently accepted — `pick >= -5` is trivially satisfied because
--         vars are implicitly [0, +∞). The query runs and pick saturates
--         at upper bound (5). User's apparent intent (allow negative) is
--         silently ignored with no warning. Worth surfacing a hint.
SELECT s_suppkey, pick
FROM supplier
DECIDE pick IS REAL
SUCH THAT pick >= -5 AND pick <= 5
MAXIMIZE SUM(pick);

-- --- R18: AVG inside POWER (nested non-linear over decision var) ------
-- Expected: rejected.
-- BUG (UX): "INTERNAL Error: DECIDE objective contains a non-aggregate
--           term: power(sum(qty), CAST(2 AS DOUBLE))" — should be a clean
--           Invalid Input error.
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10
MAXIMIZE POWER(AVG(qty), 2);

-- --- R19: Multiple quadratic groups in one objective ------------------
-- Expected: rejected — only one quadratic group per objective allowed.
SELECT p_partkey, x, y
FROM part
WHERE p_size < 3
DECIDE x IS REAL, y IS REAL
SUCH THAT x <= 5 AND y <= 5
MINIMIZE SUM(POWER(x, 2)) + SUM(POWER(y, 2));

-- --- R20: IN on aggregate (SUM) ---------------------------------------
-- Expected: rejected — IN is per-row only, not on aggregates.
SELECT s_suppkey, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) IN (5, 10, 15)
MAXIMIZE SUM(pick);

-- --- R21: Strict < on REAL/non-integer LHS ----------------------------
-- Expected: rejected — strict inequality requires integer-valued LHS.
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty < 5
MAXIMIZE SUM(qty);

-- --- R22: <> on REAL/non-integer LHS ----------------------------------
-- Expected: rejected — <> requires integer-valued LHS.
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <> 3.5
MAXIMIZE SUM(qty);

-- --- R23: CEIL/CEILING over decision variable -------------------------
-- Expected: rejected — non-linear scalar function.
-- Verified 2026-05-07: now returns clean
--   "Binder Error: Scalar function 'ceil' over a DECIDE variable is not supported..."
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10
MAXIMIZE SUM(CEIL(qty));

-- --- R24: LOG over decision variable ----------------------------------
-- Expected: rejected — non-linear; verifies LOG is treated like LN.
-- Verified 2026-05-07: now returns clean
--   "Binder Error: Scalar function 'log' over a DECIDE variable is not supported..."
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <= 10 AND qty >= 1
MAXIMIZE SUM(LOG(qty));

-- --- R25: Empty aggregate after WHEN (non-PER) ------------------------
-- Expected per docs: rejected — WHEN filters out every row leaving SUM(∅).
-- Verified 2026-05-07: behavior matches docs — raises
--   "Invalid Input Error: DECIDE empty row set for aggregate in constraint."
-- (Earlier observation that this silently no-op'd is no longer reproducible.)
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 5 WHEN (s_acctbal > 9999999)
MAXIMIZE SUM(s_acctbal * pick);

-- --- R26/R27/R28 promoted to positive cases ---------------------------
-- These three were rejection cases under the old ABS soundness gate (the
-- conservative bind-time stopgap). They are now full hard-direction ABS
-- constraints implemented via Big-M with sign-indicator binaries, see
-- C33–C35 in 01_constraints.sql. Removed from 05_rejected.sql because
-- they no longer reject.
