-- =====================================================================
-- NULL & EDGE BEHAVIOR — NULL handling, empty groups, LHS-type checks
-- =====================================================================
-- Every query targets a specific edge case in the docs (NULL semantics,
-- domain checks, integer-LHS requirement). When the actual behavior
-- diverges from documented behavior it is annotated below.

-- --- N1: NULL in WHEN predicate → row excluded -----------------------
-- Branch: NULL → false; the row drops out of the aggregate.
-- Setup: NULLIF turns s_nationkey=0 into NULL; those rows must be excluded.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 5 WHEN (NULLIF(s_nationkey, 0) IS NOT NULL)
MAXIMIZE SUM(s_acctbal * pick);

-- --- N2: NULL in WHEN via direct NULL-producing expression -----------
-- Branch: arithmetic on NULL propagates → predicate evaluates to NULL.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 8 WHEN ((CAST(NULL AS INTEGER) + s_nationkey) > 0)
MAXIMIZE SUM(s_acctbal * pick);

-- --- N3: NULL in PER grouping key → row excluded ---------------------
-- Branch: rows with NULL group key are dropped before grouping.
-- Note: PER must reference a FROM-clause column; nullable key materialized
--       in a subquery.
SELECT s_suppkey, s_acctbal, pick, grp_key
FROM (
  SELECT s_suppkey, s_acctbal,
         CASE WHEN s_nationkey > 20 THEN NULL ELSE s_nationkey END AS grp_key
  FROM supplier
) sub
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 2 PER grp_key
MAXIMIZE SUM(s_acctbal * pick);

-- --- N4: NULL in PER multi-column key (any-NULL excludes row) --------
-- Branch: composite PER key — any NULL component excludes the row.
SELECT s_suppkey, s_acctbal, pick, k1, k2
FROM (
  SELECT s_suppkey, s_acctbal,
         CASE WHEN s_nationkey > 15 THEN NULL ELSE s_nationkey END AS k1,
         CASE WHEN s_acctbal > 5000 THEN NULL ELSE 1 END           AS k2
  FROM supplier
) sub
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 2 PER (k1, k2)
MAXIMIZE SUM(s_acctbal * pick);

-- --- N5: Empty aggregate after WHEN (PER-grouped, partial empty groups) --
-- Branch: WHEN excludes some, not all, of the PER groups. Empty groups
-- are skipped silently; non-empty groups still produce constraints. Per
-- docs (when/done.md "Empty Row Sets"), the *all-empty* case is rejected
-- — that's the dedicated rejection in 05_rejected.sql / N6.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 2 WHEN (s_acctbal > 5000) PER s_nationkey
MAXIMIZE SUM(pick);

-- --- N6: Empty aggregate after WHEN (non-PER, all rows filtered) -----
-- Per docs: empty aggregate after WHEN is rejected with an error.
-- Verified 2026-05-07: actual behavior matches docs — raises
--   "DECIDE empty row set for aggregate in constraint."
-- (Earlier observation that this silently no-op'd is no longer reproducible.)
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 5 WHEN (s_acctbal > 9999999)
MAXIMIZE SUM(s_acctbal * pick);

-- --- N7: Strict < on REAL LHS (rejected) -----------------------------
-- Per docs: strict inequality requires integer-valued LHS.
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty < 5
MAXIMIZE SUM(qty);

-- --- N8: Strict > on REAL LHS (rejected) -----------------------------
-- Per docs: strict inequality requires integer-valued LHS.
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty > 1
  AND qty <= 10
MAXIMIZE SUM(qty);

-- --- N9: <> on REAL LHS (rejected) -----------------------------------
-- Per docs: <> requires integer-valued LHS.
SELECT p_partkey, qty
FROM part
DECIDE qty IS REAL
SUCH THAT qty <> 3.5
MAXIMIZE SUM(qty);

-- --- N10: Strict < on aggregate over REAL summands -------------------
-- Per docs: integer-valued LHS required. SUM over REAL vars is non-integer
-- in general, so this should be rejected (or auto-rewritten to <=K-eps with
-- a clear semantics statement). Documented here to verify the actual path.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT SUM(qty) < 25
MAXIMIZE SUM(qty);

-- --- N11: NULL coefficient in objective (data-side) ------------------
-- Branch: NULL data column flowing into the objective coefficient. The
-- documented semantics is silent on this — included to capture observed
-- behavior. NULL coefficients should either be treated as 0 or the row
-- should be excluded.
SELECT s_suppkey, s_acctbal, pick, coef
FROM (
  SELECT s_suppkey, s_acctbal,
         CASE WHEN s_acctbal < 1000 THEN NULL ELSE s_acctbal END AS coef
  FROM supplier
) sub
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 10
MAXIMIZE SUM(coef * pick);

-- --- N12: NULL on RHS of per-row constraint --------------------------
-- Branch: NULL RHS — comparison against NULL is unknown. Constraint
-- should either be skipped for that row or rejected.
SELECT s_suppkey, s_acctbal, qty, cap
FROM (
  SELECT s_suppkey, s_acctbal,
         CASE WHEN s_nationkey > 10 THEN NULL ELSE 5 END AS cap
  FROM supplier
) sub
DECIDE qty IS INTEGER
SUCH THAT qty <= cap
MAXIMIZE SUM(s_acctbal * qty);

-- --- N13: Negative-domain attempt (no explicit-bounds syntax) --------
-- Per docs: variables are implicitly non-negative. Trying to allow a
-- negative value is silently ignored (no warning).
SELECT s_suppkey, pick
FROM supplier
DECIDE pick IS REAL
SUCH THAT pick >= -5 AND pick <= 5
MAXIMIZE SUM(pick);
