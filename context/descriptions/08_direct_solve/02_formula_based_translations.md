# Formula-Based Translations

These translations apply when the optimization reduces to independent decisions, a
fixed-small candidate set, one shared value, one affine expression, or one simple
geometric constraint.

| Rule | Shape | Direct idea | Status |
|---|---|---|---|
| A1 | Independent bounded decisions | Endpoints and stationary point | Exact |
| A2 | One shared location | Mean, median, midpoint, or mode | Exact |
| A3 | Fixed-small multi-affine box | Enumerate corners | Exact |
| A4 | Two bounded regression parameters | Interior, edges, and corners | Prototype |
| A5 | Every improving direction preserves constraints | Emit one box corner | Exact |
| A6 | One squared affine residual | Project onto a reachable interval | Exact |
| N1 | One L1/L2/Linf ball | Clamp, scale, or threshold | Exact |
| N2 | One diagonal ellipsoid | Aggregate normalization | Exact |

## A1 — Independent bounded decisions

**Status:** Exact translation

### Problem

Every decision can be optimized independently inside an interval. Its objective is
linear or quadratic.

### DECIQL

```sql
SELECT id, x
FROM items
DECIDE x(REAL)
SUCH THAT x BETWEEN lo AND hi
MINIMIZE SUM(POWER(SQRT(q) * x, 2) + p * x);
```

### Direct SQL

```sql
WITH normalized AS (
    SELECT _decision_id,
           lo AS lower_bound, hi AS upper_bound,
           q AS quadratic_coefficient, p AS linear_coefficient,
           1.0 AS sense,       -- 1 for MINIMIZE; -1 for MAXIMIZE
           'REAL' AS domain
    FROM normalized_decisions
), candidates(_decision_id, x) AS (
    SELECT _decision_id, lower_bound AS x FROM normalized
    UNION ALL
    SELECT _decision_id, upper_bound FROM normalized
    UNION ALL
    SELECT _decision_id,
           LEAST(upper_bound, GREATEST(
               lower_bound,
               -linear_coefficient / (2*quadratic_coefficient)
           ))
    FROM normalized
    WHERE domain = 'REAL' AND sense*quadratic_coefficient > 0
    UNION ALL
    SELECT _decision_id,
           LEAST(upper_bound, GREATEST(
               lower_bound,
               FLOOR(-linear_coefficient / (2*quadratic_coefficient))
           ))
    FROM normalized
    WHERE domain = 'INT' AND sense*quadratic_coefficient > 0
    UNION ALL
    SELECT _decision_id,
           LEAST(upper_bound, GREATEST(
               lower_bound,
               CEIL(-linear_coefficient / (2*quadratic_coefficient))
           ))
    FROM normalized
    WHERE domain = 'INT' AND sense*quadratic_coefficient > 0
), ranked AS (
    SELECT c._decision_id, c.x,
           ROW_NUMBER() OVER (
               PARTITION BY c._decision_id
               ORDER BY n.sense * (
                            n.quadratic_coefficient*c.x*c.x
                            + n.linear_coefficient*c.x
                        ),
                        c.x
           ) AS rank
    FROM candidates c
    JOIN normalized n USING (_decision_id)
), chosen AS (
    SELECT _decision_id, x FROM ranked WHERE rank = 1
)
SELECT s.id, c.x
FROM source_rows s
JOIN chosen c USING (_decision_id);
```

The illustrative query uses `sense = 1` and `domain = 'REAL'`. A generated plan
obtains both fields from the bound DECIDE statement rather than from data;
`normalized_decisions` already contains domain-canonical legal endpoints.

### Direct algorithm

1. Collapse the objective for each decision to `a*x*x + b*x + constant` and set
   `sense` to `1` for minimization or `-1` for maximization.
2. Intersect all bounds. Evaluate both endpoints.
3. If `sense*a > 0`, also evaluate the clipped stationary point for REAL, or its
   clipped floor and ceiling for INT.
4. Rank candidates by `sense*(a*x*x+b*x)` and choose one per decision identity.

### Why it works

**Feasibility.** Every emitted candidate is an endpoint or is explicitly clipped to
the complete effective interval. For INT, floor and ceiling are integers inside the
effective integer interval.

**Optimality.** Multiplying by `sense` reduces both objective directions to
minimization. If `sense*a > 0`, the transformed quadratic decreases up to its stationary
point and increases after it; the clipped point is optimal for REAL, and one of its two
adjacent integers is optimal for INT. If `sense*a <= 0`, a linear or concave function
attains a minimum at an endpoint. The candidates are therefore exhaustive.

### Valid when

- The bound plan proves that, after constants and repeated references are combined,
  each component contains exactly one BOOL, INT, or REAL decision and an objective
  `a*x*x+b*x+constant`, with the query's MINIMIZE/MAXIMIZE direction retained.
