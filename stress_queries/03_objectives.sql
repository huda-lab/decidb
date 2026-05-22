-- =====================================================================
-- OBJECTIVES — linear / feasibility / MIN-MAX / QP / bilinear / mixed
-- =====================================================================

-- --- O1: MAXIMIZE SUM (linear) ----------------------------------------
-- Branch: standard linear maximization over decision variables.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 10
MAXIMIZE SUM(s_acctbal * pick);

-- --- O2: MINIMIZE SUM (linear) ----------------------------------------
-- Branch: linear minimization.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 10
MINIMIZE SUM(s_acctbal * pick);

-- --- O3: Feasibility (no objective) -----------------------------------
-- Branch: no MAXIMIZE/MINIMIZE; any feasible solution accepted.
SELECT s_suppkey, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) = 10;

-- --- O4: MINIMIZE MAX (easy, global auxiliary) ------------------------
-- Branch: min of max → global z with z >= expr_i per row.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 10
MINIMIZE MAX(s_acctbal * pick);

-- --- O5: MAXIMIZE MIN (easy, global auxiliary) ------------------------
-- Branch: max of min → global z with z <= expr_i per row.
SELECT s_suppkey, s_acctbal, level
FROM supplier
WHERE s_acctbal > 0
DECIDE level IS INTEGER
SUCH THAT level <= 5 AND SUM(level) <= 200
MAXIMIZE MIN(s_acctbal * level);

-- --- O6: MAXIMIZE MAX (hard, indicator + SUM(y) >= 1) -----------------
-- Branch: max of max → global z + per-row binary + at-least-one.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 3 AND SUM(pick) <= 20
MAXIMIZE MAX(s_acctbal * pick);

-- --- O7: MINIMIZE MIN (hard) ------------------------------------------
-- Branch: min of min — same Big-M shape as O6.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 5
MINIMIZE MIN(s_acctbal * pick);

-- --- O8: MAXIMIZE AVG (rewritten to SUM) ------------------------------
-- Branch: AVG → SUM (proportional).
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 10
MAXIMIZE AVG(s_acctbal * pick);

-- --- O9: Additive linear objective (two SUM terms) --------------------
-- Branch: SUM(a*x) + SUM(b*x) — combined linearly.
SELECT s_suppkey, s_acctbal, s_nationkey, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 10
MAXIMIZE SUM(s_acctbal * pick) + SUM(s_nationkey * pick);

-- --- O10: Additive objective with MIN + SUM mix -----------------------
-- Branch: composed aggregates in same objective.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 5 AND SUM(pick) <= 20
MAXIMIZE SUM(s_acctbal * pick) + MIN(s_acctbal * pick);

-- --- O11: Convex QP — MINIMIZE SUM(POWER(expr, 2)) --------------------
-- Branch: convex quadratic objective; both solvers.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) = 50
MINIMIZE SUM(POWER(qty - 3, 2));

-- --- O12: Concave QP via negation — MAXIMIZE SUM(-POWER(...)) ---------
-- Branch: negated quadratic → concave; both solvers.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) = 50
MAXIMIZE SUM(-POWER(qty - 3, 2));

-- --- O13: Non-convex QP — MAXIMIZE SUM(POWER(...)) (Gurobi only) ------
-- Branch: PSD Q + MAXIMIZE → non-convex; requires NonConvex=2.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) <= 100
MAXIMIZE SUM(POWER(qty, 2));

-- --- O14: QP using ** syntax ------------------------------------------
-- Branch: expr**2 alternate syntax for POWER(expr, 2).
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) = 30
MINIMIZE SUM((qty - 2) ** 2);

-- --- O15: QP using (expr)*(expr) self-product -------------------------
-- Branch: self-multiplication form.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) = 30
MINIMIZE SUM((qty - 2) * (qty - 2));

-- --- O16: Mixed linear + quadratic objective --------------------------
-- Branch: linear term alongside quadratic term in one objective.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) = 50
MINIMIZE SUM(POWER(qty - 3, 2)) - SUM(p_retailprice * qty);

-- --- O17: Bilinear objective bool * anything (McCormick) --------------
-- Branch: pick * qty — linearized, both solvers.
SELECT p_partkey, p_retailprice, pick, qty
FROM part
WHERE p_size < 5
DECIDE pick IS BOOLEAN, qty IS INTEGER
SUCH THAT qty <= 20 AND SUM(pick) <= 50
MAXIMIZE SUM(p_retailprice * pick * qty);

