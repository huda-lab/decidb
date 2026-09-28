# Ordered Matching and Transport

General assignment and transportation remain solver problems. A direct relational
construction becomes possible when both sides are complete and ordered, and the cost
has a form for which crossing assignments can always be uncrossed.

Both translations in this file are **exact prototypes**. Their mathematics is exact,
but DeciDB must still prove complete pair support, balance, identity, and acceptable
full-output cost before selecting them.

```text
Left/source:       1 -------- 4 -------------- 10
Right/destination: 2 ------------ 6 --------------- 11

Ordered matching:  1 -> 2,   4 -> 6,          10 -> 11
```

| Rule | Decisions | Direct construction |
|---|---|---|
| O1 | One Boolean match per pair | Match equal or reverse ranks |
| O2 | Nonnegative flow per pair | Overlap cumulative mass intervals |

## O1 — Ordered complete assignment

**Status:** Exact prototype

### Problem

Two equally sized ordered sets form a complete bipartite assignment. Every item on each
side must be matched exactly once. Cost is a recognized one-dimensional distance or
rank-one product.

### DECIQL

```sql
SELECT l.id AS left_id, r.id AS right_id, selected
FROM left_items l CROSS JOIN right_items r
DECIDE selected(BOOL)
SUCH THAT SUM(selected) = 1 PER l.id
      AND SUM(selected) = 1 PER r.id
MINIMIZE SUM(POWER(l.position-r.position, 2)*selected);
```

### Direct SQL

The generated plan chooses `SAME` for the displayed distance objective and product
maximization, or `REVERSE` for product minimization:

```sql
WITH params(match_order) AS (
    VALUES ('SAME')
), side_state AS (
    SELECT (SELECT COUNT(*) FROM left_side_rows) AS left_count,
           (SELECT COUNT(*) FROM right_side_rows) AS right_count
), outcome AS (
    SELECT CASE
             WHEN left_count = 0 OR right_count = 0 THEN 'EMPTY'
             WHEN left_count <> right_count THEN 'INFEASIBLE'
             ELSE 'FEASIBLE'
           END AS state,
           left_count AS pair_count
    FROM side_state
), left_ranked AS (
    SELECT *, ROW_NUMBER() OVER (
               ORDER BY position, _left_id
           ) AS rank
    FROM left_side_rows
), right_ranked AS (
    SELECT *, ROW_NUMBER() OVER (
               ORDER BY position, _right_id
           ) AS rank
    FROM right_side_rows
), assignments AS (
    SELECT p._decision_id,
           (
               l.rank = CASE q.match_order
                          WHEN 'SAME' THEN r.rank
                          WHEN 'REVERSE' THEN o.pair_count + 1-r.rank
                        END
           )::INTEGER AS selected
    FROM pair_rows p
    JOIN left_ranked l USING (_left_id)
    JOIN right_ranked r USING (_right_id)
    CROSS JOIN params q
    CROSS JOIN outcome o
    WHERE o.state = 'FEASIBLE'
)
SELECT p.left_id, p.right_id, a.selected
FROM pair_rows p
JOIN assignments a USING (_decision_id);
```

The planner consumes `outcome`: `EMPTY` returns the source-level empty result and
`INFEASIBLE` reports the DECIDE outcome. Neither state is treated as a successful
empty assignment.

### Direct algorithm

1. Count both sides; preserve an empty cross product and reject unequal nonempty sides.
2. Rank each base side by its ordered value and stable internal identity.
3. Match equal ranks for distance minimization or product maximization, and reverse
   ranks for product minimization.
4. Emit the Boolean assignment for every original pair row.

### Why it works

- **Feasibility.** Equal ranks define a bijection when both sides have the same
  cardinality. Emitting `1` on those pairs and `0` on every other supported pair gives
  every left and right identity exactly one match.
- **Optimality.** Let `a <= a'` be two left positions and `b <= b'` two right
  positions. For absolute or squared distance, assigning `(a,b)` and `(a',b')` costs
  no more than the crossed assignment `(a,b')` and `(a',b)`. Repeatedly uncrossing an
  arbitrary assignment therefore reaches equal-rank matching without increasing cost.
  The rearrangement inequality gives equal ranks for product maximization and reverse
  ranks for product minimization.
