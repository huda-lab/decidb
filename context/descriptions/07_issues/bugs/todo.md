# Known Bugs — Open

Bugs discovered but not yet fixed. Each entry: symptom, reproduction, what is known about the cause, what has been ruled out, and where to look next.

Resolved bugs are moved to `done.md`.

---

## Per-row constraint with decide vars on both sides + data-led LHS is silently not enforced

**Symptom.** A per-row constraint of the form `(data - x) <= c*z` (LHS leads with
a data column / data-minus-decide-var, RHS contains a decision variable) is not
enforced — the solver returns assignments that violate it, with no error.

**Reproduction.** Discovered 2026-06-17 while building L0 `norm(e, 0)` auto-M.
```sql
-- base = 2 for all rows
SELECT id, x, z FROM items DECIDE x IS REAL, z IS BOOLEAN
SUCH THAT SUM(x) <= 5 AND (base - x) <= 8*z
MINIMIZE SUM(z);
-- returns x=0, z=0 for rows where base=2 → (base-x)=2 <= 8*0=0 is FALSE (violated)
```
The mathematically-equivalent decision-variable-led form **is** enforced:
`8*z >= (base - x)` (i.e. `8*z + x >= base`) gives the correct result.

**Known.** Not simply "data-led LHS": `(base - x) <= 0` (constant RHS) *is*
enforced. It only misfires when the RHS also has a decision variable (`c*z`) and
the LHS leads with data, i.e. decide vars on both sides + data-led LHS. After
normalization that constraint has all-negative decide coefficients on the LHS
(`-x - 8z <= -base`); likely a normalizer/per-row evaluation path that mishandles
that shape.

**Workaround in use.** L0 auto-M (`RewriteNormL0` in `bind_select_node.cpp`) emits
its links decision-variable-first (`M*z >= e`, `M*z >= -e`) to avoid this.

**Where to look next.** `NormalizeDecideConstraints` and the per-row constraint
evaluation in `physical_decide.cpp`; compare the evaluated LHS terms / RHS for the
data-led vs decide-led forms. Discovered during: norm() L0 auto-M.