-- --- O18: Bilinear objective general (int * real, Gurobi) -------------
-- Branch: non-bool bilinear product.
SELECT p_partkey, p_retailprice, qty, disc
FROM part
WHERE p_size < 3
DECIDE qty IS INTEGER, disc IS REAL
SUCH THAT qty <= 20 AND disc <= 5
MAXIMIZE SUM(p_retailprice * qty - qty * disc);

-- --- O19: Scaled quadratic (K * POWER) --------------------------------
-- Branch: coefficient-scaled quadratic.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) = 40
MINIMIZE SUM(10 * POWER(qty - 1, 2));

-- --- O20: MAXIMIZE MIN with data coefficients (easy objective) --------
-- Branch: easy MIN objective with realistic coefficients.
SELECT s_suppkey, s_acctbal, s_nationkey, pick
FROM supplier
WHERE s_acctbal > 0
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 10 AND SUM(pick) <= 20
MAXIMIZE MIN(s_acctbal * pick);

-- --- O21: MINIMIZE MAX with WHEN --------------------------------------
-- Branch: easy MIN/MAX composed with WHEN filter.
SELECT s_suppkey, s_acctbal, s_nationkey, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 10
MINIMIZE MAX(s_acctbal * pick) WHEN (s_nationkey <= 10);

-- --- O22: MINIMIZE SUM(ABS(expr)) -------------------------------------
-- Branch: ABS in objective, lower-envelope linearization.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND SUM(qty) >= 50
MINIMIZE SUM(ABS(qty - 4));

-- --- O23: MAXIMIZE SUM(-ABS(expr)) ------------------------------------
-- Branch: ABS in MAXIMIZE — Big-M form.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND SUM(qty) >= 30
MAXIMIZE SUM(-ABS(qty - 5));

-- --- O24: POW(expr, 2) alias ------------------------------------------
-- Branch: POW vs POWER — same binder node? confirms alias parity.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) = 30
MINIMIZE SUM(POW(qty - 2, 2));

-- --- O25: Bool × Bool bilinear in objective (AND-linearization) -------
-- Branch: bool*bool product in objective.
SELECT s_suppkey, s_acctbal, pick, premium
FROM supplier
DECIDE pick IS BOOLEAN, premium IS BOOLEAN
SUCH THAT SUM(pick) <= 30 AND SUM(premium) <= 10
MAXIMIZE SUM(s_acctbal * pick * premium);

-- =====================================================================
-- Nested aggregate OUTER(INNER(expr)) PER col combinations.
-- Existing coverage: M11 MAX(SUM), M12 SUM(MAX), M13 SUM(AVG).
-- Below: remaining SUM/MIN/MAX combos plus AVG-as-outer.
-- =====================================================================

-- --- O26: SUM(SUM(...)) PER -------------------------------------------
-- Branch: SUM outer, SUM inner — both linear, no auxiliaries.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 30
MAXIMIZE SUM(SUM(s_acctbal * pick)) PER s_nationkey;

-- --- O27: SUM(MIN(...)) PER -------------------------------------------
-- Branch: outer SUM, inner MIN — per-group MIN auxiliaries.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 10
MAXIMIZE SUM(MIN(s_acctbal * pick)) PER s_nationkey;

-- --- O28: MIN(SUM(...)) PER -------------------------------------------
-- Branch: outer MIN of per-group sums (max-min fairness across groups).
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 10
MAXIMIZE MIN(SUM(s_acctbal * pick)) PER s_nationkey;

-- --- O29: MIN(MIN(...)) PER -------------------------------------------
-- Branch: outer MIN, inner MIN — easy direction, both levels stripped.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
WHERE s_acctbal > 0
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 10
MAXIMIZE MIN(MIN(s_acctbal * pick)) PER s_nationkey;

-- --- O30: MIN(MAX(...)) PER -------------------------------------------
-- Branch: outer MIN of per-group MAX — minimax over groups.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 10
MINIMIZE MIN(MAX(s_acctbal * pick)) PER s_nationkey;

-- --- O31: MAX(MIN(...)) PER -------------------------------------------
-- Branch: outer MAX of per-group MIN — maximin.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
WHERE s_acctbal > 0
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 10
MAXIMIZE MAX(MIN(s_acctbal * pick)) PER s_nationkey;

