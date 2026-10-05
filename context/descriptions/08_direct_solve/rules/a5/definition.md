# A5 — Objective-aligned monotone corner

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

A5 applies when every decision has a direction that:

> • improves the objective;
>
> • makes every constraint easier to satisfy.

For example:

| **Item** | **Upper bound** | **Value** | **Requirement** |
|:--------:|-----------------|-----------|-----------------|
|    A     | 4               | 5         | 2               |
|    B     | 3               | 1         | 1               |

```sql
SELECT id, x
FROM capacity
DECIDE x(INT)
SUCH THAT x BETWEEN 0 AND upper_bound
AND SUM(requirement*x) >= 8
MAXIMIZE SUM(value*x);
```

All values and requirements are positive. Increasing either decision improves the objective and increases the left side of the demand constraint.

The optimal values are therefore the upper bounds:

x_A = 4

x_B = 3

requirement supplied = 2×4 + 1×3 = 11

objective = 5×4 + 1×3 = 23

The direct SQL selects that corner:

```sql
WITH corner AS (
SELECT
id,
upper_bound AS x,
requirement,
value
FROM capacity
),
validation AS (
SELECT SUM(requirement*x) >= 8 AS feasible
FROM corner
)
SELECT id, x
FROM corner
CROSS JOIN validation
WHERE feasible;
```

In the generated rewrite, a failed validation produces DECIDE's infeasible result rather than an ordinary empty relation.

## Recognition

For every decision, the system must identify one movement direction:

move toward the lower endpoint

or

move toward the upper endpoint

Moving in that direction must:

> • never worsen the objective;
>
> • never make an inequality harder to satisfy;
>
> • leave every equality unchanged.

A5 may contain coupled constraints and cross-products. What matters is that all of them agree with the same direction for each decision.

The difference from A3 is that A3 evaluates every corner. A5 proves in advance that one particular corner is at least as good as every other feasible point.

This is not A5:

MAXIMIZE x + y

SUCH THAT x + y \<= 1

Increasing both decisions improves the objective but violates the shared constraint.

## Supported variants

| **Variant** | **Required direction** |
|----|----|
| Maximize a positive contribution | Move toward the upper endpoint. |
| Minimize a positive contribution | Move toward the lower endpoint. |
| Negative objective coefficient | The preferred direction reverses. |
| Mixed directions | Some decisions may move upward while others move downward. |
| BOOL, INT, or REAL | Use the legal endpoint for the declared domain. |
| Coupled inequality | Allowed only if every selected movement makes it easier to satisfy. |
| Equality constraint | Allowed only if all selected movements leave it unchanged. |
| Cross-product | Allowed only if its direction can be proved over the complete box. |

For example, maximizing x\*y is monotone in both decisions when x ≥ 0 and y ≥ 0. If either variable may be negative, the improving direction can change, so the A5 proof may fail.

Constraint direction also matters:

a\*x + b\*y \>= demand, with a,b \>= 0 compatible with moving upward

a\*x + b\*y \<= capacity, with a,b \>= 0 not compatible with moving upward

## Relational construction

For each component, SQL:

> 1\. determines the objective-improving direction of every decision;
>
> 2\. verifies that every constraint agrees with those directions;
>
> 3\. selects the corresponding lower or upper endpoint;
>
> 4\. evaluates every original constraint at that corner;
>
> 5\. returns the corner or reports infeasibility.

No candidate enumeration or ranking is required.

## Correctness

Start from any feasible solution and move its decisions, one at a time, toward their selected endpoints.

Each move preserves feasibility and does not worsen the objective. The process eventually reaches the selected corner. Therefore, that corner is at least as good as every feasible starting point and is globally optimal.

The same argument proves infeasibility. If the selected corner violates a constraint, no other point can satisfy it because every other point lies in a less favorable direction.

## Eligibility conditions

> • Every decision has one objective-improving direction over its complete interval.
>
> • Every inequality becomes no harder to satisfy in those directions.
>
> • Every equality remains unchanged.
>
> • Sign proofs cover all objective and constraint terms, including cross-products.
>
> • Each selected endpoint is finite and legal for its BOOL, INT, or REAL domain.
>
> • WHEN, PER, NULL, and repeated-decision semantics have been included in the direction proof.
>
> • SQL validates every normalized constraint at the selected corner.
>
> • Empty input, infeasibility, numeric behavior, decision identity, and output mapping preserve DECIDE semantics.
