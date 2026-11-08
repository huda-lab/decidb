# N1 — Euclidean-ball projection and support function

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

> • there is one decision, it is REAL and it is per row;
>
> • there is exactly one constraint of the form SUM(q \* POWER(x, 2)) \<= B, whose positive factor q is one constant (POWER(x, 2), x\*\*2, x\*x, norm(x, 2), SUM(2\*POWER(x, 2)) and SUM(POWER(2\*x, 2)) all read this way);
>
> • the objective is linear, or one convex square K\*x² on every row with a linear term (squared distance to a target vector, or a square plus a linear term), or absent;
>
> • no other constraint applies: the box is exactly [0, ∞) or (−∞, ∞).

A row-valued factor such as:

SUM(POWER(weight \* x, 2)) \<= B

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
| B \< 0 with an instance | Infeasible. |
| Independent groups | Compute a separate scale for every group. |
| Square plus a linear term | Complete the square, then project. |
| A row outside every ball (a `when`, a NULL key, no limit) | With a square, keep the target. Without one, return 0 when the direction is 0; otherwise unbounded. |
| No objective | Return the zero vector. |

For projection, let y be the target vector after applying the domain rule:

if SUM(y²) \<= B:

x = y

otherwise:

x = SQRT(B / SUM(y²)) \* y

For a linear objective, let d be the objective direction:

x = SQRT(B / SUM(d²)) \* d

A constant factor q on the sum of squares replaces B with B / q.

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

**Completing the square.** With K \> 0, each row's K\*x² + b\*x equals K\*(x − y)² − K\*y², where y = −b / (2\*K). K is one constant on every row with a linear term, so the objective is K times the squared distance to y, plus a constant, and minimizing it projects y onto the feasible set. A row with no square and no linear term has K = b = 0 and y = 0: it adds nothing to the objective, and x = 0 leaves the most of the ball to the others.

**Projecting onto the ball and the nonnegative orthant.** Let y⁺ = GREATEST(y, 0). The projection of y onto the ball intersected with the orthant equals the projection of y⁺ onto the ball:

- Take a feasible x and set x_i = 0 on every row with y_i \< 0. It stays in the orthant, its length does not grow, and its distance to y does not grow, because (x_i − y_i)² ≥ y_i² when x_i ≥ 0 and y_i \< 0. So a best x has x_i = 0 on those rows, and those rows add a constant to the distance.
- On the other rows y_i = y⁺_i, so the best x is the point of the ball and the orthant closest to y⁺.
- The projection of y⁺ onto the ball is y⁺ scaled by a factor at most 1, which is in the orthant. It is the closest point of the ball, so it is the closest point of the smaller set too.

**A linear objective on the same set.** Maximizing d·x has the same first step, with d⁺ = GREATEST(d, 0): x_i = 0 where d_i \< 0 only raises the objective. Then d⁺·x ≤ ‖d⁺‖ \* ‖x‖ ≤ ‖d⁺‖ \* SQRT(B) (Cauchy-Schwarz), with equality at x = SQRT(B / SUM(d⁺²)) \* d⁺, which is feasible.

## Eligibility conditions

> • There is one REAL decision per row, and a row belongs to at most one component.
>
> • Each component has one SUM(q \* POWER(x, 2)) \<= B constraint, with one constant q \> 0.
>
> • The objective is exactly a supported linear objective, or one convex square with its linear terms, or absent.
>
> • The domain is exactly [0, ∞) or (−∞, ∞). Any other bound declines, even one that would not bind.
>
> • B is one constant of magnitude below 1e20, or a value of the source row, where the tightest value in a component applies and a NULL has no limit, or an aggregate of the component's rows. B \< 0 with an instance is infeasible.
>
> • Zero directions and B = 0 use the explicit zero-vector branch.
>
> • Square roots, accumulation, scaling, empty input, NULL handling, decision identity, and output mapping preserve DECIDE semantics.
