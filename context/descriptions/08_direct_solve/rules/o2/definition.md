# O2 — Balanced one-dimensional Monge transport

Catalogue status: Prototype.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

Exact mathematical translation, but still a prototype because balance, complete pair support, and exact cumulative-mass arithmetic must be certified.

## Worked example

O2 transports divisible mass between ordered sources and destinations. Supply and demand totals must be equal.

| **Source** | **Position** | **Supply** |
|:----------:|--------------|------------|
|     S1     | 1            | 3          |
|     S2     | 5            | 2          |

| **Destination** | **Position** | **Demand** |
|:---------------:|--------------|------------|
|       D1        | 2            | 1          |
|       D2        | 4            | 4          |

Both sides contain five units of mass. Place each side on a cumulative mass line:

Sources: S1 \[0,3\] S2 \[3,5\]

Destinations: D1 \[0,1\] D2 \[1,5\]

The overlap lengths give the flow:

| **Pair** | **Flow** |
|----------|----------|
| S1 to D1 | 1        |
| S1 to D2 | 2        |
| S2 to D1 | 0        |
| S2 to D2 | 2        |

```sql
SELECT s.id AS source_id, d.id AS destination_id, flow
FROM sources s CROSS JOIN destinations d
DECIDE flow(REAL)
SUCH THAT SUM(flow) = s.supply PER s.id
AND SUM(flow) = d.demand PER d.id
MINIMIZE SUM(ABS(s.position-d.position)*flow);
```

```sql
WITH source_intervals AS (
SELECT *,
COALESCE(SUM(supply) OVER (
ORDER BY position, id
ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
), 0.0) AS lo,
SUM(supply) OVER (
ORDER BY position, id ROWS UNBOUNDED PRECEDING
) AS hi
FROM sources
),
destination_intervals AS (
SELECT *,
COALESCE(SUM(demand) OVER (
ORDER BY position, id
ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
), 0.0) AS lo,
SUM(demand) OVER (
ORDER BY position, id ROWS UNBOUNDED PRECEDING
) AS hi
FROM destinations
)
SELECT
s.id AS source_id,
d.id AS destination_id,
GREATEST(
0.0,
LEAST(s.hi, d.hi)-GREATEST(s.lo, d.lo)
) AS flow
FROM source_intervals s
CROSS JOIN destination_intervals d;
```

## Recognition

After normalization:

> • every decision is a nonnegative REAL flow for one source-destination pair;
>
> • the pair relation is a complete cross product of ordered, unique identities;
>
> • every source has one fixed supply and every destination has one fixed demand;
>
> • all supplies and demands are nonnegative and their totals are equal;
>
> • the objective uses a recognized one-dimensional Monge distance;
>
> • no arc bound, fixing, or other constraint changes the transport problem.

Unlike O1, the two sides may contain different numbers of rows. Only their total mass must balance.

A transport problem with a missing arc does not belong to O2. The interval-overlap construction may need to send positive flow through that exact arc.

## Supported variants

| **Variant** | **Handling** |
|----|----|
| Absolute distance | Sort by position and use cumulative interval overlap. |
| Squared distance | The same monotone overlap construction remains optimal. |
| Different side cardinalities | Allowed when total supply equals total demand. |
| Split flow | One source or destination interval may overlap several intervals. |
| Zero supply or demand | Its interval has zero length, so all incident flows are zero. |
| Equal positions | Stable internal identity gives deterministic cumulative intervals. |
| Unit supply and demand | Every overlap is zero or one, reducing the distance case to O1. |
| Proved-disjoint groups | Apply the construction independently after verifying balance in every group. |

A nonnegative constant multiplier on the distance changes the objective value but not the optimal flow.

## Relational construction

For each independent transport component, SQL:

> 1\. validates identities, masses, complete pair support, and total balance;
>
> 2\. sorts the sources and destinations by position and stable internal identity;
>
> 3\. turns every supply and demand into an interval on the same cumulative mass line;
>
> 4\. assigns each pair the length of the intersection of its two intervals;
>
> 5\. recomputes the source and destination totals;
>
> 6\. maps every positive or zero flow back to the original pair row.

The translation uses sorting, windowed prefix sums, LEAST, GREATEST, subtraction, a cross product, grouping, and joins.

## Correctness

When total supply equals total demand, the source intervals and destination intervals both partition the same cumulative mass line. The overlaps with one source interval partition that interval, so its outgoing flows sum to its supply. The same argument shows that every destination receives exactly its demand. Overlap lengths cannot be negative.

For ordered sources and destinations, absolute and squared distance satisfy the Monge inequality: uncrossed flow costs no more than crossed flow. Moving the smaller amount from two crossed positive flows onto the uncrossed pairs preserves every supply and demand. Repeating this exchange produces the monotone interval-overlap flow without increasing the objective.

The final cross product also emits zero flow for nonoverlapping intervals, preserving the complete DECIDE assignment rather than returning only its positive support.

## Eligibility conditions

> • Every decision is a nonnegative REAL flow, and the normalized child contains exactly one decision for every pair of unique, non-NULL source and destination identities.
>
> • Pair support is a complete cross product with no filters, missing arcs, or duplicate pair decisions.
>
> • The only coupling constraints are one supply equality per source and one demand equality per destination, with no arc bounds, fixed flows, or other constraints.
>
> • The PER groups and WHEN masks cover exactly the intended incident pairs.
>
> • Supplies and demands are finite, non-NULL, and nonnegative, and their totals balance under DECIDE's equality policy.
>
> • Empty input preserves the source-level empty result. Invalid numeric input produces DECIDE's numeric outcome, while negative mass or unequal totals produce infeasibility.
>
> • The objective is exactly a nonnegative constant multiple of a supported one-dimensional Monge distance, initially absolute or squared distance, with no additional or secondary objective term.
>
> • Positions and scale values are finite and non-NULL, and their evaluated distances preserve the Monge ordering under DECIDE's numeric policy.
>
> • Every mass, cumulative endpoint, subtraction, interval overlap, and output cast is exact and representable under the admitted numeric policy. The current prototype requires a proved common exact mass quantum.
>
> • Equal positions and zero masses use stable internal identities. Recomputed row and column sums must certify every required marginal without a one-cell repair.
>
> • The result emits one flow for every original pair row and preserves DECIDE's result type, source cardinality, schema, ordering, status, and output mapping.

## Prototype limitations

The interval construction is mathematically exact, but DeciDB must still prove complete pair support, valid balanced marginals, and a numeric representation in which cumulative endpoints and overlap lengths meet DECIDE's equality semantics. If those facts or the final marginal checks cannot be certified, the query remains on the general solver path.

## O1 and O2 comparison

O1 distance minimization is the unit-mass case of O2:

> • every source supplies one unit;
>
> • every destination demands one unit;
>
> • every cumulative overlap is either zero or one.

O1 remains separate because its decisions are Boolean and its product-objective variants rely on the rearrangement inequality rather than the Monge transport proof.
