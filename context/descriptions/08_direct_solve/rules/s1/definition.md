# S1 — Top-k and cardinality intervals

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

What the implementation admits today is in [done.md](done.md); this file is the class as the catalogue defines it.

## Worked example

S1 chooses Boolean items using their scores while enforcing a minimum and maximum number of selections.

| **Candidate** | **Department** | **Score** |
|:-------------:|----------------|-----------|
|       A       | Sales          | 9         |
|       B       | Sales          | 5         |
|       C       | Sales          | -1        |
|       D       | Research       | 8         |
|       E       | Research       | 4         |
|       F       | Research       | 3         |

Suppose each department must select between one and two candidates:

```sql
SELECT id, department, selected
FROM candidates
DECIDE selected(BOOL)
SUCH THAT SUM(selected) >= 1 PER department
AND SUM(selected) <= 2 PER department
MAXIMIZE SUM(score*selected);
```

The result selects scores 9 and 5 from Sales, and 8 and 4 from Research.

```sql
WITH ranked AS (
  SELECT *,
    ROW_NUMBER() OVER (
      PARTITION BY department
      ORDER BY score DESC, id
    ) AS rank,
    COUNT(*) FILTER (WHERE score > 0) OVER (
      PARTITION BY department
    ) AS positive_count
  FROM candidates
)
SELECT
  id,
  department,
  rank <= LEAST(2, GREATEST(1, positive_count)) AS selected
FROM ranked;
```

## Recognition

After normalization:

> • every free decision is Boolean;
>
> • the objective is linear: score\*selected;
>
> • the only coupling constraint counts selected items;
>
> • each selected item contributes exactly 1 to that count;
>
> • each group has a legal interval L <= selected count <= U.

A weighted constraint such as:

```sql
SUM(size*selected) <= capacity
```

does not belong to S1. It is a budget problem because different items consume different amounts.

## Supported variants

| **Variant** | **Meaning** |
|----|----|
| Exactly k | Set L = U = k. |
| At most k | Set L = 0 and U = k. |
| At least k | Set L = k and U to the group size. |
| Between L and U | Choose the best legal number of items. |
| One per group | Exact top-1 within each group. |
| Global top-k | Use one constant group for the entire input. |
| Grouped top-k | Partition ranking by the group key. |
| MINIMIZE | Rank the smallest scores first, or negate the scores. |
| Fixed decisions | Subtract fixed selected items from the remaining count bounds. |

Scores determine how many items should be selected:

> • positive scores are worth selecting;
>
> • negative scores are avoided unless the lower bound requires them;
>
> • zero scores may be selected without changing the objective.

If a group has p positive scores, the optimal count is:

```sql
selected count = LEAST(U, GREATEST(L, p))
```

## Relational construction

For each group, SQL:

> 1. adjusts the lower and upper counts for fixed decisions;
>
> 2. checks that the remaining count interval is feasible;
>
> 3. ranks free decisions by score;
>
> 4. counts the positive scores;
>
> 5. calculates the optimal legal prefix length;
>
> 6. selects that ranked prefix and joins the Boolean results back to the source rows.

The translation uses grouped counts, ROW_NUMBER, filtered window aggregates, LEAST, GREATEST, and joins.

## Correctness

For any fixed number m, selecting the m highest scores is optimal. Replacing a selected lower-scoring item with an
unselected higher-scoring item cannot reduce the objective.

The objective improves while additional scores are positive and worsens once additional scores are negative. Therefore,
the best unconstrained count is the number of positive scores. Clamping that count into [L, U] gives the best legal
count.

Stable ordering by decision identity chooses one deterministic answer when scores tie. Other tied selections may be
equally optimal.

## Eligibility conditions

> • Every free decision is Boolean and contributes exactly 1 to its count.
>
> • The objective is exactly linear with finite scores.
>
> • The only coupling is one cardinality interval per global or proved-disjoint group.
>
> • Paired lower and upper bounds use the same group membership and WHEN condition.
>
> • Count bounds are finite, decision-independent, and converted to inclusive integers.
>
> • Fixed selected decisions are subtracted from the remaining bounds.
>
> • A group with L > U produces DECIDE's infeasible result.
>
> • NULL PER keys and WHEN-excluded rows follow DECIDE's bypass semantics.
>
> • Empty input, ties, Boolean types, decision identity, and output mapping preserve DECIDE semantics.