- Every constraint on that decision reduces to an individual bound; directional RHS
  reduction, `WHEN`, and NULL-`PER` bypass semantics have already been applied, and no
  shared factor or cross term connects two decision identities.
- The complete effective interval is finite at both endpoints after strict comparisons
  are canonicalized. BOOL uses
  `[MAX(0,CEIL(lower)), MIN(1,FLOOR(upper))]`; INT uses
  `[CEIL(lower), FLOOR(upper)]`; REAL uses the closed real interval.
- A nonempty decision with `lower > upper` takes the DECIDE infeasible branch. Empty
  input emits no source rows or manufactured assignment. A pre-rewrite shape proof
  miss leaves the existing solver plan unchanged rather than reporting infeasibility.
- Bounds and coefficients are finite and non-NULL under DECIDE's solver-input rules;
  casts, integer range, stationary division, candidate evaluation, and comparison use
  DECIDE-compatible overflow and tolerance handling. A zero quadratic coefficient
  takes the endpoint branch.
- The plan keys candidates by stable internal decision identity, maps the chosen typed
  value to every row sharing a row/entity/scalar decision, and preserves source
  cardinality, schema, surrounding relational behavior, and primary-objective ties.

> **Not this class:** `POWER(x + y - 1, 2)` connects `x` and `y`. Optimizing each variable separately can be wrong.

## A2 — One shared mean, median, midpoint, or mode

**Status:** Exact translation

### Problem

Many rows contribute observations, but they share one scalar or entity decision. The
objective asks that shared decision to represent the observations under a recognized
loss.

### DECIQL

```sql
SELECT id, center
FROM observations
DECIDE scalar center(REAL)
SUCH THAT center BETWEEN lo AND hi
MINIMIZE SUM(weight * ABS(center - value));
```

### Direct SQL

```sql
WITH bounds AS (
    SELECT MAX(lo) AS lower_bound,
           MIN(hi) AS upper_bound
    FROM observations
), ordered AS (
    SELECT *,
           SUM(weight) OVER (ORDER BY value, id) AS cumulative_weight,
           SUM(weight) OVER () AS total_weight
    FROM observations
), answer AS (
    -- This is the weighted-L1 branch. An inconsistent interval is routed to
    -- DECIDE's infeasible outcome before this relation is used.
    SELECT CASE
             WHEN MAX(total_weight) = 0 THEN lower_bound
             ELSE LEAST(
                      upper_bound,
                      GREATEST(
                          lower_bound,
                          MIN(value) FILTER (
                              WHERE 2*cumulative_weight >= total_weight
                          )
                      )
                  )
           END AS center
    FROM ordered CROSS JOIN bounds
    WHERE lower_bound <= upper_bound
    GROUP BY lower_bound, upper_bound
)
SELECT o.id, a.center
FROM observations o CROSS JOIN answer a;
```

The displayed branch is weighted absolute loss. The same generated-plan interface
selects the formula in the table below for the other recognized losses.

### Direct algorithm

| Loss | Constrained construction |
|---|---|
| `SUM(w*(center-value)^2)` | Clamp `SUM(w*value)/SUM(w)`; use the lower bound when total weight is zero |
| `SUM(w*ABS(center-value))` | Clamp a weighted median; use the lower bound when total weight is zero |
| `MAX(ABS(center-value))` | Clamp `(MIN(value)+MAX(value))/2` |
| `norm(center-value, 0)` | Rank tolerance-safe target values by frequency, plus one representative from every tolerance-safe gap |

For L0, sort the distinct targets and the endpoints of their forbidden open regions
`0 < ABS(center-value) < decide_l0_tolerance`. Keep a target only when no other target
places it in such a region. Also emit one point from each nonempty remainder of the
legal interval. Rank all candidates by the exact L0 count; if none exists, report
infeasibility. Repeat the chosen shared value over the source rows.

### Why it works

**Feasibility.** Clamping places each convex-loss answer in the complete interval. The
L0 branch removes precisely the tolerance-ambiguous points, then tests every remaining
kind of candidate: an exact target or an open interval on which the count is constant.
An empty L0 candidate set is therefore genuinely infeasible.

**Optimality.** For positive total weight, the L2 derivative vanishes at the weighted
mean and the L1 subgradient crosses zero at a weighted median; projection of either
minimizer set onto an interval remains optimal. With zero total weight the objective is
constant. For L-infinity, every center has loss at least half the target range, attained
at the midpoint before interval projection. For L0, a feasible center equal to target
`v` has loss `n-frequency(v)`; everywhere else the loss is `n` and is constant between
successive forbidden-region boundaries. Ranking the finite candidate set is exhaustive.

### Valid when

