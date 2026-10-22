# R1 — Continuous (fractional) knapsack

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

This construction covers the § Admitted class. The worked example is its special case: positive weights, zero lower
bounds, a `<=` budget.

An *item* is one `REAL` decision on one row; a row with several decisions gives several items. An item has a box
[l, u], where a bound of magnitude 1e20 or more is infinite; a weight w, its coefficient in the budget; and a value v,
its objective coefficient, negated under `MINIMIZE` and zero with no objective. The budget asks L ≤ Σ w·x ≤ U: `<=`
has L = −∞, `>=` has U = +∞, `=` has L = U. At a price λ per unit of budget, an item earns v − λ·w per unit.

An item with w ≠ 0 has:

> • a *break-even price* t = v / w;
>
> • a *high end*, the box end that spends more (u when w > 0, l when w < 0), and a *low end*, the other;
>
> • an *anchor* a, 0 clamped into the box, always finite;
>
> • *room up* w·(high − a) and *room down* w·(a − low), both nonnegative and in units of budget, either possibly
> infinite.

At a price λ, an item with t > λ belongs at its high end and one with t < λ at its low end. Spend therefore falls as the
price rises: a positive-weight row leaves its upper end, a negative-weight row reaches it. One sort by t finds the price.

For each group (the whole input when there is no `PER`):

> 1\. Items of rows outside the budget (its `WHEN` does not hold, or a `PER` key is NULL) and items with w = 0 take
> their own best end: u when v > 0, l when v < 0, the anchor when v = 0.
>
> 2\. Infeasible when a box is empty or the reachable spend [S − Σ down, S + Σ up] misses [L, U], where S = Σ w·a.
>
> 3\. Unbounded when a step-1 item has v ≠ 0 and an infinite best end, or when lo > hi. Here lo is the highest t of an
> item with infinite room up and hi the lowest t of one with infinite room down; under `<=` lo is raised to 0, under
> `>=` hi is lowered to 0.
>
> 4\. The target spend T is the zero-price spend S + Σ{t > 0} up − Σ{t < 0} down, clamped into [L, U]. When the
> zero-price spend already lies in [L, U], the budget does not bind.
>
> 5\. Sort the items by t, highest first. For each distinct t, running sums give A, the room up of every item with a
> higher t, and B, the room down of every item with a lower t. At price t the spend can be anything in
> S + A − B + [−Σ down, Σ up], the last two summed over the items tied at t. These ranges fall as t falls and meet end
> to end. The price λ* is the t whose range holds T.
>
> 6\. Items with t > λ* take their high end and items with t < λ* their low end. The items tied at λ* share
> D = T − (S + A − B): starting from their anchors, they move up when D > 0 and down when D < 0, each by its room or
> by what remains, found by a running sum over the tie. An infinite room takes all that remains.
>
> 7\. The items of a row return to one column per decision.

Steps 2 and 3 are aggregates over the group, decided after every row is read, infeasibility first. After them, every
sum steps 4 to 6 use at λ* is finite. When T lies on the shared end of two adjacent ranges, both give the same
assignment.

The budget directions differ only in T. `<=` and `>=` bind on their one side. `=` fixes T, so an exact fill continues
past items whose value is nonpositive, at a negative price if it must. `BETWEEN` takes the nearer side, so it binds on
at most one. A positive lower bound needs no baseline step: it is the low end of a positive-weight item. A data-valued
budget is one L and U per group (§ Admitted class), so the steps are unchanged.

The translation uses arithmetic, one sort, windowed running sums, grouped aggregates, clamping and, for several
decisions per row, an unpivot and a join back.

## Correctness

The argument runs item by item. It uses no sign of a weight, no order between items and no finite bound except at the
answer.

**Certificate.** Take a price λ and an assignment x inside every box with L ≤ Σ w·x ≤ U. Suppose each item with
v − λ·w > 0 is at u, each with v − λ·w < 0 is at l, λ > 0 only when Σ w·x = U, and λ < 0 only when Σ w·x = L. Then for
any feasible y,

    Σ v·y = Σ (v − λ·w)·y + λ·Σ w·y ≤ Σ (v − λ·w)·x + λ·Σ w·x = Σ v·x.

The first sum cannot grow when y replaces x, because each x sits at the end its own net value prefers. The second
cannot grow by the sign rule. So x is optimal. An item outside every budget enters only the first sum, with net value v.

**The construction meets it at λ*.** An item's net value at λ* is w·(t − λ*). For t > λ* it has the sign of w, so the
item prefers u when w > 0 and l when w < 0, its high end either way. For t < λ* the sign flips and the item prefers its
low end. So step 6 puts every item with a nonzero net value at the end that value prefers. Items tied at λ* have net value zero, and any point of their
box serves. The spend is T by construction. When λ* > 0 the zero-price spend exceeds T, because spend falls as the
price rises, so T is U; when λ* < 0, T is L. Step 1 is the same rule with λ = 0.

**Several decisions on one row.** The certificate sums over items, and nothing in it ties two items on one row
together. The solver's model has the same items and the same single constraint, since a linking constraint or
objective term keeps the query on the solver. The pieces of a benefit are therefore separate items in the ranking, and
the greedy fill matches the solver whether the piece values shrink or grow. The catalogue needs concavity only to
rebuild one variable from its pieces; R1 never rebuilds, because each decision answers in its own column.

**Infeasible.** Every assignment spends inside [S − Σ down, S + Σ up], so when that range misses [L, U], or a box is
empty, no assignment exists. Infeasibility is tested before boundedness because a problem with no assignment is
infeasible whatever its open bounds would allow.

**Unbounded.** When step 3 fails, a direction improves without end from any feasible assignment:

> • a step-1 item with v > 0 and u infinite, or v < 0 and l infinite, moves toward its open end;
>
> • an item with infinite room up and an item with infinite room down, the first with the higher t, move together at
> equal spend, and each unit of budget gains the difference of their t;
>
> • under `<=`, an item with infinite room down and t < 0 moves down: spend falls and value rises. Under `>=`, an item
> with infinite room up and t > 0 moves up.

When step 3 passes, an item with infinite room up and t > λ* would make A infinite, and one with infinite room down and
t < λ* would make B infinite, so neither holds at the range that holds a finite T. T is finite because step 3 rules out
an infinite zero-price spend on an open side. The answer is finite.

**Groups and scope.** Groups share no item and the objective is a sum over items, so each group is its own problem with
its own price. A row outside every budget is a group with no budget. The empty-aggregate error for a `WHEN` that covers
no row comes before any of this, as on the solver.

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
> required (§ Correctness). A constraint or objective term that links decisions stays on the
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
