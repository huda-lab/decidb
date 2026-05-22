-- =====================================================================
-- VARIABLES — type forms, multiplicity, scoping (row vs table)
-- =====================================================================

-- --- V1: IS BOOLEAN ---------------------------------------------------
-- Branch: 0/1 binary variable.
SELECT s_suppkey, s_acctbal, pick
FROM supplier
DECIDE pick IS BOOLEAN
SUCH THAT SUM(pick) <= 10
MAXIMIZE SUM(s_acctbal * pick);

-- --- V2: IS INTEGER ---------------------------------------------------
-- Branch: non-negative integer variable.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS INTEGER
SUCH THAT qty <= 10
MAXIMIZE SUM(p_retailprice * qty);

-- --- V3: IS REAL ------------------------------------------------------
-- Branch: continuous non-negative variable.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty IS REAL
SUCH THAT qty <= 10
  AND SUM(qty) <= 100
MAXIMIZE SUM(p_retailprice * qty);

-- --- V4: Default type (no IS clause) → INTEGER ------------------------
-- Branch: omitted IS clause defaults to INTEGER.
SELECT p_partkey, p_retailprice, qty
FROM part
WHERE p_size < 5
DECIDE qty
SUCH THAT qty <= 10
MAXIMIZE SUM(p_retailprice * qty);

-- --- V5: Multiple variables, same type --------------------------------
-- Branch: two BOOLEAN vars; per-row link constraint between them.
SELECT s_suppkey, s_acctbal, pick, premium
FROM supplier
DECIDE pick IS BOOLEAN, premium IS BOOLEAN
SUCH THAT premium <= pick
  AND SUM(pick) <= 30
  AND SUM(premium) <= 5
MAXIMIZE SUM(s_acctbal * pick) + SUM(s_acctbal * premium);

-- --- V6: Multiple variables, mixed types (BOOL + INT) -----------------
-- Branch: pairing a binary "include" flag with an integer quantity.
SELECT p_partkey, p_retailprice, pick, qty
FROM part
WHERE p_size < 5
DECIDE pick IS BOOLEAN, qty IS INTEGER
SUCH THAT qty <= 20
  AND SUM(pick) <= 50
  AND SUM(qty) <= 200
MAXIMIZE SUM(p_retailprice * pick) + SUM(p_retailprice * qty);

-- --- V7: Three variables — BOOLEAN + INTEGER + REAL -------------------
-- Branch: all three type tags coexist in one DECIDE.
SELECT p_partkey, p_retailprice, pick, qty, stock
FROM part
WHERE p_size < 5
DECIDE pick IS BOOLEAN, qty IS INTEGER, stock IS REAL
SUCH THAT qty <= 10 AND stock <= 50
  AND SUM(pick) <= 30
  AND SUM(qty) <= 100
  AND SUM(stock) <= 300
MAXIMIZE SUM(p_retailprice * pick + p_retailprice * qty + 0.5 * p_retailprice * stock);

-- --- V8: Table-scoped variable (Table.var) ----------------------------
-- Branch: one variable per unique entity in `supplier`.
SELECT s_suppkey, s_acctbal, supplier.pick
FROM supplier
DECIDE supplier.pick IS BOOLEAN
SUCH THAT SUM(supplier.pick) <= 10
MAXIMIZE SUM(s_acctbal * supplier.pick);

-- --- V9: Table-scoped via alias --------------------------------------
-- Branch: alias-qualified variable on aliased FROM-source.
SELECT s.s_suppkey, s.s_acctbal, s.pick
FROM supplier s
DECIDE s.pick IS BOOLEAN
SUCH THAT SUM(s.pick) <= 8
MAXIMIZE SUM(s.s_acctbal * s.pick);

-- --- V10: Table-scoped over a join (entity-keyed reuse) ---------------
-- Branch: per-row repetitions of an entity collapse to one variable.
SELECT l.l_orderkey, l.l_suppkey, l.l_extendedprice, supplier.pick
FROM lineitem l
JOIN supplier ON l.l_suppkey = supplier.s_suppkey
WHERE l.l_orderkey < 500
DECIDE supplier.pick IS BOOLEAN
SUCH THAT SUM(supplier.pick) <= 15
MAXIMIZE SUM(l.l_extendedprice * supplier.pick);

-- --- V11: Mixed table-scoped + row-scoped -----------------------------
-- Branch: one entity-keyed BOOLEAN plus a per-row INTEGER quantity.
SELECT l.l_orderkey, l.l_linenumber, l.l_suppkey, l.l_extendedprice,
       supplier.pick, qty
FROM lineitem l
JOIN supplier ON l.l_suppkey = supplier.s_suppkey
WHERE l.l_orderkey < 200
DECIDE supplier.pick IS BOOLEAN, qty IS INTEGER
SUCH THAT qty <= 5
  AND SUM(supplier.pick) <= 30
  AND SUM(qty) <= 200
MAXIMIZE SUM(l.l_extendedprice * qty);

-- --- V12: Two distinct table-scoped variables on the same table -------
-- Branch: two entity-keyed vars (BOOL + INT) sharing the same scope key;
--      also exercises a per-row SUCH THAT constraint with qualified LHS
--      (`supplier.qty <= 10`).
SELECT s_suppkey, s_acctbal, supplier.pick, supplier.qty
FROM supplier
DECIDE supplier.pick IS BOOLEAN, supplier.qty IS INTEGER
SUCH THAT supplier.qty <= 10
  AND SUM(supplier.pick) <= 10
  AND SUM(supplier.qty) <= 50
MAXIMIZE SUM(s_acctbal * supplier.pick) + SUM(s_acctbal * supplier.qty);

-- --- V13: Table-scoped vars on two different tables -------------------
-- Branch: separate entity scopes for two joined tables.
SELECT s.s_suppkey, s.s_nationkey, s.s_acctbal,
       s.pick, nation.flag
FROM supplier s
JOIN nation ON s.s_nationkey = nation.n_nationkey
DECIDE s.pick IS BOOLEAN, nation.flag IS BOOLEAN
SUCH THAT SUM(s.pick) <= 20
  AND SUM(nation.flag) <= 5
MAXIMIZE SUM(s.s_acctbal * s.pick * nation.flag);
