# R2 — Bounded integer marginal units

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

R2 allocates whole units among bounded integer decisions. Each possible increment has a marginal penalty or benefit.

| **Item** | **Lower bound** | **Upper bound** | **Target** |
|:--------:|-----------------|-----------------|------------|
|    A     | 0               | 3               | 2          |
|    B     | 0               | 4               | 4          |

Suppose the two decisions must sum to 5:

```sql
SELECT id, x
FROM allocation
DECIDE x(INT)
SUCH THAT x BETWEEN lo AND hi
AND SUM(x) = 5
MINIMIZE SUM(POWER(x-target, 2));
```

SQL expands every legal increment into a separate row. For example, the marginal penalty of moving from q-1 to q is:

POWER(q-target, 2) - POWER((q-1)-target, 2)

```sql
WITH units AS (
SELECT a.id, q,
POWER(q-target, 2)
- POWER((q-1)-target, 2) AS marginal_penalty
FROM allocation a,
LATERAL range(a.lo+1, a.hi+1) r(q)
),
ranked AS (
SELECT *,
ROW_NUMBER() OVER (
ORDER BY marginal_penalty, id, q
) AS marginal_rank
FROM units
),
chosen AS (
SELECT *
FROM ranked
WHERE marginal_rank <= 5
),
counts AS (
SELECT id, COUNT(*) AS units_added
FROM chosen
GROUP BY id
)
SELECT
a.id,
a.lo + COALESCE(c.units_added, 0) AS x
FROM allocation a
LEFT JOIN counts c USING (id);
```

With stable tie ordering, this example returns A = 2 and B = 3.

## Recognition

After normalization:

> • every decision is INT with one contiguous finite interval;
>
> • the objective is a sum of independent discrete-convex penalties, or the equivalent maximization of separable concave benefits;
>
> • successive marginal penalties never decrease;
>
> • one shared resource counts all units with the same positive coefficient after exact scaling;
>
> • there is no other coupling constraint or objective term.

Unequal resource coefficients create a weighted integer problem rather than R2. Nonmonotone marginals also fail because a later attractive unit might require taking an earlier unattractive unit.

## Supported variants

| **Variant** | **Handling** |
|----|----|
| Exact total | Select exactly the required number of marginal units. |
| Resource upper bound | Select improving marginal units up to the limit. |
| Discrete-convex minimization | Rank marginal penalties from smallest to largest. |
| Concave-benefit maximization | Rank marginal benefits from largest to smallest. |
| Positive lower bounds | Start from the lower bounds and allocate only the additional units. |
| Common resource multiplier | Scale it out only when the resulting total is an exact legal integer. |
| Tied marginals | Order by decision identity and increment number so predecessors come first. |
| Independent groups | Expand and rank units separately for each proved-disjoint component. |

For an upper resource bound, a zero or positive marginal penalty does not improve a minimization objective, so SQL may stop before using the complete allowance.

## Relational construction

For each component, SQL:

> 1\. converts the written bounds to legal integer endpoints;
>
> 2\. starts every decision at its lower bound;
>
> 3\. derives the number of additional units required or allowed;
>
> 4\. emits one row for every legal increment of every decision;
>
> 5\. computes and ranks the marginal penalty or benefit of each increment;
>
> 6\. selects the required or improving prefix;
>
> 7\. counts selected increments per decision and adds them to the lower bound.

The translation uses range, arithmetic, sorting, ROW_NUMBER, grouping, and joins.

## Correctness

Every integer assignment can be represented as a prefix of increments for each decision. Nondecreasing marginal penalties ensure that earlier increments appear no later than later increments of the same decision.

For any fixed number of added units, no legal assignment can have a smaller total penalty than the globally cheapest marginal units. Because that selected set is prefix-closed, it reconstructs a valid integer assignment and attains that bound.

An equality fixes the number of selected increments. An upper bound selects only the increments that improve the objective, up to the allowed number.

## Eligibility conditions

> • Every decision is INT with a finite, nonempty contiguous integer domain.
>
> • The objective is exactly separable discrete-convex minimization or the equivalent concave-benefit maximization.
>
> • Marginal penalties are nondecreasing for every decision.
>
> • The component has exactly one resource constraint with one common positive coefficient after exact scaling.
>
> • Equality scaling proves that the required total is an exact integer.
>
> • SQL emits one marginal row for every legal increment and preserves predecessor ordering.
>
> • Empty domains or unreachable exact totals produce DECIDE infeasibility.
>
> • BIGINT, overflow, WHEN, PER, NULL, numeric, decision-identity, and output-mapping semantics are preserved.
