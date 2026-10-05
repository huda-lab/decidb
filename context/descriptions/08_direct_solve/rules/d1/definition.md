# D1 — Exact keyed decomposition

Catalogue status: Exact.

> Any assignment with the same objective value is acceptable. Wherever this text describes a stable or deterministic
> tie order, it describes the catalogue's construction, not a requirement on the result.

## Worked example

D1 applies when one DECIDE query contains several independent subproblems. It proves the separation, rewrites each subproblem, and combines their assignments.

| **Candidate** | **Department** | **Score** |
|:-------------:|----------------|-----------|
|       A       | Sales          | 9         |
|       B       | Sales          | 6         |
|       C       | Research       | 5         |
|       D       | Research       | 8         |

Each department must select exactly one candidate:

```sql
SELECT id, department, selected
FROM candidates
DECIDE selected(BOOL)
SUCH THAT SUM(selected) = 1 PER department
MAXIMIZE SUM(score*selected);
```

The Sales choice cannot affect the Research choice. D1 therefore treats the query as two independent S1 problems:

| **Component** | **Direct result** |
|---------------|-------------------|
| Sales         | Select A          |
| Research      | Select D          |

```sql
WITH ranked AS (
SELECT
id,
ROW_NUMBER() OVER (
PARTITION BY department
ORDER BY score DESC, id
) AS score_rank
FROM candidates
),
assignments AS (
SELECT id, score_rank = 1 AS selected
FROM ranked
)
SELECT c.id, c.department, a.selected
FROM candidates c
JOIN assignments a USING (id);
```

The partitioned ranking is the S1 rewrite. D1 provides the proof that each department may use it independently and that the results may be joined safely.

## Recognition

After fixed decisions are substituted:

> • the remaining decisions split into disjoint components;
>
> • every constraint refers only to decisions in one component;
>
> • every nonseparable objective term also stays inside one component;
>
> • the global objective combines component values in a supported way;
>
> • every component matches a complete leaf rewrite such as A1, S1, or R1.

A useful way to detect the split is to connect decisions that occur together in a constraint or a nonseparable objective term. Separate connected components may be solved independently.

A query does not belong to D1 when a shared decision, resource constraint, scalar, or objective term connects the apparent groups. For example, one budget across all departments reconnects them.

## Supported variants

| **Variant** | **Handling** |
|----|----|
| One leaf class across many keys | Apply one partitioned SQL rewrite, as in the department example. |
| Different leaf classes | Rewrite each component with its matching rule, then union the assignment relations. |
| Split created by fixed decisions | Substitute the fixed values, rebuild the components, and rewrite the remaining pieces. |
| Additive objective | Optimize every component in the objective's requested direction. |
| Outer MIN or MAX | Compose only when improving every component in one proved direction cannot worsen the outer objective. |
| Infeasible component | The complete DECIDE problem is infeasible. |
| Favorably unbounded component | An additive objective is unbounded in that direction. |
| NULL PER key | The row bypasses that grouped factor and is handled by its actual remaining component. |

D1 inherits the status of every leaf it uses. Decomposition cannot make an unsupported or prototype leaf eligible by itself.

## Relational construction

D1 performs the following semantic steps:

> 1\. normalizes decision identity, coefficients, PER, and WHEN behavior;
>
> 2\. substitutes decisions whose values are already proved;
>
> 3\. connects decisions that share any remaining constraint or nonseparable objective term;
>
> 4\. finds the exact independent components;
>
> 5\. matches every component to a supported leaf rewrite;
>
> 6\. runs those relational rewrites and unions their typed assignment rows;
>
> 7\. verifies one assignment per decision and joins them back to the source rows.

The generated SQL depends on the leaf classes. It may use partitioned windows for S1, clamping for A1, or marginal ordering for R1. D1 contributes the partition keys and the final identity-based assembly.

## Correctness

If every constraint belongs to one component, the global feasible set is exactly the combination of the component feasible sets. Joining one feasible assignment from each component therefore gives a feasible global assignment. If one component is infeasible, no global assignment can satisfy all constraints.

For an additive objective, the total value is the sum of the component values. Replacing any component assignment with its local optimum cannot worsen that sum. Doing this for every component produces a global optimum.

An outer MIN or MAX needs an additional monotonicity proof. For example, maximizing the minimum of independent component values permits maximizing every component first, because increasing a component cannot reduce the minimum. Each required component optimum must be finite and attained.

Disjoint internal identities ensure that the union contains one assignment per decision. The final identity join reproduces entity and scalar values on the correct source rows.

## Eligibility conditions

> • All decisions fixed by bounds or another exact rule are substituted before the component split is proved.
>
> • Every remaining constraint and every nonseparable objective term belongs wholly to one component.
>
> • The objective is an additive combination of component objectives, or an outer MIN/MAX whose required direction is proved coordinatewise monotone.
>
> • Every component optimum needed by an outer MIN/MAX is finite and attained.
>
> • Under an additive objective, an infeasible component makes the global problem infeasible, while a component unbounded in the favorable direction makes it unbounded.
>
> • Exact row, entity, and scalar decision identities are known, and no decision identity occurs in more than one component.
>
> • Component membership follows the complete entity key and proved functional dependencies rather than coincidental equality of data values.
>
> • PER keys, WHEN masks, qualified reducers, join multiplicities, and data-valued right-hand sides are normalized before decomposition.
>
> • NULL PER keys bypass their grouped factor. Shared scalars or any factor that spans groups reconnect the affected decisions.
>
> • Every component matches one complete leaf rule and satisfies that rule's full **Valid when** checklist. A proof failure in any component rejects the complete D1 rewrite and keeps the original DECIDE query on the solver path.
>
> • Component keys and solver-read coefficients are non-NULL and finite wherever DECIDE requires them. Empty, infeasible, and unbounded outcomes preserve DECIDE's precedence and cannot appear as missing join rows.
>
> • Every leaf emits one typed assignment per internal decision identity. Assembly verifies complete, duplicate-free coverage and preserves source cardinality, entity and scalar fan-out, schema, ordering, status, NULL behavior, and numeric semantics.

## Exactness

Once independence and every leaf's eligibility are proved, the global feasibility and optimality arguments follow directly from the product of the component feasible sets. D1 introduces no approximation. If the split or any leaf cannot be proved, DeciDB keeps the undivided problem on the general solver path.
