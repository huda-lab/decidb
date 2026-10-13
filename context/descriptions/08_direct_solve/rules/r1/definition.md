# R1 — Continuous piecewise-linear allocation

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

R1 divides one resource among continuous decisions. Each additional unit has a known marginal benefit, and a decision may be partially filled.

| **Material** | **Available amount** | **Unit resource** | **Unit benefit** |
|:------------:|----------------------|-------------------|------------------|
|      A       | 4                    | 2                 | 10               |
|      B       | 3                    | 3                 | 9                |
|      C       | 5                    | 2                 | 4                |

With a resource limit of 14, the benefit per unit of resource is 5 for A, 3 for B, and 2 for C. The optimum takes all four units of A and two units of B.

```sql
SELECT id, amount
FROM materials
DECIDE amount(REAL)
SUCH THAT amount BETWEEN 0 AND available
AND SUM(unit_resource*amount) <= 14
MAXIMIZE SUM(unit_benefit*amount);
```

```sql
WITH ranked AS (
SELECT *,
unit_benefit / unit_resource AS density,
COALESCE(
SUM(unit_resource*available) OVER (
ORDER BY unit_benefit/unit_resource DESC, id
ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
),
0.0
) AS prior_resource
FROM materials
)
SELECT
id,
LEAST(
available,
GREATEST(0.0, (14.0-prior_resource)/unit_resource)
) AS amount
FROM ranked;
```

The result is A = 4, B = 2, and C = 0. Only B is partially filled.

## Recognition

After normalization:

> • every decision is REAL with finite lower and upper bounds;
>
> • the objective is a sum of independent linear or concave piecewise-linear benefits;
>
> • each resource coefficient is strictly positive;
>
> • there is exactly one shared resource upper bound or exact-fill equality;
>
> • no other constraint or objective term couples the decisions.

Ordinary fractional knapsack is the one-segment version of R1. A piecewise-linear benefit belongs to R1 when its marginal benefits do not increase as more of the same decision is allocated.

BOOL or INT decisions do not belong to R1 because they cannot partially fill the final segment. Two resource constraints also destroy the single density ordering.

## Supported variants

| **Variant** | **Handling** |
|----|----|
| One linear segment per decision | Rank decisions by benefit/resource. |
| Several concave segments | Emit one row per segment and rank all marginal segments. |
| Resource upper bound | Stop when the resource is exhausted or the next marginal benefit is nonpositive. |
| Exact resource equality | Continue until the exact resource amount is filled, even if later marginal benefits are negative. |
| Positive lower bounds | Allocate every lower bound first and rank only the remaining segment capacity. |
| Tied densities | Stable decision and segment order chooses one optimal allocation. |
| Independent groups | Rank and fill separately for every proved-disjoint resource component. |

For an exact-fill equality, the required resource must lie between the resource used by all lower bounds and the resource used by all upper bounds. Continuity allows at most one final segment to be partially filled.

## Relational construction

For each independent resource component, SQL:

> 1\. starts every decision at its lower bound;
>
> 2\. subtracts that baseline resource from the available amount;
>
> 3\. converts each remaining linear piece into a row containing its resource capacity and marginal benefit per unit of resource;
>
> 4\. ranks the segment rows by decreasing marginal benefit per resource;
>
> 5\. computes the resource consumed by earlier segments;
>
> 6\. fills each segment completely or partially using LEAST and GREATEST;
>
> 7\. combines the selected segments back into one value per decision.

The translation uses arithmetic, sorting, windowed prefix sums, clamping, grouping, and joins.

## Correctness

Suppose an allocation uses resource in a lower-density segment while a higher-density segment still has capacity. Moving the same amount of resource to the higher-density segment improves the objective or leaves it unchanged when the densities tie.

Repeating this exchange produces the density-ordered prefix used by SQL. Concavity ensures that an earlier segment of one decision never appears after one of its later segments, so the reconstructed decision remains legal.

Every selected segment stays within its capacity. Continuous decisions allow the final segment to use exactly the remaining resource when equality requires it.

## Eligibility conditions

> • Every decision is REAL with finite, nonempty bounds.
>
> • The objective is exactly separable, continuous, and concave piecewise linear after converting the requested direction to maximization.
>
> • Every segment decomposition is finite and has nonincreasing marginal benefit.
>
> • There is exactly one resource upper bound or equality with finite, strictly positive resource coefficients.
>
> • There is no second resource, cross-decision objective term, or secondary objective.
>
> • Lower-bound resource use and total remaining capacity are checked before filling.
>
> • Inconsistent bounds or an unreachable exact resource requirement produce DECIDE infeasibility.
>
> • WHEN, PER, NULL, numeric, tie, decision-identity, and output-mapping semantics are preserved.

## Admitted class

What DeciDB admits beyond, or in place of, the catalogue text. Where the two differ, this section holds.

> • **Weights of any sign.** A weight may be a column or expression and zero or negative on some rows; the plan cannot
> prove it positive before it reads the rows (column statistics do not help: the DECIDE pass runs before DuckDB
> propagates them, most weights are expressions or come from joins or files without them, and a prepared plan keeps
> its choice while rows change). One price per unit of budget settles every row: a row goes to its upper
> bound when its value beats price × weight, and to its lower bound otherwise. A row with a nonpositive weight and a
> positive value takes its upper bound without competing for the budget.
>
> • **Bounds of any size.** A bound, constant or per row, of magnitude 1e20 or more is no bound, as on the solver. An
> uncapped row needs its value no more than price × weight and a floorless row needs it no less; when no price meets
> all of them, the problem is unbounded. That is decided after every row is read, as infeasibility is.
>
> • **Pieces.** DECIDE has no piecewise syntax. Pieces written as rows are plain R1. Pieces written as several `REAL`
> decisions on one row sharing the budget are admitted, each (row, decision) an item in the ranking; nothing in the
> query orders or recombines them, so the ranking matches the solver whatever the piece values. Concavity is not
> required, pending the check in `todo.md` § R1-06. A constraint or objective term that links decisions stays on the
> solver.
>
> • **Budget direction.** `<=`, `=`, `>=` and `BETWEEN`. Each row starts at its own best end; when that spend misses
> the budget, rows move toward it in order of least value lost per unit of budget. `BETWEEN` binds on at most one side.
> A required spend outside what the boxes can reach is infeasible.
>
> • **Budget scope.** `PER` gives each group its own budget and price. A `WHEN` on the budget, on the clause or on the
> aggregate (the same meaning for a constant budget), and a NULL `PER` key, leave a row out of every budget; such a
> row takes its own best end, as an A1 row does. A budget whose `WHEN` covers no row of a nonempty source raises the
> empty-aggregate error, as the solver does.
>
> • **Errors.** An empty box on any row is infeasible; NULL or NaN values raise as they do on the solver.