- **Completeness.** Stable ordering resolves tied positions without changing the
  primary objective, and the final cross product supplies the required zero-valued
  decisions as well as the selected pairs.

### Valid when

- The decision is BOOL and the normalized child is proved to be the complete cross
  product of two exact, non-NULL, unique side identities, with exactly one decision per
  pair and no filtered or duplicated pair support.
- The only coupling consists of invariant unit equalities requiring one match per left
  and right identity; every `PER`/`WHEN` mask covers exactly the intended incident
  pairs, and no individual fixing conflicts with the rank assignment.
- An exact generated count gate returns the source-level empty result if either side is
  empty and reports `INFEASIBLE` if two nonempty sides have unequal cardinality; it
  never continues with a partial rank match or switches to a solver at runtime.
- The objective is exactly one constant-scaled whitelisted form with the matching
  direction fixed in advance: nonnegative absolute or squared distance is minimized;
  a positive rank-one product is maximized by equal ranks or minimized by reverse
  ranks. There are no extra objective terms or secondary tie requirements.
- Positions, scales, and solver-read values are non-NULL and finite. Every evaluated
  subtraction, absolute value, square, product, and scaling operation is finite; exact
  arithmetic or a certified error bound proves that the resulting DECIDE coefficient
  matrix retains the required Monge or rearrangement ordering. Otherwise an earlier
  guard reproduces DECIDE's numeric error or the rule is not admitted.
- The generated plan ranks by stable internal side identity, emits one assignment for
  every original pair row, and preserves decision type, output cardinality, projection,
  and ordering. A user `id` column is used only after its uniqueness is proved, and the
  cost model must prefer materializing the complete pair output to the solver path.

> **Not this class:** A missing pair can make the rank match infeasible. An arbitrary cost matrix can make a crossed assignment strictly better.

### Open prototype gates

The mathematics above is exact. The rule remains a prototype until the optimizer can
establish every `Valid when` fact through a real child plan and the full
`left_count * right_count` output is shown to retain a useful speedup.

## O2 — Balanced one-dimensional Monge transport

**Status:** Exact prototype

### Problem

Ordered sources and destinations form a complete transport relation. Sources have fixed
supplies, destinations have fixed demands, and the totals balance.

### DECIQL

```sql
SELECT s.id AS source_id, d.id AS destination_id, flow
FROM sources s CROSS JOIN destinations d
DECIDE flow(REAL)
SUCH THAT SUM(flow) = s.supply PER s.id
      AND SUM(flow) = d.demand PER d.id
MINIMIZE SUM(ABS(s.position-d.position)*flow);
```

### Direct SQL

```sql
WITH outcome AS (
    SELECT CASE
             WHEN source_count = 0 OR destination_count = 0 THEN 'EMPTY'
             WHEN NOT inputs_valid THEN 'ERROR'
             WHEN NOT masses_nonnegative OR NOT totals_balance THEN 'INFEASIBLE'
             ELSE 'FEASIBLE'
           END AS state
    FROM transport_factor_state
), source_intervals AS (
    SELECT *,
           COALESCE(SUM(supply) OVER (
               ORDER BY position, _source_id
               ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
           ), 0) AS lo,
           SUM(supply) OVER (
               ORDER BY position, _source_id ROWS UNBOUNDED PRECEDING
           ) AS hi
    FROM source_side_rows CROSS JOIN outcome
    WHERE state = 'FEASIBLE'
), destination_intervals AS (
    SELECT *,
           COALESCE(SUM(demand) OVER (
               ORDER BY position, _destination_id
               ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
           ), 0) AS lo,
           SUM(demand) OVER (
               ORDER BY position, _destination_id ROWS UNBOUNDED PRECEDING
           ) AS hi
    FROM destination_side_rows CROSS JOIN outcome
    WHERE state = 'FEASIBLE'
), assignments AS (
    SELECT p._decision_id,
           GREATEST(
               0,
               LEAST(s.hi, d.hi)-GREATEST(s.lo, d.lo)
           )::DOUBLE AS flow
    FROM pair_rows p
    JOIN source_intervals s USING (_source_id)
    JOIN destination_intervals d USING (_destination_id)
)
SELECT p.source_id, p.destination_id, a.flow
FROM pair_rows p
JOIN assignments a USING (_decision_id);
```