- The bound plan proves one REAL scalar or entity decision per independent component;
  all source rows in that component reference that same internal identity, and no
  shared factor reconnects two components.
- After constants and repeated terms are combined, the objective is exactly one of:
  weighted squared distance, weighted absolute distance, unweighted maximum absolute
  distance, or unweighted `norm(center-value, 0)`, in the MINIMIZE direction. There is
  no additional term that selects a different center.
- L1/L2 weights are finite and nonnegative. Their zero-total-weight branch chooses a
  canonical legal bound without dividing. L-infinity has at least one observation in
  every instantiated component.
- All constraints on the shared decision reduce to one finite closed interval after
  directional RHS reduction and compatible `WHEN`/`PER` membership. A nonempty
  component with `lower > upper` reports DECIDE infeasibility.
- The L0 branch uses the session's exact `decide_l0_tolerance`, finite residual bounds,
  exact duplicate-target grouping, and a complete sorted forbidden-gap scan. It admits
  only candidates whose every residual is either exactly zero or at least the
  tolerance, and reports infeasibility when the legal interval is entirely ambiguous.
- Values, weights, bounds, and the active tolerance are non-NULL and finite under
  DECIDE's solver-input rules. Arithmetic, median ordering, boundary comparisons, and
  final residual checks use DECIDE-compatible numeric and tolerance handling.
- Empty input returns no source rows and manufactures no assignment. A pre-rewrite
  shape or identity proof miss leaves the existing solver plan unchanged.
- The chosen typed value is joined by stable internal scalar/entity identity, repeated
  on every corresponding source row, and preserves source cardinality, schema,
  surrounding relational behavior, and primary-objective ties.

> **Not this class:** Applying `AVG(target)` to independent row decisions is wrong; each row's optimum is its own target.

## A3 — Fixed-small multi-affine box

**Status:** Exact translation

### Problem

A fixed small number of bounded variables may multiply one another, but no variable is
squared and there are no constraints beyond the box.

### DECIQL

```sql
SELECT id, x, y
FROM cases
DECIDE x(REAL), y(REAL)
SUCH THAT x BETWEEN x_lo AND x_hi
      AND y BETWEEN y_lo AND y_hi
MAXIMIZE SUM(alpha*x*y + beta*x + gamma*y);
```

### Direct SQL

```sql
WITH corners AS (
    SELECT _component_id,
           CASE bx WHEN 0 THEN x_lo ELSE x_hi END AS x,
           CASE by WHEN 0 THEN y_lo ELSE y_hi END AS y,
           alpha, beta, gamma
    FROM normalized_components
    CROSS JOIN (VALUES (0), (1)) vx(bx)
    CROSS JOIN (VALUES (0), (1)) vy(by)
), chosen AS (
    SELECT _component_id, x, y
    FROM corners
    QUALIFY ROW_NUMBER() OVER (
        PARTITION BY _component_id
        ORDER BY alpha*x*y + beta*x + gamma*y DESC, x, y
    ) = 1
)
SELECT s.id, c.x, c.y
FROM source_rows s
JOIN chosen c USING (_component_id);
```

### Direct algorithm

Generate every lower/upper corner of the box, evaluate the objective at each corner,
and select the best one. A component with `k` decisions has at most `2^k` corners.

### Why it works

**Feasibility.** Every enumerated point chooses one legal endpoint for each coordinate,
so every candidate lies in the box. An empty effective interval is detected before
enumeration.

**Optimality.** Start from any feasible point and fix all coordinates except one. A
multi-affine objective is affine in that coordinate, so one endpoint is no worse in the
requested objective direction. Repeating for every coordinate reaches an enumerated
corner without worsening the objective. Hence at least one enumerated corner is global
optimal; ranking all corners finds one.

### Valid when

- The bound plan proves an independent component of a fixed, statically bounded number
  of BOOL, INT, or REAL decisions and retains the query's MINIMIZE/MAXIMIZE direction.
- After constants and repeated references are combined, every objective monomial is
  affine in each decision separately: each decision has exponent at most one in that
  monomial. Coefficients are decision-free, and no unsupported nonlinear expression
  remains.
- Every constraint in the component reduces to an individual bound after directional
  RHS reduction and compatible `WHEN`/`PER` handling; there is no coupled constraint.
- Every effective endpoint is finite after strict comparisons are canonicalized. BOOL
  uses `[MAX(0,CEIL(lower)), MIN(1,FLOOR(upper))]`; INT uses
  `[CEIL(lower), FLOOR(upper)]`; REAL uses its closed interval. Any empty interval in a
  nonempty component reports DECIDE infeasibility.
- Exact identity and factor provenance prove that components do not share a row,
  entity, or scalar decision. The resulting `2^k` candidates fit an explicit
  plan-size and execution-cost limit; otherwise the rewrite is not admitted and the
  existing solver plan remains.
