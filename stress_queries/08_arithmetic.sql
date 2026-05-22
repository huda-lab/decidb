-- =====================================================================
-- SQL FUNCTIONS / ARITHMETIC — aggregates, ABS, +/-/*//, ** exponent
-- =====================================================================
-- Each query exercises one arithmetic/function branch in isolation.
-- Aggregate-shape coverage (PER, WHEN, MIN/MAX directions) lives in
-- 01_constraints.sql / 02_modifiers.sql / 03_objectives.sql.

-- --- A1: SUM aggregate -----------------------------------------------
-- Branch: SUM as aggregate over decision vars (constraint + objective).
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 15
MAXIMIZE SUM(s_acctbal * pick);

-- --- A2: AVG aggregate -----------------------------------------------
-- Branch: AVG → SUM rewrite with row-count scaling.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 10
DECIDE qty IS INTEGER
SUCH THAT qty <= 20
  AND AVG(qty) <= 4
MAXIMIZE SUM(p_retailprice * qty);

-- --- A3: MIN aggregate (easy direction) -------------------------------
-- Branch: MIN(...) >= K — stripped to per-row bound.
SELECT s_suppkey, s_acctbal, level
FROM supplier
WHERE s_acctbal > 0
DECIDE level IS INTEGER
SUCH THAT level <= 5
  AND MIN(s_acctbal * level) >= 100
MAXIMIZE SUM(level);

-- --- A4: MAX aggregate (easy direction) -------------------------------
-- Branch: MAX(...) <= K — stripped to per-row bound.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT MAX(s_acctbal * pick) <= 8000
  AND SUM(pick) >= 5
MAXIMIZE SUM(pick);

-- --- A5: ABS in per-row constraint (lower-envelope) ------------------
-- Branch: |expr| <= K → expr <= K AND expr >= -K.
SELECT s_suppkey, s_acctbal, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT qty <= 20
  AND ABS(qty - 5) <= 3
MAXIMIZE SUM(s_acctbal * qty);

-- --- A6: ABS in MIN objective (lower-envelope) -----------------------
-- Branch: ABS in MINIMIZE — uses lower-envelope linearization.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND SUM(qty) >= 50
MINIMIZE SUM(ABS(qty - 4));

-- --- A7: ABS in MAX objective (Big-M) --------------------------------
-- Branch: -ABS in MAXIMIZE — Big-M form.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND SUM(qty) >= 30
MAXIMIZE SUM(-ABS(qty - 5));

-- --- A8: Binary addition (var + var) ---------------------------------
-- Branch: + combining two decision-var terms.
SELECT p_partkey, p_retailprice, qty, stock
FROM part
WHERE p_size < 5
DECIDE qty IS REAL, stock IS REAL
SUCH THAT qty <= 50 AND stock <= 50
  AND SUM(qty + stock) <= 200
MAXIMIZE SUM(p_retailprice * (qty + stock));

-- --- A9: Binary subtraction (var - var) ------------------------------
-- Branch: - combining two decision-var terms.
SELECT p_partkey, p_retailprice, qty, stock
FROM part
WHERE p_size < 5
DECIDE qty IS REAL, stock IS REAL
SUCH THAT qty <= 50 AND stock <= 50
  AND SUM(qty - stock) <= 100
MAXIMIZE SUM(p_retailprice * qty - p_retailprice * stock);

-- --- A10: Unary minus on decision variable ---------------------------
-- Branch: prefix `-x` flips coefficient sign.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 10
  AND SUM(qty) >= 20
MINIMIZE SUM(-p_retailprice * qty);

-- --- A11: Multiplication — var × constant -----------------------------
-- Branch: scalar coefficient on a decision variable.
SELECT s_suppkey, qty
FROM supplier
DECIDE qty IS INTEGER
SUCH THAT 3 * qty <= 30
MAXIMIZE SUM(2 * qty);

-- --- A12: Multiplication — var × column (data coefficient) -----------
-- Branch: data column lifted into linear coefficient.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
  AND SUM(p_retailprice * qty) <= 100000
MAXIMIZE SUM(p_retailprice * qty);

-- --- A13: Multiplication — bool × var (bilinear, McCormick) -----------
-- Branch: var × var with a Boolean factor → linearizable both solvers.
SELECT p_partkey, p_retailprice, pick, qty
FROM part
WHERE p_size < 5
DECIDE pick IS BOOLEAN, qty IS INTEGER
SUCH THAT qty <= 20
  AND SUM(pick) <= 50
MAXIMIZE SUM(p_retailprice * pick * qty);

-- --- A14: Multiplication — int × real (general bilinear, Gurobi) ------
-- Branch: var × var without Boolean factor → non-convex, Gurobi only.
SELECT p_partkey, p_retailprice, qty, disc
FROM part
WHERE p_size < 3
DECIDE qty IS INTEGER, disc IS REAL
SUCH THAT qty <= 20 AND disc <= 5
MAXIMIZE SUM(p_retailprice * qty - qty * disc);

-- --- A15: Division by constant ---------------------------------------
-- Branch: var / K → const-scaled coefficient.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty / 2 <= 5
  AND SUM(qty) <= 100
MAXIMIZE SUM(p_retailprice * qty);

-- --- A16: Division by data column ------------------------------------
-- Branch: var / col → per-row coefficient = 1/col_value.
SELECT p_partkey, p_retailprice, p_size, qty
FROM part
WHERE p_size BETWEEN 1 AND 5
DECIDE qty IS REAL
SUCH THAT qty / p_size <= 4
  AND SUM(qty) <= 200
MAXIMIZE SUM(p_retailprice * qty);

-- --- A17: ** exponent (only 2) ---------------------------------------
-- Branch: expr**2 alias of POWER(expr, 2).
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) = 30
MINIMIZE SUM((qty - 2) ** 2);

-- --- A18: POW(expr, 2) alias -----------------------------------------
-- Branch: POW alias parity with POWER.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) = 30
MINIMIZE SUM(POW(qty - 2, 2));

-- --- A19: Self-product (expr) * (expr) -------------------------------
-- Branch: explicit self-multiplication recognized as quadratic.
SELECT p_partkey, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 20 AND SUM(qty) = 30
MINIMIZE SUM((qty - 2) * (qty - 2));

-- --- A20: Mixed addition + subtraction + scaling in one SUM ----------
-- Branch: combined arithmetic — linear coefficients flatten to one row.
SELECT p_partkey, p_retailprice, qty, stock
FROM part
WHERE p_size < 5
DECIDE qty IS REAL, stock IS REAL
SUCH THAT qty <= 30 AND stock <= 30
  AND SUM(2 * qty - 0.5 * stock + 1) <= 500
MAXIMIZE SUM(p_retailprice * qty + 0.25 * p_retailprice * stock);