-- --- O32: MAX(MAX(...)) PER -------------------------------------------
-- Branch: outer MAX of per-group MAX — global maximum (degenerates).
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 5
MAXIMIZE MAX(MAX(s_acctbal * pick)) PER s_nationkey;

-- --- O33: AVG(SUM(...)) PER -------------------------------------------
-- Branch: AVG outer → SUM with constant divisor (≡ SUM(SUM)/n_groups).
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 30
MAXIMIZE AVG(SUM(s_acctbal * pick)) PER s_nationkey;

-- --- O34: AVG(MIN(...)) PER -------------------------------------------
-- Branch: AVG outer, MIN inner.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
WHERE s_acctbal > 0
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 10
MAXIMIZE AVG(MIN(s_acctbal * pick)) PER s_nationkey;

-- --- O35: MIN(AVG(...)) PER -------------------------------------------
-- Branch: outer MIN, inner AVG (1/n_g coefficient scaling).
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 10
MAXIMIZE MIN(AVG(s_acctbal * pick)) PER s_nationkey;

-- --- O36: MAX(AVG(...)) PER -------------------------------------------
-- Branch: outer MAX, inner AVG.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 30
MINIMIZE MAX(AVG(s_acctbal * pick)) PER s_nationkey;

-- --- O37: Linear+quadratic mixed inside one SUM -----------------------
-- Branch: SUM(POWER(linear, 2) + linear) — quadratic and linear terms
-- combined inside a single SUM. Doc-claimed equivalent to the sibling-
-- sum form (O16) but the parser path is distinct: the quadratic
-- extractor must split the inner expression into a POWER part
-- populating Q and a linear part populating c.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20
MINIMIZE SUM(POWER(qty - 3, 2) + p_retailprice * qty);

-- --- O38: Nested-aggregate quadratic objective ------------------------
-- Branch: SUM(SUM(POWER(...))) PER — quadratic inner under nested-
-- aggregate routing. M11–M13/O26–O36 cover nested aggregates with
-- linear/MIN/MAX inner; this exercises the QP path under the same
-- nested wrapper.
SELECT s_suppkey, s_nationkey, qty
FROM supplier
DECIDE qty IS REAL
SUCH THAT qty <= 10
MINIMIZE SUM(SUM(POWER(qty - 5, 2))) PER s_nationkey;

-- --- O39: ABS in PER-grouped objective --------------------------------
-- Branch: SUM(ABS(...)) PER. O22 covers flat ABS objective (single SUM);
-- M5–M9 cover PER on linear. This combination exercises ABS auxiliary
-- emission organized by PER groups.
SELECT s_suppkey, s_nationkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
MINIMIZE SUM(ABS(qty - 5)) PER s_nationkey;

-- --- O40: Hard MAX in objective with WHEN -----------------------------
-- Branch: MAXIMIZE MAX(...) WHEN — hard direction (per-row binary
-- indicators + global aux z + sum-y >= 1) composed with WHEN row
-- filter. O6 is hard MAX without WHEN; O21 is easy MAX with WHEN.
-- The combination affects which rows participate in the indicator pool.
SELECT s_suppkey, s_nationkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) >= 5
MAXIMIZE MAX(s_acctbal * pick) WHEN (s_nationkey <= 10);

-- --- O41: POWER over multi-variable linear inner ----------------------
-- Branch: POWER(x + y - K, 2) — the quadratic extractor must produce
-- off-diagonal Q entries from the cross-term 2xy. O11–O15 are all
-- single-variable inner.
SELECT p_partkey, qty, stock
FROM part
WHERE p_size < 5
DECIDE qty IS REAL, stock IS REAL
SUCH THAT qty <= 10 AND stock <= 10
MINIMIZE SUM(POWER(qty + stock - 8, 2));

-- --- O42: MAXIMIZE SUM(ABS(expr)) — Big-M sign-indicator path ---------
-- Branch: MAXIMIZE of SUM over ABS(linear-in-decide-var) is the one
-- remaining ABS code path that uses Big-M with a per-row sign-indicator
-- binary. After the 2026-05-07 ABS soundness gate, hard-direction ABS
-- in constraints is rejected, leaving this objective shape as the only
-- Big-M ABS user. Without this query that code path goes untested by
-- the stress suite (the audit flagged this regression in coverage).
-- The optimum picks qty=0 or qty=10 for every row to maximize |qty-5|.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
MAXIMIZE SUM(ABS(qty - 5));