- Bounds and objective data are non-NULL and finite under DECIDE's solver-input rules;
  candidate arithmetic and ordering have DECIDE-compatible overflow, precision, and
  tie handling, followed by validation of the chosen box point.
- Empty input emits no rows. Assignments are keyed by stable internal decision identity,
  cast to the declared result types, repeated over shared entity/scalar rows, and joined
  without changing source cardinality, schema, or surrounding relational behavior.

> **Not this class:** `POWER(x, 2)` may have an interior optimum. A bilinear constraint can also make the best box corner infeasible.

## A4 — Bounded two-parameter least squares

**Status:** Exact prototype

### Problem

Fit an intercept and slope under independent finite bounds.

### DECIQL

```sql
SELECT id, intercept, slope
FROM observations
DECIDE scalar intercept(REAL), scalar slope(REAL)
SUCH THAT intercept BETWEEN intercept_lo AND intercept_hi
      AND slope BETWEEN slope_lo AND slope_hi
MINIMIZE SUM(POWER(y - (intercept + slope*x), 2));
```

### Direct SQL

```sql
WITH normalized_observations AS (
    -- The displayed query is unweighted. A recognized weighted square supplies
    -- its nonnegative weight here instead of 1.0.
    SELECT *, 1.0 AS weight
    FROM observations
), stats AS (
    SELECT SUM(weight) AS sw,
           SUM(weight*x) AS sx, SUM(weight*y) AS sy,
           SUM(weight*x*x) AS sxx, SUM(weight*x*y) AS sxy,
           SUM(weight*y*y) AS syy
    FROM normalized_observations
), candidates(intercept, slope) AS (
    -- Generated plan: the legal normal-equation candidate when nonsingular.
    SELECT interior_intercept, interior_slope FROM interior_candidate
    UNION ALL
    -- The clamped one-dimensional optimum on each of the four box edges.
    SELECT edge_intercept, edge_slope FROM edge_candidates
    UNION ALL
    -- The four box corners.
    SELECT corner_intercept, corner_slope FROM corner_candidates
), ranked AS (
    SELECT *,
           ROW_NUMBER() OVER (
               ORDER BY
                   syy
                   - 2*intercept*sy
                   - 2*slope*sxy
                   + sw*intercept*intercept
                   + 2*intercept*slope*sx
                   + slope*slope*sxx,
                   intercept,
                   slope
           ) AS rank
    FROM candidates CROSS JOIN stats
), chosen AS (
    SELECT intercept, slope FROM ranked WHERE rank = 1
)
SELECT s.id, c.intercept, c.slope
FROM source_rows s
CROSS JOIN chosen c;
```

The named candidate relations above are constant-size expressions generated from the
six sufficient statistics. With `D = sw*sxx-sx*sx`, the interior point is
`((sy*sxx-sx*sxy)/D, (sw*sxy-sx*sy)/D)`. On an intercept edge `a=A`, the slope is
the clipped `(sxy-A*sx)/sxx`; on a slope edge `b=B`, the intercept is the clipped
`(sy-B*sx)/sw`. A zero denominator uses a canonical endpoint. These are ordinary SQL
expressions, not custom solver operators.

### Direct algorithm

1. Aggregate the weighted least-squares sufficient statistics.
2. If the normal-equation determinant is nonzero, add the interior stationary point
   when it lies in the rectangle.
3. Fix each parameter at each of its two bounds and add the clipped one-dimensional
   optimum on that edge.
4. Add all four corners, evaluate the original squared objective, and choose a minimum.

### Why it works

**Feasibility.** The interior point is emitted only when it is inside both bounds. Each
edge candidate fixes one legal endpoint and clips the other coordinate; corners are
legal by construction.

**Optimality.** Nonnegative weighted squared error is a convex differentiable quadratic
on a compact rectangle. An optimum in the rectangle's interior satisfies both normal
equations. An optimum in the relative interior of an edge satisfies that edge's
one-dimensional stationary equation. Every remaining optimum is a corner. The
candidate set covers all three cases. If the Hessian or an edge curvature is singular,
the objective is flat in the corresponding direction and a covered edge endpoint or
corner is equally optimal.

### Valid when

- The bound plan proves exactly two REAL identities in each independent component,
  representing intercept and slope, and all rows in the component reference those same
  identities.
- After constants and duplicate references are combined, the MINIMIZE objective is
  exactly `SUM(weight*(y-intercept-slope*x)^2)` plus a decision-free constant. Weights
  are finite and nonnegative, and there is no other decision-dependent objective term.
- The only constraints are the complete independent bounds of a finite closed rectangle
  after directional RHS reduction and compatible `WHEN`/`PER` handling; no factor or
  identity connects two proposed components.
