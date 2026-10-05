# S4 — Exact count and budget, maximizing the best selected item

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

S4 chooses exactly k Boolean items within one budget. The objective depends only on the highest reward among the selected items.

| **Item** | **Reward** | **Weight** |
|:--------:|------------|------------|
|    A     | 10         | 8          |
|    B     | 9          | 6          |
|    C     | 4          | 2          |
|    D     | 3          | 1          |

Suppose we must select exactly two items with a budget of 9:

```sql
SELECT id, selected
FROM items
DECIDE selected(BOOL)
SUCH THAT SUM(selected) = 2
AND SUM(weight*selected) <= 9
MAXIMIZE MAX(reward*selected);
```

The best result selects A and D:

total weight = 8 + 1 = 9

largest reward = 10

A is the **anchor**, meaning the item that determines the objective. D is the cheapest companion that completes the required count.

```sql
WITH ranked AS (
SELECT *,
ROW_NUMBER() OVER (
ORDER BY weight, id
) AS weight_rank,
SUM(weight) OVER (
ORDER BY weight, id
ROWS UNBOUNDED PRECEDING
) AS prefix_weight
FROM items
),
prefixes AS (
SELECT
MAX(prefix_weight) FILTER (WHERE weight_rank = 1) AS prefix_1,
MAX(prefix_weight) FILTER (WHERE weight_rank = 2) AS prefix_2
FROM ranked
),
anchors AS (
SELECT r.*,
CASE
WHEN weight_rank <= 2 THEN prefix_2
ELSE weight + prefix_1
END AS extension_weight
FROM ranked r
CROSS JOIN prefixes
),
winner AS (
SELECT *
FROM anchors
WHERE extension_weight <= 9
QUALIFY ROW_NUMBER() OVER (
ORDER BY reward DESC, id
) = 1
),
fillers AS (
SELECT i.id
FROM items i
CROSS JOIN winner w
WHERE i.id <> w.id
QUALIFY ROW_NUMBER() OVER (
ORDER BY i.weight, i.id
) <= 1
),
chosen AS (
SELECT id FROM winner
UNION ALL
SELECT id FROM fillers
)
SELECT
i.id,
chosen.id IS NOT NULL AS selected
FROM items i
LEFT JOIN chosen USING (id);
```

## Recognition

After normalization:

> • every decision is Boolean;
>
> • exactly k items must be selected;
>
> • there is one additive upper budget;
>
> • the objective is exactly MAX(reward\*selected);
>
> • rewards are finite and nonnegative;
>
> • no other objective term values the companion items.

Replacing MAX with SUM does not belong to S4:

MAXIMIZE SUM(reward\*selected)

That is a 0-1 knapsack problem because the rewards of all selected items matter.

## Supported variants

| **Variant** | **Handling** |
|----|----|
| k = 1 | Choose the highest-reward item that fits the budget. |
| k = 0 | Select nothing when zero weight satisfies the budget. |
| k = number of items | Select everything if the total weight fits. |
| Anchor among the k lightest items | Its cheapest extension is the ordinary k-item weight prefix. |
| Anchor outside the k lightest items | Add the anchor to the k−1 lightest other items. |
| Negative or zero weights | They appear first among the cheapest companions. |
| Tied rewards | Stable decision identity chooses one optimal anchor. |
| Tied weights | Stable decision identity chooses deterministic companions. |
| Independent groups | Solve separately only when count and budget components are proved disjoint. |

Fixed selected items are not part of this class. A fixed selected item may already determine the maximum reward, which changes the anchor logic.

## Relational construction

For each independent component, SQL:

> 1\. validates that k is one exact integer;
>
> 2\. handles empty input, k = 0, and impossible counts;
>
> 3\. sorts items by weight;
>
> 4\. records the weight of the cheapest k and k−1 prefixes;
>
> 5\. computes the cheapest size-k extension containing each possible anchor;
>
> 6\. keeps anchors whose extension satisfies the budget;
>
> 7\. selects the feasible anchor with the highest reward;
>
> 8\. returns that anchor with its k−1 cheapest companions.

## Correctness

Consider any feasible size-k selection. One selected item determines its maximum reward. Call that item the anchor.

Replacing the other selected items with the k−1 cheapest possible companions cannot increase the total weight. Therefore, if the original selection is feasible, the cheapest extension of its anchor is also feasible.

Conversely, every feasible anchor extension is a valid size-k selection. Choosing the highest-reward feasible anchor therefore produces the greatest maximum reward attainable by any feasible selection.

## Eligibility conditions

> • Every decision is a free Boolean decision. Fixed selections are not admitted.
>
> • The count is one finite, exact, component-invariant integer k.
>
> • The objective is exactly MAX(reward\*selected) with finite nonnegative rewards.
>
> • The only coupling constraints are the exact count and one additive upper budget.
>
> • Weights and the budget are finite and decision-independent.
>
> • There are no additional quotas, budgets, or objective terms.
>
> • k \< 0, k above the item count, or no feasible anchor produces DECIDE infeasibility.
>
> • k = 0, empty input, grouped components, NULL handling, numeric comparisons, ties, Boolean types, decision identity, and output mapping preserve DECIDE semantics.
