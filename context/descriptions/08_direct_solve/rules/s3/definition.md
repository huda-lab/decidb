# S3 — Maximum item count under one budget

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

S3 selects as many Boolean items as possible while staying within one shared budget.

Each item has a **weight**, meaning how much of the budget it consumes.

| **Item** | **Weight** |
|----------|------------|
| A        | 2          |
| B        | 3          |
| C        | 4          |
| D        | 6          |

With a budget of 9, the best answer selects A, B, and C.

```sql
SELECT id, selected
FROM items
DECIDE selected(BOOL)
SUCH THAT SUM(weight*selected) <= 9
MAXIMIZE SUM(selected);
```

SQL sorts by smallest weight and finds the longest affordable prefix:

```sql
WITH ranked AS (
SELECT *,
ROW_NUMBER() OVER (
ORDER BY weight, id
) AS rank,
SUM(weight) OVER (
ORDER BY weight, id
ROWS UNBOUNDED PRECEDING
) AS prefix_weight
FROM items
),
prefixes AS (
SELECT 0 AS k, 0 AS prefix_weight
UNION ALL
SELECT rank, prefix_weight
FROM ranked
),
best AS (
SELECT MAX(k) AS k
FROM prefixes
WHERE prefix_weight <= 9
)
SELECT
id,
rank <= k AS selected
FROM ranked
CROSS JOIN best;
```

The prefix weights are 2, 5, 9, and 15, so the largest feasible prefix has three items.

## Recognition

After normalization:

> • every free decision is Boolean;
>
> • there is exactly one additive upper budget;
>
> • each item may have a different weight;
>
> • every selected item contributes the same positive amount to the objective;
>
> • no other constraint couples the decisions.

The objective may therefore be written as:

MAXIMIZE constant \* SUM(selected)

A problem with different item values does not belong to S3:

MAXIMIZE SUM(value\*selected)

That is a general 0-1 knapsack problem. The cheapest items may not provide the greatest value.

## Supported variants

| **Variant** | **Handling** |
|----|----|
| Positive weights | Sort from smallest to largest and take the longest affordable prefix. |
| Zero weights | They appear at the start and can be selected without consuming budget. |
| Negative weights | They provide budget credit and also appear first. Every prefix must still be checked. |
| Fixed selected items | Subtract their weights from the remaining budget. |
| Common objective multiplier | Any shared positive multiplier gives the same maximum-count solution. |
| Independent groups | Solve separately only when the groups and their budgets are proved disjoint. |
| Tied weights | Use stable decision identity to choose one optimal prefix. |

Negative weights require care. The empty prefix may be infeasible when the budget is negative, while a prefix containing negative-weight items may be feasible. SQL must therefore test the empty prefix and every running prefix rather than stopping at the first failure.

## Relational construction

For each independent budget component, SQL:

> 1\. subtracts the weight of fixed selected items from the budget;
>
> 2\. sorts free items by ascending weight;
>
> 3\. computes the running prefix weight;
>
> 4\. includes the empty prefix with size zero;
>
> 5\. finds the largest prefix whose weight satisfies the budget;
>
> 6\. marks that prefix as selected and joins the Boolean results back to the source rows.

The translation uses sorting, ROW_NUMBER, a windowed running SUM, MAX, and joins.

## Correctness

Among all subsets containing exactly k items, the k smallest weights have the smallest total weight.

Therefore:

a feasible size-k subset exists

if and only if

the k-smallest prefix fits the budget

The largest feasible prefix gives the greatest possible item count, which is exactly the objective.

## Eligibility conditions

> • Every free decision is Boolean.
>
> • Every selected item has the same strictly positive objective coefficient.
>
> • There is exactly one additive upper budget per independent component.
>
> • Item weights and the budget are finite and decision-independent.
>
> • Fixed selected items are stored and subtracted from the remaining budget.
>
> • The empty prefix and every sorted prefix are considered.
>
> • If no prefix is feasible, the result is DECIDE infeasibility rather than an empty assignment.
>
> • Grouped or conditional forms have been separated into proved-disjoint components.
>
> • Empty input, NULL handling, numeric comparisons, ties, Boolean types, decision identity, and output mapping preserve DECIDE semantics.