- A nonempty component with an inconsistent rectangle reports DECIDE infeasibility.
  Empty input emits no source rows. Zero total weight takes a canonical legal corner;
  zero edge curvature takes its canonical endpoint.
- Inputs, weights, bounds, and sufficient statistics are non-NULL and finite under
  DECIDE's solver-input rules. Accumulation, determinant classification, division,
  candidate containment, objective comparison, and final residual validation follow a
  specified DECIDE-compatible numeric policy that is total for the admitted input
  class. Until that policy can certify the class before rewrite, this prototype is not
  admitted and the existing solver plan remains.
- The candidate relation is keyed by exact component and internal decision identities;
  typed intercept and slope assignments are repeated over scalar/entity rows and joined
  back without changing source cardinality, schema, ties, or surrounding relational
  behavior.

> **Not this class:** Independently clamping the unconstrained slope and intercept is not valid because the two parameters are correlated.

## A5 — Objective-aligned monotone corner

**Status:** Exact translation

### Problem

Every decision has a direction that improves the objective and cannot hurt any
constraint. The optimum is the corresponding corner of the decision box.

### DECIQL

```sql
SELECT id, x
FROM capacity
DECIDE x(INT)
SUCH THAT x <= upper_bound
      AND SUM(requirement*x) >= demand
MAXIMIZE SUM(value*x);
```

### Direct SQL

```sql
WITH corner AS (
    -- For the displayed query, monotonicity selects each effective upper endpoint.
    SELECT _decision_id, upper_endpoint::BIGINT AS x
    FROM normalized_monotone_decisions
)
SELECT s.id, c.x
FROM source_rows s
JOIN corner c USING (_decision_id);
```

The complete plan also evaluates the original constraints at this corner. If the
corner fails under the proven monotonicity conditions, no feasible assignment exists.

### Direct algorithm

Choose each decision's objective-improving lower or upper endpoint, emit that corner,
and validate the normalized constraints.

### Why it works

**Feasibility.** If any feasible point exists, move its coordinates one at a time toward
their certified endpoints. Every inequality residual moves only toward feasibility and
every equality is invariant, so the final corner is feasible. Consequently, if that
corner fails a validated constraint, no feasible starting point exists.

**Optimality.** The same moves weakly improve the objective in its requested direction.
Thus the emitted corner is at least as good as every feasible starting point and is
globally optimal.

### Valid when

- The bound plan retains the MINIMIZE/MAXIMIZE direction and proves, over the complete
  effective box, one direction for every BOOL, INT, or REAL decision in which the full
  objective is coordinatewise nonworsening.
- For every normalized inequality written as a feasibility residual `g(x) >= 0`, each
  selected movement makes `g` nondecreasing over the complete box. Every equality is
  invariant under all selected movements. These signs include all cross terms rather
  than only their displayed coefficients.
- After strict comparisons are canonicalized, each selected endpoint is finite and
  belongs to the complete bound intersection. BOOL first intersects the written bounds
  with `{0,1}`; INT uses the required ceiling/floor conversion. An empty discrete
  intersection is infeasible, and no decision identity receives conflicting directions
  through repeated references.
- Exact identity, `WHEN`, and `PER` provenance covers every factor, including NULL-`PER`
  bypass rows. Data-valued RHS reduction is complete, and no unexamined objective or
  constraint term remains.
- The generated plan evaluates every original normalized constraint at the corner. A
  failed constraint on a nonempty input takes the DECIDE infeasible branch; a
  pre-rewrite failure to prove monotonicity leaves the existing solver plan unchanged.
  Empty input emits no rows or assignment.
- Bounds, coefficients, and corner evaluations are non-NULL and finite under DECIDE's
  solver-input rules. Sign proofs, integer conversion, overflow checks, and final
  residual comparisons use DECIDE-compatible numeric and tolerance handling.
- Values are emitted under stable internal row/entity/scalar identities, with declared
  result types and correct repetition, and are joined back without changing source
  cardinality, schema, ties, or surrounding relational behavior.

> **Not this class:** Maximizing `x+y` with `x+y<=1` cannot set both variables to their upper bounds; the improving direction hurts the shared constraint.

## A6 — One squared affine residual over a box

**Status:** Exact translation

### Problem

Several continuous decisions occur only through one affine expression inside one
squared residual.

### DECIQL

```sql
SELECT id, x, y
FROM targets
DECIDE x(REAL), y(REAL)
SUCH THAT x BETWEEN x_lo AND x_hi
      AND y BETWEEN y_lo AND y_hi
MINIMIZE SUM(POWER(ax*x + ay*y - target, 2));
```

### Direct SQL

