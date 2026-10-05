# N2 — Linear support over a diagonal ellipsoid

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

N2 is the weighted version of N1.

Each decision contributes a different amount to the shared sum-of-squares bound:

SUM(weight \* POWER(x, 2)) \<= B

For example:

| **Item** | **Coefficient** | **Weight** |
|:--------:|-----------------|------------|
|    A     | 1               | 1          |
|    B     | 4               | 4          |

```sql
SELECT id, x
FROM directions
DECIDE x(REAL)
SUCH THAT SUM(weight * POWER(x, 2)) <= 5
MAXIMIZE SUM(coefficient*x);
```

The optimal result is x_A = 1 and x_B = 1:

constraint = 1×1² + 4×1² = 5

objective = 1×1 + 4×1 = 5

```sql
WITH prepared AS (
SELECT
id,
weight,
GREATEST(coefficient, 0.0) AS direction
FROM directions
),
normalizer AS (
SELECT SUM(POWER(direction, 2) / weight) AS h
FROM prepared
)
SELECT
id,
CASE
WHEN h = 0 THEN 0.0
ELSE SQRT(5.0 / h) * direction / weight
END AS x
FROM prepared
CROSS JOIN normalizer;
```

## Recognition

After normalization:

> • all decisions are REAL;
>
> • there is exactly one constraint of the form SUM(weight\*x²) \<= B;
>
> • every weight is strictly positive;
>
> • the quadratic constraint has no cross-terms such as x\*y;
>
> • the objective is linear;
>
> • no other constraint couples the decisions.

N1 is the special case where every weight equals 1.

N2 does not include squared-distance projection onto the ellipsoid. With unequal weights, the closest point is generally not obtained by simply scaling the target vector.

## Supported variants

| **Variant** | **Direct rule** |
|----|----|
| Different positive weights | Larger weights make a coordinate consume more of the shared bound. |
| Linear MAXIMIZE | Use the objective coefficients as the direction. |
| Linear MINIMIZE | Negate the objective coefficients first. |
| Signed decisions | Use the complete direction vector. |
| Nonnegative decisions | Replace negative directions with zero. |
| Zero direction | Return the zero vector. |
| B = 0 | Return the zero vector. |
| Independent groups | Compute a separate normalizer for every group. |

Weight values matter directly:

> • a small weight makes a coordinate cheaper;
>
> • a large weight makes it more expensive;
>
> • a zero weight may leave a direction unbounded;
>
> • a negative weight does not define an ellipsoid.

The solution divides each objective direction by its weight before applying the shared scale:

h = SUM(direction² / weight)

x_i = SQRT(B / h) \* direction_i / weight_i

## Relational construction

For each component, SQL:

> 1\. converts MINIMIZE or MAXIMIZE into one objective direction;
>
> 2\. applies the signed or nonnegative domain rule;
>
> 3\. computes SUM(direction² / weight);
>
> 4\. calculates the shared scale SQRT(B / h);
>
> 5\. divides each direction by its weight and applies the scale;
>
> 6\. joins the resulting values back to their decision identities.

The rewrite uses grouped sums, division, SQRT, CASE, scalar multiplication, and joins.

## Correctness

Introduce a scaled coordinate:

z_i = SQRT(weight_i) \* x_i

The weighted constraint becomes an ordinary sum-of-squares constraint:

SUM(z_i²) \<= B

The transformed objective direction becomes:

coefficient_i / SQRT(weight_i)

The problem has therefore been reduced to N1. Aligning with the transformed direction gives the best linear objective, and converting back produces the SQL formula above.

Substituting the result into the original constraint gives exactly B, unless the direction is zero.

## Eligibility conditions

> • Every decision is REAL and belongs to exactly one component.
>
> • The component has exactly one explicit SUM(weight\*POWER(x, 2)) \<= B constraint.
>
> • Every effective weight is finite and strictly positive.
>
> • There are no quadratic cross-terms.
>
> • The objective is exactly linear.
>
> • The signed or nonnegative domain is known.
>
> • No additional coupling constraint or active coordinate bound changes the solution.
>
> • B is finite, component-invariant, and nonnegative.
>
> • Zero directions and B = 0 use the explicit zero-vector branch.
>
> • Accumulation, division, square roots, scaling, empty input, NULL handling, decision identity, and output mapping preserve DECIDE semantics.
