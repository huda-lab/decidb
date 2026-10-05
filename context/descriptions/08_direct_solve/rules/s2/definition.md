# S2 — Nested upper quotas

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

S2 selects high-scoring Boolean items while respecting a hierarchy of upper quotas.

For example:

| **Candidate** | **Division** | **Department** | **Score** |
|:-------------:|--------------|----------------|-----------|
|       A       | North        | Sales          | 9         |
|       B       | North        | Sales          | 7         |
|       C       | North        | Research       | 8         |
|       D       | South        | Sales          | 6         |
|       E       | South        | Sales          | 5         |
|       F       | West         | Research       | 4         |

Suppose we may select:

> • at most one candidate per department;
>
> • at most one per division;
>
> • at most two overall.

```sql
SELECT id, division, department, selected
FROM candidates
DECIDE selected(BOOL)
SUCH THAT SUM(selected) <= 1 PER (division, department)
AND SUM(selected) <= 1 PER division
AND SUM(selected) <= 2
MAXIMIZE SUM(score*selected);
```

The department stage keeps scores 9, 8, 6, and 4. The division stage keeps 9, 6, and 4. The global stage selects 9 and 6.

```sql
WITH department_stage AS (
SELECT *
FROM candidates
WHERE score > 0
QUALIFY ROW_NUMBER() OVER (
PARTITION BY division, department
ORDER BY score DESC, id
) <= 1
),
division_stage AS (
SELECT *
FROM department_stage
QUALIFY ROW_NUMBER() OVER (
PARTITION BY division
ORDER BY score DESC, id
) <= 1
),
chosen AS (
SELECT *
FROM division_stage
QUALIFY ROW_NUMBER() OVER (
ORDER BY score DESC, id
) <= 2
)
SELECT
c.id,
chosen.id IS NOT NULL AS selected
FROM candidates c
LEFT JOIN chosen USING (id);
```

## Recognition

After normalization:

> • every free decision is Boolean;
>
> • the objective is linear: score\*selected;
>
> • every coupling constraint is an upper count quota;
>
> • each selected item contributes exactly 1 to every quota it belongs to;
>
> • quota memberships form a nested hierarchy.

The hierarchy must be **laminar**. For any two quota sets:

> • they are disjoint;
>
> • or one set is completely contained inside the other.

Departments inside divisions inside a global set satisfy this rule.

Region quotas and product quotas usually do not. Their memberships can overlap without either set containing the other, so independent ranking stages may discard the globally best combination.

## Supported variants

| **Variant** | **Handling** |
|----|----|
| Different hierarchy depths | Process quota levels from the deepest sets to the root. |
| Different caps per group | Rank separately using each group's own cap. |
| Global cap | Apply one final ranking without a partition. |
| Zero cap | Remove every free item in that quota. |
| Cap above group size | The quota has no effect on the free items. |
| Fixed selected items | Subtract them from every quota they belong to. |
| MINIMIZE | Negate the scores, then use the same procedure. |
| Negative scores | Do not select them because every quota is an upper bound. |
| Tied scores | Use stable decision identity to choose one optimal result. |
| WHEN or NULL PER bypass | Pass nonmembers through to any applicable ancestor quota. |

Lower count requirements are not part of S2. They can force selections that the bottom-up upper-quota procedure would otherwise remove.

## Relational construction

SQL:

> 1\. proves the quota sets form a nested hierarchy;
>
> 2\. subtracts fixed selected items from every applicable quota;
>
> 3\. rejects any quota whose remaining cap is negative;
>
> 4\. removes free items with no positive benefit;
>
> 5\. starts at the deepest quota level and retains the highest-scoring prefix under each cap;
>
> 6\. repeats the ranking on the survivors at every parent level;
>
> 7\. maps the final survivors to TRUE and all other free decisions to FALSE.

Each quota level becomes another ROW_NUMBER stage.

## Correctness

At a leaf quota with cap q, only its q highest positive scores can ever be useful. Replacing one of them with a lower-scoring item cannot improve the objective.

After processing the children of a parent quota, any feasible child selection can be replaced by an equally good or better selection using only the surviving rows. The parent can therefore rank those survivors without losing an optimal solution.

Because child quota sets are disjoint, and every parent fully contains its children, repeating this argument up to the root produces a global optimum.

Each stage only removes rows, so quotas satisfied at lower levels remain satisfied.

## Eligibility conditions

> • Every free decision is Boolean and contributes exactly 1 to each joined quota.
>
> • The objective is exactly linear with finite scores.
>
> • Every coupling constraint is an upper cardinality quota.
>
> • Quota memberships form a proved laminar hierarchy.
>
> • Caps are finite, decision-independent, and converted to inclusive integers.
>
> • Fixed selected decisions are subtracted from every quota they join.
>
> • A negative remaining cap produces DECIDE's infeasible result.
>
> • WHEN and NULL PER membership follows DECIDE's bypass semantics.
>
> • Empty input, zero caps, ties, Boolean types, decision identity, and output mapping preserve DECIDE semantics.