```sql
WITH contributions AS (
    SELECT residual_component, decision_key, decision_ordinal,
           coefficient, canonical_bound,
           LEAST(coefficient*lo, coefficient*hi) AS z_lo,
           GREATEST(coefficient*lo, coefficient*hi) AS z_hi
    FROM normalized_decisions
), component_params AS (
    -- Admission proves target is functionally dependent on residual_component;
    -- MIN/MAX carries that proved value; equality is an internal assertion.
    SELECT residual_component, MIN(effective_target) AS target
    FROM normalized_decisions
    GROUP BY residual_component
    HAVING MIN(effective_target) = MAX(effective_target)
), reachable AS (
    SELECT residual_component,
           SUM(z_lo) AS reachable_lo,
           SUM(z_hi) AS reachable_hi
    FROM contributions
    GROUP BY residual_component
), target_sum AS (
    SELECT r.residual_component,
           GREATEST(r.reachable_lo, LEAST(p.target, r.reachable_hi)) AS t,
           r.reachable_lo AS base
    FROM reachable r
    JOIN component_params p USING (residual_component)
), prefixes AS (
    SELECT *,
           COALESCE(SUM(z_hi-z_lo) OVER (
               PARTITION BY residual_component
               ORDER BY decision_ordinal
               ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
           ), 0) AS prior_width
    FROM contributions
)
SELECT p.residual_component, p.decision_key,
       CASE
         WHEN p.coefficient = 0 THEN p.canonical_bound
         ELSE (
             p.z_lo + LEAST(
                 p.z_hi-p.z_lo,
                 GREATEST(0, s.t-s.base-p.prior_width)
             )
         ) / p.coefficient
       END AS value
FROM prefixes p
JOIN target_sum s USING (residual_component);
```

### Direct algorithm

1. Collapse repeated references, assign each decision to its exact residual component,
   and reduce that component to `weight*(SUM(a_i*x_i)-target)^2+constant`.
2. Convert each decision interval into its contribution interval and sum those
   intervals per component.
3. Project the component's one effective target onto its reachable interval.
4. Partition by component and prefix-fill contribution widths until the projected
   target is reached; map each contribution back to its decision value.

### Why it works

**Feasibility.** For each decision, prefix filling chooses a contribution inside
`[MIN(a_i*l_i,a_i*u_i), MAX(a_i*l_i,a_i*u_i)]`; division by nonzero `a_i` therefore
returns a legal decision value, while a zero coefficient uses a legal canonical bound.
The required fill lies between zero and total width, so the constructed contributions
sum exactly to the projected scalar. Partitioning prevents one component's width or
target from leaking into another.

**Optimality.** A continuous box mapped by one affine expression has the complete
interval obtained by summing its contribution intervals. Over that interval,
`weight*(z-target)^2` with positive weight is minimized by projecting the target.
The construction reaches that projection, so it is globally optimal. Target
invariance follows from normalization: one effective residual is created per component
and that target is functionally dependent on the component key; the SQL MIN/MAX check
only verifies this invariant.

### Valid when

- The bound plan proves independent residual components of REAL decisions. After
  constants and duplicate references are combined, each component's requested
  objective direction is exactly equivalent to minimizing
  `weight*(SUM(a_i*x_i)-target)^2+constant` with finite `weight > 0`.
- Multiple written squares are admitted only when exact algebra reduces them to that
  single effective square; the resulting target is finite and functionally dependent
  on the exact component key. The same target is repeated on every normalized
  coefficient row and is checked with `MIN(target)=MAX(target)`.
- Every constraint is an individual finite bound after directional RHS reduction and
  compatible `WHEN`/`PER` handling. No shared factor connects components, and no extra
  objective term distinguishes assignments with the same affine sum.
- Each nonempty component has a nonempty closed box. An inconsistent bound reports
  DECIDE infeasibility; zero coefficients choose a declared canonical legal bound.
  Empty input emits no source rows or manufactured component.
- Exact row/entity/scalar identity and a stable per-component decision ordinal are
  available from the bound plan. Repeated entity references are coefficient-collapsed,
  and NULL-`PER` bypass rows are not incorrectly placed in a SQL NULL partition.
- Coefficients, targets, and bounds are non-NULL and finite under DECIDE's solver-input
  rules. Interval products and sums, projection, prefix filling, division, and the final
  affine residual are either exact and representable or covered by a proved error bound
  that certifies DECIDE's overflow and tolerance checks; otherwise the rule is not
  admitted.
- The typed assignment is joined by internal decision identity and repeated over every
  corresponding source row without changing cardinality, schema, ties, or surrounding
  relational behavior.

> **Not this class:** Adding `-3*x` to `POWER(x+2*y-6,2)` selects among points on the residual valley, so an arbitrary prefix fill may be suboptimal.

## N1 — L1, L2, and Linf balls

**Status:** Exact translation

### Problem

A REAL vector is constrained by one zero-centered norm ball. The objective either
projects a target vector onto the ball or maximizes a linear score over it.

