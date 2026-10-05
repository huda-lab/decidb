# A3 — Fixed-small multi-affine box

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

A3 covers a small group of bounded decisions whose objective may contain products such as x\*y, provided the objective is linear in each decision when the others are fixed.

For example:

```sql
SELECT id, x, y
FROM cases
DECIDE x(REAL), y(REAL)
SUCH THAT x BETWEEN 0 AND 1
AND y BETWEEN 0 AND 1
MAXIMIZE SUM(3*x*y - x + 2*y);
```

There are four corners to compare:

| **x** | **y** | **Objective** |
|:-----:|-------|---------------|
|   0   | 0     | 0             |
|   0   | 1     | 2             |
|   1   | 0     | -1            |
|   1   | 1     | 4             |

The best solution is x = 1, y = 1.

```sql
WITH corners AS (
SELECT id, x, y
FROM cases
CROSS JOIN (VALUES (0.0), (1.0)) xs(x)
CROSS JOIN (VALUES (0.0), (1.0)) ys(y)
)
SELECT id, x, y
FROM corners
QUALIFY ROW_NUMBER() OVER (
PARTITION BY id
ORDER BY 3*x*y - x + 2*y DESC
) = 1;
```

## Recognition

A3 extends A1 by allowing several decisions to interact through multi-affine products.

An objective is multi-affine when every decision has exponent at most one in each term:

x allowed

x\*y allowed

x\*y\*z allowed

x² not allowed

x²\*y not allowed

For example, x\*y is allowed because:

> • when y is fixed, the expression is linear in x;
>
> • when x is fixed, the expression is linear in y.

Every constraint must still be an individual bound. A coupled constraint such as x + y \<= 0.5 does not belong to A3 because the optimum may lie somewhere other than a box corner.

## Supported variants

| **Variant** | **Effect on the translation** |
|----|----|
| Number of decisions | With k decisions, SQL generates 2^k corners: 2 decisions give 4 corners, 3 give 8, and 4 give 16. |
| Cross-products | Products such as x\*y and x\*y\*z are supported as long as no decision is squared within a term. |
| REAL decisions | Each coordinate uses its lower or upper real endpoint. |
| INT decisions | Each coordinate uses its lowest or highest legal integer. |
| BOOL decisions | Each coordinate uses 0 or 1, subject to its bounds. |
| MINIMIZE or MAXIMIZE | The same corners are generated, but SQL reverses the ranking direction. |
| Independent components | SQL enumerates and ranks corners separately for each component. |

“Fixed-small” describes how the translation is formed. The complete set of 2^k corners must be generated. The correctness argument does not allow sampling only some corners.

## Relational construction

For each component, SQL:

> 1\. creates a two-row relation containing the lower and upper endpoint of every decision;
>
> 2\. cross joins those relations to generate every corner;
>
> 3\. evaluates the objective at each corner;
>
> 4\. ranks the corners and selects the best one;
>
> 5\. maps the selected values back to their decision identities.

## Correctness

Fix every decision except one. Because the objective is linear in the remaining decision, moving that decision to one of its endpoints cannot make the objective worse.

Repeat this for every decision. The process reaches a corner without worsening the objective. Therefore, at least one box corner is globally optimal, and evaluating every corner is sufficient.

## Eligibility conditions

> • Each component has a fixed finite number of BOOL, INT, or REAL decisions.
>
> • Every objective term is affine in each decision separately.
>
> • Coefficients do not depend on other decisions.
>
> • Constraints reduce entirely to finite individual bounds, with no coupled constraint.
>
> • SQL generates all 2^k corners for each component.
>
> • Empty intervals, empty input, numeric behavior, ties, decision identity, and output mapping preserve DECIDE semantics.
