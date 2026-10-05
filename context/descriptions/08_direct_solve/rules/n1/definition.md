# N1 — Projection or linear support under one sum-of-squares bound

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

N1 chooses a vector of REAL decisions under one explicit sum-of-squares constraint:

SUM(POWER(x, 2)) \<= B

For example:

| **Item** | **Coefficient** |
|----------|-----------------|
| A        | 3               |
| B        | 4               |

```sql
SELECT id, x
FROM directions
DECIDE x(REAL)
SUCH THAT SUM(POWER(x, 2)) <= 25
MAXIMIZE SUM(coefficient*x);
```

The coefficient vector is (3, 4). Its sum of squares is already 25, so the optimal decision vector is also (3, 4).

```sql
WITH prepared AS (
SELECT
id,
GREATEST(coefficient, 0.0) AS direction
FROM directions
),
total AS (
SELECT SUM(POWER(direction, 2)) AS squared_length
FROM prepared
)
SELECT
id,
CASE
WHEN squared_length = 0 THEN 0.0
ELSE SQRT(25.0 / squared_length) * direction
END AS x
FROM prepared
CROSS JOIN total;
```

No norm(...) operation is needed in either the DECIDE query or the relational rewrite.

## Recognition

After normalization:

> • all decisions are REAL;
>
> • there is exactly one constraint of the form SUM(POWER(x, 2)) \<= B;
>
> • every decision appears once with unit weight;
>
> • the objective is either linear or squared distance to a target vector;
>
> • no other constraint couples the decisions.

A weighted constraint such as:

SUM(weight \* POWER(x, 2)) \<= B

belongs to N2 rather than N1.

Cross-terms such as x\*y and active coordinate bounds generally fall outside N1.

## Supported variants

| **Variant** | **Direct rule** |
|----|----|
| Linear MAXIMIZE | Scale the coefficient vector to the boundary. |
| Linear MINIMIZE | Negate the coefficient vector, then use the same rule. |
| Squared-distance projection | Keep the target if it satisfies the bound. Otherwise, scale it toward zero. |
| Signed decisions | Use the complete target or coefficient vector. |
| Nonnegative decisions | Replace negative target values or coefficients with zero before scaling. |
| Zero direction | Return the zero vector. |
| B = 0 | Return the zero vector. |
| Independent groups | Compute a separate scale for every group. |

For projection, let y be the target vector after applying the domain rule:

if SUM(y²) \<= B:

x = y

otherwise:

x = SQRT(B / SUM(y²)) \* y

For a linear objective, let d be the objective direction:

x = SQRT(B / SUM(d²)) \* d

## Relational construction

For each component, SQL:

> 1\. constructs the target or objective direction;
>
> 2\. applies the signed or nonnegative domain rule;
>
> 3\. computes SUM(POWER(direction, 2));
>
> 4\. determines whether scaling is required;
>
> 5\. multiplies each coordinate by the shared scale;
>
> 6\. joins the values back to their decision identities.

The rewrite uses grouped sums, SQRT, CASE, scalar multiplication, and joins.

## Correctness

For projection, scaling changes the vector's length without changing its direction. The closest feasible point to an outside target therefore lies where that direction meets the sum-of-squares boundary.

For a linear objective, the score is largest when the decision vector points in the same direction as the coefficients. Scaling that direction to the boundary uses the complete available squared magnitude.

The generated vector satisfies SUM(POWER(x, 2)) \<= B by construction.

## Eligibility conditions

> • Every decision is REAL and belongs to exactly one component.
>
> • The component has exactly one explicit SUM(POWER(x, 2)) \<= B constraint.
>
> • Every decision occurs once with unit weight and no cross-term.
>
> • The objective is exactly a supported linear objective or squared-distance projection.
>
> • The signed or nonnegative domain is known.
>
> • No additional constraint or active coordinate bound changes the constructed solution.
>
> • B is finite, component-invariant, and nonnegative.
>
> • Zero directions and B = 0 use the explicit zero-vector branch.
>
> • Square roots, accumulation, scaling, empty input, NULL handling, decision identity, and output mapping preserve DECIDE semantics.
