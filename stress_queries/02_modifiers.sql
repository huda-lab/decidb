-- =====================================================================
-- MODIFIERS — WHEN / PER / aggregate-local WHEN
-- =====================================================================

-- --- M1: WHEN on a per-row constraint ---------------------------------
-- Branch: row-level WHEN filter on a per-row bound.
SELECT s_suppkey, s_nationkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 20
  AND qty <= 5 WHEN s_nationkey <= 10
MAXIMIZE SUM(qty);

-- --- M2: WHEN on an aggregate constraint ------------------------------
-- Branch: filter rows contributing to an aggregate.
-- NOTE: WHEN's condition grammar (b_expr) excludes top-level AND/OR and
--       disallows IN/IS NULL/etc. without parens. Wrap complex predicates
--       in parentheses.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 4 WHEN (s_nationkey IN (1, 2, 3))
MAXIMIZE SUM(s_acctbal * pick);

-- --- M3: WHEN on objective --------------------------------------------
-- Branch: objective-level WHEN — coefficient zeroed for filtered rows.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 10
MAXIMIZE SUM(s_acctbal * pick) WHEN s_acctbal > 3000;

-- --- M4: Aggregate-local WHEN (filter on individual term) -------------
-- Branch: per-term WHEN inside an additive constraint.
-- NOTE: parens required around the comparison in each aggregate-local WHEN.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(s_acctbal * pick) WHEN (s_nationkey = 1)
        + SUM(s_acctbal * pick) WHEN (s_nationkey = 2)
        <= 15000
MAXIMIZE SUM(pick);

-- --- M5: PER single column --------------------------------------------
-- Branch: one aggregate constraint per distinct value.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 3 PER s_nationkey
MAXIMIZE SUM(s_acctbal * pick);

-- --- M6: PER multi-column ---------------------------------------------
-- Branch: composite grouping key.
SELECT l_orderkey, l_linestatus, l_returnflag, l_extendedprice, pick
FROM lineitem
WHERE l_orderkey < 200
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 5 PER (l_linestatus, l_returnflag)
MAXIMIZE SUM(l_extendedprice * pick);

-- --- M7: WHEN + PER composition ---------------------------------------
-- Branch: WHEN filter composed with PER grouping (default WHEN-then-PER).
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 2 WHEN s_acctbal > 1000 PER s_nationkey
MAXIMIZE SUM(s_acctbal * pick);

-- --- M8: PER with MAX aggregate ---------------------------------------
-- Branch: MIN/MAX aggregate under PER grouping.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MAX(s_acctbal * pick) <= 7000 PER s_nationkey
  AND SUM(pick) >= 5
MAXIMIZE SUM(pick);

-- --- M9: Empty-group skipping (WHEN filters out some, not all, groups) --
-- Branch: individual empty groups silently skipped — the constraint emits
-- one row per non-empty group. The WHEN bound is set high enough that a
-- subset of nations have zero qualifying suppliers; those groups get no
-- constraint, while qualifying groups do. (Filtering out *every* row
-- across all groups is rejected — see when/done.md "Empty Row Sets".)
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 2 WHEN s_acctbal > 5000 PER s_nationkey
MAXIMIZE SUM(pick);

-- --- M10: WHEN on MIN aggregate ---------------------------------------
-- Branch: WHEN filter on a MIN aggregate (easy direction, per-row bound).
-- Fixed feasibility: relaxed bound so a non-empty feasible set exists.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MIN(s_acctbal * pick) >= 0 WHEN (s_acctbal > 1000)
  AND SUM(pick) <= 30
MAXIMIZE SUM(pick);

-- --- M11: Nested aggregate MAX(SUM(...)) PER (objective) --------------
-- Branch: OUTER(INNER(expr)) PER col — MAX(SUM(...)) PER grouping.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 30
MINIMIZE MAX(SUM(s_acctbal * pick)) PER s_nationkey;

-- --- M12: Nested aggregate SUM(MAX(...)) PER (objective) --------------
-- Branch: outer SUM, inner MAX.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 30
MINIMIZE SUM(MAX(s_acctbal * pick)) PER s_nationkey;

-- --- M13: Nested AVG as inner (coefficient-scaled) --------------------
-- Branch: AVG inner → 1/n_g scaling.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 20
MAXIMIZE SUM(AVG(s_acctbal * pick)) PER s_nationkey;

-- --- M14: Aggregate-local WHEN in objective ---------------------------
-- Branch: two SUM terms with independent WHEN masks in one objective.
-- NOTE: WHEN conditions need parens.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 10
MAXIMIZE SUM(s_acctbal * pick) WHEN (s_nationkey <= 5)
       + SUM(s_acctbal * pick) WHEN (s_nationkey > 20);

-- --- M15: WHEN with NULL in predicate (treated as false) --------------
-- Branch: NULL-producing WHEN predicate excludes rows.
-- NOTE: IS NOT NULL inside WHEN needs parens.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 5 WHEN (NULLIF(s_nationkey, 0) IS NOT NULL)
MAXIMIZE SUM(s_acctbal * pick);

-- --- M16: PER with a NULL grouping key (row excluded) -----------------
-- Branch: rows with NULL PER key are dropped from grouping.
-- NOTE: PER must reference a FROM-clause column, not a SELECT-list alias.
--       Use a subquery to materialize the nullable key first.
SELECT s_suppkey, s_acctbal, pick, grp_key
FROM (
  SELECT s_suppkey, s_acctbal,
         CASE WHEN s_nationkey > 20 THEN NULL ELSE s_nationkey END AS grp_key
  FROM supplier
) sub
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 2 PER grp_key
MAXIMIZE SUM(s_acctbal * pick);

-- --- M17: PER with SUM on multi-column grouping + aggregate -----------
-- Branch: composite PER + aggregate with data coefficients.
SELECT l_orderkey, l_linestatus, l_returnflag, l_quantity, l_extendedprice, qty
FROM lineitem
WHERE l_orderkey < 200
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND SUM(qty * l_quantity) <= 200 PER (l_linestatus, l_returnflag)
MAXIMIZE SUM(l_extendedprice * qty);

-- --- M18: WHEN chained with AND/OR on data columns --------------------
-- Branch: richer WHEN predicate with boolean composition.
-- NOTE: AND/OR at top level of WHEN are excluded; wrap entire composite
--       predicate in parens.
SELECT o_orderkey, o_orderpriority, o_totalprice, pick
FROM orders
WHERE o_orderkey < 500
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 20 WHEN ((o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH') AND o_totalprice > 10000)
MAXIMIZE SUM(o_totalprice * pick);

-- --- M19: AVG inside aggregate-local WHEN ------------------------------
-- Branch: aggregate-local WHEN on an AVG term. AVG-as-inner has 1/n_g
-- coefficient scaling that interacts with the WHEN row count — a
-- distinct path from M4/M14 (SUM in aggregate-local WHEN) and C3
-- (plain AVG without WHEN).
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT AVG(s_acctbal * pick) WHEN (s_nationkey <= 10) <= 1500
MAXIMIZE SUM(pick);
