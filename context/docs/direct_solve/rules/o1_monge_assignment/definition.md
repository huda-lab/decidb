# O1 — Monge assignment

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

O1 matches two equally sized ordered sets. Every item on each side must be matched exactly once.

| **Left item** | **Position** | **Right item** | **Position** |
|:-------------:|--------------|----------------|--------------|
|      L1       | 1            | R1             | 2            |
|      L2       | 4            | R2             | 6            |
|      L3       | 10           | R3             | 11           |

For squared distance, the optimum pairs equal ranks:

L1 -\> R1

L2 -\> R2

L3 -\> R3

Its total penalty is 1 + 4 + 1 = 6.

```sql
CREATE TABLE left_items (id INTEGER, position DOUBLE NOT NULL);
CREATE TABLE right_items (id INTEGER, position DOUBLE NOT NULL);

SELECT l.id AS left_id, r.id AS right_id, selected
FROM left_items l CROSS JOIN right_items r
DECIDE selected(BOOL)
SUCH THAT per l: sum(selected) by (l) = 1
AND per r: sum(selected) by (r) = 1
MINIMIZE SUM(POWER(l.position-r.position, 2)*selected);
```

`per l` makes each distinct row of `left_items` one class, so a duplicate row shares its class, and `NOT NULL` on the
positions is what lets the plan rely on them (see Correctness).

```sql
WITH left_ranked AS (
SELECT *, ROW_NUMBER() OVER (
ORDER BY position, id
) AS match_rank
FROM left_items
),
right_ranked AS (
SELECT *, ROW_NUMBER() OVER (
ORDER BY position, id
) AS match_rank
FROM right_items
)
SELECT
l.id AS left_id,
r.id AS right_id,
l.match_rank = r.match_rank AS selected
FROM left_ranked l
CROSS JOIN right_ranked r;
```

The SQL emits all nine pair decisions. Three are TRUE; the remaining six are FALSE.

## Recognition

After normalization:

> • the decisions are Boolean and represent every pair in a complete cross product;
>
> • both sides contain the same number of unique ordered identities;
>
> • every left identity and every right identity must appear in exactly one selected pair;
>
> • the objective has a recognized ordered form whose crossings can always be removed;
>
> • no other constraint or objective term changes the matching.

The recognized objectives are one-dimensional absolute or squared distance minimization and a rank-one product objective. Distance minimization and product maximization use equal ranks. Product minimization uses opposite ranks.

A relation with a missing pair does not belong to O1. The rank pair selected by the formula might be absent even when another complete assignment remains feasible.

## Supported variants

| **Variant** | **Handling** |
|----|----|
| Absolute distance minimization | Sort both sides in the same direction and match equal ranks. |
| Squared distance minimization | Use the same equal-rank match. |
| Rank-one product maximization | Match equal ranks. |
| Rank-one product minimization | Match the smallest left rank with the largest right rank. |
| Tied positions | Stable internal identity chooses one optimal ordering. |
| Compatible fixed pairs | Allowed only when every fixing agrees with the constructed rank match. |
| Empty side | Preserve the empty source result. |
| Unequal nonempty side sizes | Report DECIDE infeasibility. |

The product cases change only the rank direction. They do not require evaluating every possible assignment.

## Relational construction

SQL:

> 1\. checks that the two sides are both empty or have the same nonzero size;
>
> 2\. ranks each side by position and stable internal identity;
>
> 3\. joins equal ranks for distance minimization or product maximization;
>
> 4\. joins opposite ranks for product minimization;
>
> 5\. marks the chosen pairs TRUE and every other original pair FALSE;
>
> 6\. maps those Boolean values back to the original pair rows.

The translation uses counts, ROW_NUMBER, rank arithmetic, a cross product, Boolean comparison, and joins.

## Correctness

Equal-rank or opposite-rank pairing is a bijection, so every identity on each side is matched exactly once.

For distance, consider two left positions a \<= a' and two right positions b \<= b'. Pairing them in order costs no more than crossing the pairs. Any crossed assignment can therefore be uncrossed without making the objective worse. Repeating this step produces the equal-rank assignment.

For a product objective, the rearrangement inequality gives equal ranks for maximization and opposite ranks for minimization. Stable identity ordering resolves position ties without changing the objective.

Every left row meets every right row when DECIDE's source is a cross product: filter pushdown runs before the DECIDE
optimizer, so a filter on one side sits inside that side, and a filter across both sides turns the cross product into
a join, which is not O1. The classes of a relation key are the distinct rows of their side, so the copies of one row
share its position and every pair of two classes costs the same on each of its copies; choosing one copy of each
matched pair reaches the optimum of the classes. A NULL position would make its item free: `SUM` drops a NULL term, so
that item costs nothing against any partner and the rank order no longer places it. A position declared `NOT NULL` on
the table its side scans rules this out for every row the plan reads.

The rank order is computed on the input values themselves, so the pairing is exact for them. The costs the objective
evaluates round each term once, far inside the solver's own tolerance.

## Eligibility conditions

> • Every decision is BOOL, and DECIDE's source is a cross product of the two sides, so it holds a decision for every pair of a left row and a right row.
>
> • No filter reads both sides; a filter on one side only changes that side's rows.
>
> • The only coupling constraints require exactly one selected pair per left identity and exactly one per right identity.
>
> • The two `per` classes and their `when` masks cover exactly the intended incident pairs, and any fixed decisions agree with the rank construction.
>
> • An exact count gate preserves empty input and reports infeasibility for unequal nonempty side sizes.
>
> • The objective is exactly one supported form with the required direction: a nonnegative constant multiple of absolute or squared distance is minimized, or a positive constant multiple of a rank-one product is maximized or minimized.
>
> • There is no additional objective term, secondary objective, or other coupling constraint.
>
> • Positions are columns declared `NOT NULL` on the tables their sides scan directly; scale values are finite constants.
>
> • Ties use stable internal identities rather than an unproved user key.
>
> • The result emits one Boolean assignment for every original pair row and preserves DECIDE's source cardinality, schema, ordering, status, and output mapping.
