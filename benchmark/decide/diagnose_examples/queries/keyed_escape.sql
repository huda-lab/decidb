-- An escape of a keyed decision: one `keep` per supplier, uncapped for German suppliers, counted per key (runaway_+inf,
-- scope keyed).
SELECT s.s_suppkey, keep
DECIDE per s: keep(REAL)
FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey
SUCH THAT when n.n_name <> 'GERMANY': keep <= 50
MAXIMIZE SUM(keep)
