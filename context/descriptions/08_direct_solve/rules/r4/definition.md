# R4 — Proportional max-min fairness

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

R4 allocates one resource so that the smallest fraction of demand served is as large as possible.

| **Recipient** | **Demand** | **Capacity** | **Resource per unit** |
|:-------------:|------------|--------------|-----------------------|
|       A       | 2          | 4            | 1                     |
|       B       | 4          | 8            | 1                     |

With a budget of 9, one complete service level consumes 2 + 4 = 6 resource units. The common level is therefore 9/6 = 1.5, giving allocations 3 and 6.

```sql
SELECT id, amount
FROM demands
DECIDE amount(REAL)
SUCH THAT amount BETWEEN 0 AND capacity
AND SUM(resource_weight*amount) <= 9
MAXIMIZE MIN(amount/demand);
```

```sql
WITH state AS (
SELECT
SUM(resource_weight*demand) AS resource_per_level,
MIN(capacity/demand) AS capacity_level
FROM demands
),
level AS (
SELECT LEAST(9.0/resource_per_level, capacity_level) AS service_level
FROM state
)
SELECT
id,
demand*service_level AS amount
FROM demands
CROSS JOIN level;
```

## Recognition

After normalization:

> • every decision is REAL with 0 \<= amount \<= capacity;
>
> • every demand is strictly positive;
>
> • the objective is exactly MAXIMIZE MIN(amount/demand);
>
> • there is exactly one resource upper bound with strictly positive coefficients;
>
> • there is no secondary objective or other coupling constraint.

A second resource, a nonzero allocation baseline, or an objective that also rewards leftover allocation does not belong to R4.

## Supported variants

| **Variant** | **Handling** |
|----|----|
| Budget-limited | The level is budget / SUM(resource_weight\*demand). |
| Capacity-limited | The first capacity/demand ratio fixes the common level. |
| Zero budget | Return a zero service level and zero allocations. |
| Zero capacity | The common level is zero. |
| Different demands | Larger demand receives proportionally more allocation at the same level. |
| Different resource weights | Include them in the resource required for one common level. |
| Repeated decision identity | Sum its resource coefficients, retain its tightest capacity, and use its greatest positive demand. |
| Independent groups | Compute a separate common level for each proved-disjoint component. |

If a capacity fixes the service level before the budget is exhausted, leftover resource is intentionally unused. The objective contains no secondary preference for where to place it.

## Relational construction

For each component, SQL:

> 1\. computes the resource required to raise every decision by one normalized service level: SUM(resource_weight\*demand);
>
> 2\. computes the highest level allowed by any individual capacity: MIN(capacity/demand);
>
> 3\. takes the smaller of the budget-limited and capacity-limited levels;
>
> 4\. emits amount = demand\*service_level for every decision;
>
> 5\. validates the resource and capacity constraints and maps the assignments back to the source rows.

The translation uses grouped sums, MIN, division, LEAST, multiplication, and joins.

## Correctness

Any assignment with minimum service level s must give every recipient at least demand_i\*s. It therefore requires at least:

s \* SUM(resource_weight_i\*demand_i)

resource, and it must satisfy s \<= capacity_i/demand_i for every recipient. These requirements provide two upper bounds on s.

The SQL formula chooses the smaller upper bound and assigns every recipient exactly that common level. The result satisfies the budget and every capacity, and no other assignment can achieve a larger minimum service level.

## Eligibility conditions

> • Every decision is REAL with the exact box 0 \<= amount \<= capacity.
>
> • Demands and resource coefficients are finite and strictly positive.
>
> • Capacities and the budget are finite and nonnegative.
>
> • The objective is exactly one maximum of the minimum normalized service ratio.
>
> • There is exactly one resource upper bound and no lower resource requirement, equality, second resource, baseline, or secondary objective.
>
> • Repeated identities and component membership are resolved exactly before computing the level.
>
> • Negative budgets or inconsistent boxes produce DECIDE infeasibility, while zero budgets and zero capacities produce level zero.
>
> • Empty input, WHEN, PER, NULL, numeric, decision-identity, and output-mapping semantics are preserved.