### DECIQL

```sql
SELECT id, x
FROM directions
DECIDE x(REAL)
SUCH THAT norm(x, 2) <= 25
MAXIMIZE SUM(coefficient*x);
```

In DeciDB, `norm(x,2)` is the sum of squares, so the Euclidean radius above is `5`.

### Direct SQL

```sql
WITH q AS (
    -- Nonnegative L2-support branch for the displayed MAXIMIZE query.
    SELECT *, GREATEST(coefficient, 0.0) AS direction
    FROM directions
), scale AS (
    SELECT SUM(direction*direction) AS h
    FROM q
)
SELECT id,
       CASE
         WHEN h = 0 THEN 0.0
         ELSE SQRT(25.0/h)*direction
       END AS x
FROM q CROSS JOIN scale;
```

### Direct algorithm

Let `B` be the DECIDE constraint RHS. Its geometric radius is `rho=B` for L1
and Linf, and `rho=SQRT(B)` for DeciDB's squared L2 norm.

Projection minimizes `SUM((x_i-target_i)^2)`:

| Ball | Signed domain | Nonnegative domain |
|---|---|---|
| Linf | `CLAMP(target_i, -rho, rho)` | `CLAMP(target_i, 0, rho)` |
| L2 | Set `y=target`; keep `y` if `SUM(y*y)<=B`, else scale it by `rho/NORM2(y)` | Use the same rule with `y_i=MAX(target_i,0)` |
| L1 | `SIGN(target_i)*MAX(ABS(target_i)-theta,0)` | `MAX(target_i-theta,0)` |

For L1, sort magnitudes (signed) or positive targets (nonnegative) and choose the unique
threshold interval whose `theta >= 0` makes the resulting L1 sum at most `rho`, with
equality when the preprojected target is outside the ball.

Linear support first converts the query to `MAXIMIZE SUM(direction_i*x_i)`, using the
original coefficient for MAXIMIZE and its negation for MINIMIZE:

| Ball | Signed domain | Nonnegative domain |
|---|---|---|
| Linf | `rho*SIGN(direction_i)`; use zero when the coefficient is zero | `rho` where `direction_i>0`, otherwise zero |
| L2 | `rho*direction/NORM2(direction)` | Apply the signed formula to `MAX(direction,0)` |
| L1 | Put all `rho` on a stable index maximizing `ABS(direction_i)`, with its sign | Put all `rho` on a stable index maximizing positive `direction_i` |

A zero support direction returns the zero vector. The L1 nonnegative branch also
returns zero when no coefficient is positive.

### Why it works

**Feasibility.** Clamping enforces the Linf ball coordinate by coordinate. L2 scaling
sets the squared norm to `B` only when necessary. L1 thresholding sets the L1 norm to
at most `rho`; the support construction places at most `rho` mass. Positive-part
variants also satisfy DeciDB's nonnegative domain.

**Optimality.** The projection formulas satisfy the KKT stationarity and complementary
slackness equations: coordinate clipping is separable for Linf, L2 has one radial
multiplier, and L1 has one soft-threshold multiplier. For support, Hölder bounds L1 by
`rho*MAX(ABS(direction))`, Linf by `rho*SUM(ABS(direction))`, and Cauchy--Schwarz bounds
L2 by `rho*NORM2(direction)`; the signed constructions attain those bounds. In the
nonnegative domain, negative coefficients can only hurt, so replacing the direction by
its positive part gives the same attainable upper bound.

### Valid when

- The bound plan proves independent components of REAL decisions, each with exactly one
  zero-centered upper norm constraint: `norm(x,1)<=B`, `norm(x,2)<=B`, or
  `norm(x,'inf')<=B`. Factor membership, `WHEN`, and `PER` keys are exact; no decision
  identity belongs to two proposed balls.
- After identity collapse, the norm contains each decision exactly once with unit
  coefficient and multiplicity; a repeated entity or unequal norm weight belongs to a
  weighted rule such as N2, not this rule.
- The complete objective is either MINIMIZE uniform squared Euclidean distance to a
  finite target vector, or a finite linear MINIMIZE/MAXIMIZE objective normalized to a
  support direction. No other decision-dependent term or coupling constraint remains.
- The domain is proved to be either the default nonnegative orthant or the signed
  variant used by the selected formula. Every other coordinate bound is redundant for
  the whole relevant ball, or the exact constructed point is proved to satisfy it;
  otherwise the rule does not apply.
- `B` is a finite, component-invariant scalar after directional RHS reduction. `B<0`
  on a nonempty vector reports DECIDE infeasibility; `B=0`, a zero support direction,
  and an already feasible projection target take their explicit zero/no-scaling
  branches without division.