`transport_factor_state` is a planner-owned one-row relation that retains exact side
counts, validated masses, and balance even when no assignment is produced. The planner
consumes `ERROR`, `EMPTY`, and `INFEASIBLE` before returning rows.

### Direct algorithm

1. Validate both sides; preserve an empty cross product and reject invalid, negative,
   or unbalanced masses.
2. Sort sources and destinations and give each an interval on their common cumulative
   mass line.
3. Assign each source-destination pair the exact length of the two intervals' overlap.
4. Certify both marginals and map the flow back to every pair row.

### Why it works

- **Feasibility.** When total supply equals total demand `M`, the source intervals and
  destination intervals each partition `[0,M]`. Intersections with one source interval
  partition that source interval, so its outgoing flow sums to its supply; the symmetric
  argument gives every destination its demand. Intersection lengths are nonnegative.
- **Optimality.** For ordered sources `s <= s'` and destinations `d <= d'`, the admitted
  cost obeys the Monge inequality
  `c(s,d) + c(s',d') <= c(s,d') + c(s',d)`. Moving the smaller of two crossed positive
  flows onto the uncrossed arcs preserves both marginals and cannot increase cost.
  Repetition yields exactly the monotone interval-overlap transport.
- **Completeness.** The cross product emits zero flow for nonoverlapping intervals, so
  the construction returns every original pair decision, not only its sparse support.

### Valid when

- The decision is REAL and the normalized child is proved to be a complete cross
  product with one nonnegative flow decision per exact pair of non-NULL, unique source
  and destination identities; pair filters and duplicates are absent.
- The only coupling is one invariant supply equality per source and one invariant demand
  equality per destination. Their `PER`/`WHEN` masks contain exactly the corresponding
  incident pairs, with no arc bounds, fixes, or additional resource constraints.
- Exact generated guards first preserve an empty cross product, then reproduce DECIDE's
  invalid-input outcome for NULL/nonfinite masses and `INFEASIBLE` for a negative mass
  or unequal totals; they never construct a partial transport or switch to a solver at
  runtime.
- The objective is exactly one nonnegative constant multiple of an expression proved
  Monge in the same side order, initially absolute or squared one-dimensional distance,
  with no extra or secondary objective term.
- Positions and cost inputs are non-NULL and finite. The admitted numeric subclass has
  a proved common exact mass quantum: every mass, cumulative endpoint, subtraction,
  overlap, and output cast is exact and representable, and DECIDE's equality tolerance
  distinguishes unequal totals. Equal positions and zero masses use stable internal
  identities; every evaluated position difference, square, and scale is finite and the
  resulting coefficient matrix is proved Monge under DECIDE's numeric values.
- Recomputed row and column sums equal the required marginals under DECIDE's policy.
  Until the exact-arithmetic and residual certificates can be established before
  rewrite, this prototype is not admitted; no post-hoc one-cell repair is assumed.
- The generated plan emits and maps a value for every original pair row, preserves the
  REAL output type and source cardinality, and never substitutes a user `id` for
  unproved internal identity. The cost model must separately prove that constructing
  the complete pair output is preferable to the solver path.

> **Not this class:** General transportation remains a network-flow problem. Missing one required arc can invalidate the cumulative-overlap assignment.

### Open prototype gates

The mathematics above is exact. The rule remains a prototype until the optimizer can
establish every `Valid when` fact, implement its stated numeric policy, and show that
complete pair-output construction is worthwhile.

## Relationship between O1 and O2

O1's distance-minimization branch is the unit-mass special case of O2:

- every source supplies one unit;
- every destination demands one unit; and
- every interval overlap is either zero or one.

The rank-one product branches use the rearrangement inequality rather than O2's Monge
distance theorem. The entries remain separate because Boolean assignment and
continuous transport also have different domains, output behavior, and admission
conditions.