- Empty input emits no source rows or manufactured vector. NULL or nonfinite targets,
  coefficients, budgets, or bounds follow DECIDE's solver-input outcome rather than
  being silently ignored by SQL aggregates. A pre-rewrite proof miss leaves the
  existing solver plan unchanged.
- L1 sorting uses a stable internal decision ordinal and a certified threshold,
  including tied magnitudes. Admission requires L2 accumulation, square root, scaling,
  and all final domain/norm residuals to be certified by a total DECIDE-compatible
  overflow and tolerance policy; otherwise the rewrite is not chosen.
- Assignments are keyed by stable internal row/entity/scalar identities, cast to REAL,
  repeated over every corresponding source row, and joined without changing source
  cardinality, schema, primary-objective ties, or surrounding relational behavior.

> **Not this class:** Intersecting an L1 or L2 ball with an active coordinate box changes the threshold. INT decisions also cannot use fractional scaling.

## N2 — Linear support over a diagonal ellipsoid

**Status:** Exact translation

### Problem

Maximize a linear score over a positive diagonal ellipsoid.

### DECIQL

```sql
SELECT id, x
FROM directions
DECIDE x(REAL)
SUCH THAT SUM(POWER(SQRT(weight)*x, 2)) <= 25
MAXIMIZE SUM(coefficient*x);
```

### Direct SQL

```sql
WITH normalized AS (
    -- Nonnegative branch for the displayed MAXIMIZE query. A signed branch
    -- uses coefficient directly; MINIMIZE negates it first.
    SELECT *, GREATEST(coefficient, 0.0) AS direction
    FROM directions
), normalizer AS (
    SELECT SUM(direction*direction/weight) AS h
    FROM normalized
)
SELECT id,
       CASE
         WHEN h = 0 THEN 0.0
         ELSE SQRT(25.0/h)*direction/weight
       END AS x
FROM normalized CROSS JOIN normalizer;
```

### Direct algorithm

Convert the query to `MAXIMIZE SUM(direction_i*x_i)`. Use
`u_i=direction_i` for the signed domain and `u_i=MAX(direction_i,0)` for the default
nonnegative domain. Compute `h=SUM(u_i*u_i/weight_i)`. Return zero if `h=0`; otherwise
return `x_i=SQRT(B/h)*u_i/weight_i`, where `B` is the ellipsoid RHS.

### Why it works

**Feasibility.** For `h>0`, substitution gives
`SUM(weight_i*x_i*x_i)=(B/h)*SUM(u_i*u_i/weight_i)=B`; for `h=0`, zero is feasible.
The positive-part branch also satisfies the nonnegative domain.

**Optimality.** Substituting `z_i=SQRT(weight_i)*x_i` turns the ellipsoid into a
Euclidean ball. Cauchy--Schwarz bounds signed support by
`SQRT(B)*SQRT(SUM(direction_i^2/weight_i))`, and the formula attains equality. In the
nonnegative domain, negative direction coefficients cannot improve the objective, so
the same proof applied to the positive part gives the exact optimum.

### Valid when

- The bound plan proves independent components of REAL decisions. After repeated
  references are coefficient-collapsed, each has exactly one finite linear
  MINIMIZE/MAXIMIZE objective and one constraint
  `SUM(weight_i*x_i*x_i)<=B`, with no cross-quadratic term.
- Every effective diagonal weight is finite and strictly positive. No other
  decision-dependent objective term or nonredundant coupling constraint remains.
- The decision domain is proved to be either the default nonnegative orthant or the
  signed variant used by the formula. Every extra coordinate bound is redundant for
  the relevant ellipsoid, or the exact constructed point is proved to satisfy it.
- `B` is finite and component-invariant after directional RHS reduction and compatible
  `WHEN`/`PER` handling. `B<0` for a nonempty component reports DECIDE infeasibility;
  `B=0` and `h=0` return zero without division.
- Exact factor and row/entity/scalar identity prove that components do not overlap and
  that all coefficient/weight multiplicities from joins are retained. NULL-`PER`
  bypass rows are not collapsed into a SQL NULL partition.
- Empty input emits no source rows. NULL or nonfinite coefficients, weights, budgets,
  or bounds preserve DECIDE's solver-input outcome. A pre-rewrite proof miss leaves the
  existing solver plan unchanged rather than producing an assignment.
- Accumulation of `h`, division, square root, scaling, and the final ellipsoid residual
  use a total DECIDE-compatible overflow and tolerance policy. The rewrite is admitted
  only when that policy certifies the whole accepted numeric class.
- REAL assignments are keyed by stable internal decision identity, repeated over every
  corresponding source row, and joined without changing cardinality, schema,
  primary-objective ties, or surrounding relational behavior.

> **Not this class:** Projection onto an unequal-weight ellipsoid is not radial. Cross terms or an active box also invalidate the normalization.
