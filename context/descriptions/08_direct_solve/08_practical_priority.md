# Practical Priority for Direct Solve

**Status:** Research recommendation, not an implementation commitment

**Scope:** The 19 classes in this directory

**Research snapshot:** 2026-09-18

**Decision question:** Which classes are likely enough to occur in real DECIDE queries, and valuable enough as relational rewrites, to justify their optimizer, semantic, testing, and maintenance cost?

## Recommendation

Do not implement the catalogue as a 19-rule feature. The evidence supports a much
smaller initial portfolio:

1. Build only the narrow common infrastructure needed to prove keyed independence,
   construct complete typed assignments, preserve DECIDE outcomes, and explain why a
   rule matched.
2. Prioritize **S1**, **A1**, and **R1** as the first vertical-slice prototypes, not yet
   as proven production features.
3. Run workload pilots for **R3**, **S2**, **A4**, and grouped signed-L2 projection
   from **N1** before committing their recognizers to production.
4. Build only narrow homogeneous-key support from **D1**. Defer **A5** as a leaf and
   retain monotonicity only as a possible future presolve technique.
5. Defer **S3** and the other lower-evidence classes until an exact workload appears.
   In particular, remove **A6**, **S4**, **O1**, and **O2** from the near-term roadmap.

The resulting prototype tranche is deliberately small:

| Role | Classes | Why |
|---|---|---|
| First vertical-slice candidates | **S1, A1, R1** | Credible formulations, favorable relational plans, and material solver-path work to avoid; observed DECIDE demand is still required |
| Narrow foundation | **D1** | Required to apply a leaf safely per exact key; avoid a generic mixed-rule component engine initially |
| Evidence-gathering pilots | **R3, S2, A4, N1 signed-L2 projection** | Credible domains or near matches exist, but product fit, realistic eligibility, or numeric behavior is not yet established |
| Deferred presolve hypothesis | **A5** | Monotonicity may fix decisions and expose other classes, but evidence for either a standalone leaf or a worthwhile sign-analysis pass is weak |

This differs from a ranking based only on mathematical elegance or current synthetic
benchmarks. In particular:

- **S4** has an attractive Q9 rewrite and large HiGHS overhead, but no convincing
  real objective matching its anchor-plus-cheapest-fillers semantics.
- **O1/O2** solve important mathematical problems, but their current DECIDE contract
  requires a complete pair relation and therefore throws away the sparse output that
  makes one-dimensional matching and transport attractive.
- **A2** contains ubiquitous statistics, but most users would write `AVG`, `MEDIAN`,
  or a quantile directly. Recognizing an optimization formulation of those aggregates
  is not automatically worth a new optimizer rule.

## How the classes were judged

A class was not credited merely because an algorithm or application shares its name.
The useful unit of evidence is a realistic formulation whose decision domain,
objective, constraints, and required output all preserve the proposed class.

Five questions were applied to every rule:

1. **Natural demand.** Is there a concrete workload in which a user might naturally
   submit this optimization structure, rather than a theorem that can be made to fit?
2. **Structural survival.** Do routine requirements preserve the class, or do they add
   a second resource, lower quota, temporal coupling, secondary objective, or missing
   pair and immediately send the query back to the solver?
3. **Value over ordinary SQL.** Does DECIDE make the problem meaningfully easier to
   state, or is the direct answer already the obvious SQL expression?
4. **Relational economics.** Is the assignment plan compact and natural for DuckDB?
   A polynomial algorithm is not useful if it expands `2^k`, every integer unit, or an
   `n*m` pair relation.
5. **Delivery cost.** Can DeciDB prove the class and preserve complete assignment,
   status, NULL, identity, tolerance, and tie semantics without disproportionate
   recognizer and numeric complexity?

Formulation evidence is described as one of three strengths:

- **Exact:** the cited formulation has an exact subcase matching the class.
- **Near match:** the domain is real, but normal requirements often break the class.
- **Adjacent:** the formula is important elsewhere, but there is no clear
  SQL-facing DECIDE workload.

Product fit is separate: **plausible** means the evidence justifies a prototype,
**unknown** means a DeciDB workload has not been established, and **poor** means the
current class or output contract conflicts with how the application is normally used.
No class yet has **demonstrated** product fit from an observed user DECIDE-query corpus.
The confidence column is confidence in the recommended disposition, not a rating of a
paper or source.

## Portfolio decision

| ID | Formulation match | Product fit | Best real-world evidence | What usually breaks the class | Disposition | Confidence |
|---|---|---|---|---|---|---|
| **A1** | Exact primitive | Plausible | Independent range repair, clipping, and nearest legal values | Budgets, process equalities, cross terms, temporal rules | **Prototype**; low-complexity first wave | High |
| **A2** | Exact common branches | Unknown | Mean and median location estimation | Multidimensional location, capacity, richer losses; native SQL already solves several branches | **Defer / opportunistic** | High |
| **A3** | Exact niche | Unknown | Vertex evaluation of small multi-affine uncertainty boxes | Correlated uncertainty, physical constraints, larger dimension | **Defer until an actual query appears** | High |
| **A4** | Near match | Plausible | Per-device gain/offset calibration by bounded least squares | Extra covariates, nonlinear drift, no meaningful active coefficient bounds | **Pilot** | Medium |
| **A5** | Near match | Unknown | Monotone worst-case/tolerance corners | Sign changes and black-box response models make proof difficult | **Defer; possible future presolve** | High |
| **A6** | Near match | Poor | One-period aggregate target tracking | Cost, fairness, comfort, dynamics, or any secondary allocation criterion | **Drop from near-term roadmap** | High |
| **N1** | Exact signed-L2 subrule | Plausible | Gradient clipping and norm-ball projection | Tensor-system ownership, weighted norms, active boxes, overlapping groups | **Pilot signed L2 projection only** | Medium-high |
| **N2** | Adjacent | Unknown | Robust-optimization and trust-region inner support problems | Full covariance, active bounds, projection rather than support | **Defer** | Medium-high |
| **S1** | Exact fixed-score final stage | Plausible | Per-user/per-group top-k, deduplication, latest/best row selection | Diversity, exposure, inventory, promotions, or shared budgets | **Best first architecture-validation leaf** | High |
| **S2** | Near match | Plausible | Country-within-region and other hierarchical upper quotas | Lower/reserved quotas, overlapping attributes, matching, weighted membership | **Pilot** | Medium |
| **S3** | Adjacent; no verified exact workload | Unknown | Equal-priority batch admission under one budget | Different values, deadlines, dependencies, or a second resource | **Defer** | Medium |
| **S4** | None found | Poor | No credible exact workload found | Real lineups and portfolios value the whole set and add roles/dependencies | **Drop from near-term roadmap** | High |
| **R1** | Exact core subcase | Plausible | Fixed-demand single-zone dispatch and divisible budget allocation | Network limits, multiple resources, nonconcave response, time coupling | **Prototype narrowly** | Medium-high |
| **R2** | Exact subcases | Unknown | Integer sample allocation, apportionment, identical-server allocation | Large integer widths, heterogeneous units, skills, schedules, extra constraints | **Defer** | High |
| **R3** | Exact subcases | Plausible | Capped-simplex projection and fixed-demand quadratic dispatch | Multiple resources, ramping, reserves, cross terms; numeric certification | **Pilot; likely next wave if it passes** | Medium-high |
| **R4** | Near match | Poor | Proportional coverage/rationing | Real max-min policies progressively redistribute after a recipient caps | **Defer; investigate a distinct progressive class** | High |
| **O1** | Near match | Poor | One-dimensional scalar-distance pair matching | Calipers, missing pairs, unequal sides; complete Boolean pair output | **Drop from near-term roadmap** | High |
| **O2** | Exact mathematical problem | Poor | One-dimensional Wasserstein transport | Sparse/objective-only output is wanted; physical transport has networks and missing arcs | **Drop from near-term roadmap** | High |
| **D1** | Exact composite pattern | Plausible infrastructure | Independent per-user, tenant, order, or scenario problems | Shared inventory, scalar decisions, budgets, or nonseparable objectives | **Build narrowly as shared infrastructure** | High |

## First-wave prototype candidates

### S1 — top-k and cardinality intervals

S1 is the best architecture-validation rule because top-k has strong scale evidence,
a natural relational plan, and material current solver overhead. The sources below
establish demand for the primitive, not yet demand for expressing it through DECIDE.

A production-shaped example is the final selection stage of a recommender:

```sql
SELECT user_id, item_id, chosen
FROM candidate_scores
DECIDE chosen(BOOL)
SUCH THAT SUM(chosen) = 25 PER user_id
MAXIMIZE SUM(score * chosen);
```

This is exactly S1 plus homogeneous keyed D1 after candidate scores are fixed and no
cross-item or cross-user rule remains.
Google describes a production YouTube [top-k recommender operating over millions of
items and billions of users](https://research.google/pubs/top-k-off-policy-correction-for-a-reinforce-recommender-system/).
[Amazon Personalize](https://docs.aws.amazon.com/personalize/latest/dg/API_RS_GetRecommendations.html)
returns a requested number of results ordered by prediction score, including a
[batch per-user workflow](https://docs.aws.amazon.com/personalize/latest/dg/batch-data-upload.html).
Outside recommendation, DuckDB itself presents top-N and top-N-per-group as common
analysis patterns for recent events, outliers, high-price orders, and deduplication,
and documents dedicated relational implementations in
[Fast Top N Aggregation and Filtering](https://duckdb.org/2024/10/25/topn.html).

The qualification is equally important. Production recommenders often add category
diversity, provider exposure, inventory, promotions, filters, or multiple ranking
objectives. Those constraints are not evidence for S1; they are explicit rejection
cases. S1 is valuable as the clean final stage and as a leaf exposed after other
decisions are fixed, not as a claim that recommendation systems are generally top-k.

The existing P4 benchmark has one million Boolean decisions across 249,987 groups.
Its tracked median query times are `0.548 s` with Gurobi and `82.499 s` with HiGHS. A
local partitioned-`ROW_NUMBER` probe materialized all one million assignments and
matched the recorded objective. Its runs were not retained as a repository artifact,
so this is a feasibility observation only. It suggests that output cardinality by
itself need not dominate the kernel; the end-to-end advantage remains unproved.

**Recommendation:** make S1 plus homogeneous keyed D1 the first end-to-end proof of
the architecture. Support
one exact global or keyed count interval, fixed additive scores, complete assignment
mapping, and hard rejection of any weighted budget or crossing constraint. Let cost
selection choose DuckDB's best top-N or window implementation after correctness is
proved.

### A1 — independent bounded decisions

A1 is the lowest-risk useful leaf. It covers independent linear or quadratic choices,
including projection to a legal interval:

```sql
SELECT id,
       LEAST(hi, GREATEST(lo, target)) AS x
FROM items;
```

Elementwise clipping is a standard operation in data and machine-learning systems;
for example, TensorFlow exposes
[`clip_by_value`](https://www.tensorflow.org/api_docs/python/tf/clip_by_value).
Bounded numerical data repair provides a database-adjacent motivation. Bertossi et al.
study [least-squares numerical database repairs](https://arxiv.org/abs/cs/0503032),
including measurement values and upper limits. Only the independent range-repair
subcase is A1: process balance equations and aggregate consistency constraints reconnect
the variables and must stay with the solver.

A1 is not strategically differentiating on its own. A user who already knows the
answer will normally write `LEAST/GREATEST` directly. Its value is that generated or
uniformly expressed DECIDE workloads do not pay solver construction and loading for a
problem whose solution is a scalar formula.

The current solver-path opportunity is still compelling. At one million decisions,
the tracked P1
medians are `0.626 s`/`0.806 s` for Gurobi/HiGHS, while P2 is `0.243 s` with Gurobi and
hits a `300.275 s` HiGHS solver limit. The limit is an incomplete solver outcome, and
the projection still needs a controlled end-to-end measurement. The removable
solver-path work and the simplicity of the relational formula together justify a
small prototype, not a claimed speedup.

**Recommendation:** prototype the interval projection and endpoint/stationary-point
branches that fall naturally out of normalized coefficients. Do not broaden A1 into a
general symbolic algebra project. Its implementation is justified only if it reuses
the identity, status, and assignment foundation required by S1 and R1.

### R1 — continuous one-resource allocation

R1 is the most distinctive first-wave capability: unlike A1 and S1, its useful answer
is not usually a single obvious SQL aggregate or ranking expression.

The strongest exact formulation match is the core of single-zone economic dispatch. A
generator relation contains lower/upper output and linear or convex piecewise-linear
cost; total generation must meet fixed demand. The U.S. Department of Energy describes
the [basic bounded economic-dispatch model](https://www.osti.gov/servlets/purl/1862342),
and MATPOWER documents [convex piecewise-linear generator costs derived from bids and
offers](https://matpower.org/docs/MATPOWER-manual-7.1.pdf). MATPOWER's full optimal
power flow is not R1 because it adds network constraints; only its fixed-demand,
single-zone cost subproblem is. PyPSA's
[single-zone market-clearing example](https://docs.pypsa.org/latest/examples/demand-supply-bids/)
is adjacent merit-order evidence, but it optimizes elastic demand as well as supply and
therefore does not match current R1 without a proved transformation.

Dispatch is operationally frequent—
[security-constrained dispatch](https://www.ferc.gov/sites/default/files/2020-05/final-cong-rpt.pdf)
can run every few minutes—but energy-management and market software normally owns it.
Frequency supports the value of a fast kernel; it does not establish that the
fixed-demand subproblem will arrive as a DECIDE query.

The boundary is sharp. Security-constrained dispatch adds transmission and reliability
constraints; multi-period dispatch adds ramping and storage; unit commitment adds
Boolean decisions. Media-budget allocation is another credible domain, but deployed
tools use nonlinear response curves, discrete grids, ROI constraints, and time-varying
plans. They become R1 only when the submitted curve is explicitly finite,
piecewise-linear, separable, and concave under one shared budget.

R1's plan—expand explicit marginal segments, order by value per resource, scan a
prefix, and partially fill one segment—is a natural DuckDB workload. The tracked Q11
case has 136K rows and median times of `0.235 s` with Gurobi and `0.936 s` with HiGHS,
but it is only **shape-level** R1 evidence. The generated TPC-H schema proves
`ps_supplycost` non-NULL, not strictly positive, and the query has no proving filter or
catalog constraint. Observed positive values cannot satisfy R1's exact-fact admission
contract. A validation variant must add a provable positivity premise. An exploratory
local probe found the sort-and-prefix kernel plausible, but no retained harness
supports a durable direct-path timing claim yet.

**Recommendation:** prototype the one-segment fractional-allocation case and
objectives whose finite segments are already explicit in the bound expression. Do not
commit initially to arbitrary expression-to-segment extraction. Validate with real
single-zone offer curves and reject a second resource or nonconcave marginal sequence.

## Workload pilots before implementation

### R3 — strictly convex quadratic allocation

R3 has strong exact application subcases and excellent algorithmic economics, but it carries
the largest numeric proof burden among plausible near-term rules.

The evidence comes from two sources at different levels:

- The capped-simplex problem is an exact R3 mathematical primitive: it minimizes
  squared distance subject to coordinate bounds and one sum equality. Ang et al. apply
  it to sparse regression on a
  [1.5-million-SNP genome-wide association dataset](https://arxiv.org/abs/2110.08471)
  and report benefits at million-variable scale. It establishes an important
  algorithmic workload, not yet a natural SQL-facing DECIDE query.
- The fixed-demand generator subcase of classical single-area economic dispatch uses
  positive quadratic generator costs, generator boxes, and one energy-balance
  equality. A PNNL report contains this
  [quadratic economic-dispatch formulation](https://www.osti.gov/servlets/purl/1812546),
  but its broader model also includes flexible load and can permit zero quadratic
  coefficients. Only the strictly positive, already-committed generator subcase is R3.

Real grid models commonly add transmission constraints, ramping, reserves, storage,
and commitment decisions, so the exact match is a copper-plate snapshot, isolated
system, planning subproblem, or already-committed dispatch—not general SCED.

The current P3 benchmark takes `1.004 s` with Gurobi and reaches a `300.329 s` HiGHS
solver limit. An exploratory event-scan probe was encouraging, but it has no retained
benchmark artifact and the dataset has only 80 distinct breakpoint values. It did not
implement all numeric admission and residual certificates. Sorting two events per
decision is promising; coincident and nearly coincident breakpoints, resource
residuals, overflow, and KKT certification are the real implementation risk.

**Pilot gate:** use real capped-simplex and single-area dispatch data, deliberately
include ill-conditioned and nearly tied breakpoints, and compare the complete direct
plan with both solvers. Promote R3 only if the optimizer can certify the result without
post-hoc solver fallback and the benefit survives full mapping and status handling.

### S2 — nested upper quotas

Hierarchical quotas are real. Arnosti's
[Algorithms for Affirmative Action](https://pubsonline.informs.org/doi/10.1287/ited.2023.0039)
uses examples including the U.S. Diversity Visa lottery, with country quotas inside
regional quotas, and explains the greedy structure of nested upper quotas. The scale is
not hypothetical: the U.S. State Department reported
[19,927,656 qualified DV-2025 entries](https://travel.state.gov/content/travel/en/us-visas/immigrate/diversity-visa-program-entry/dv-2025-selected-entrants.html).
That figure demonstrates batch size, not execution frequency or DeciDB ownership: the
lottery is annual and is operated by a specialized government process.

That does not establish the full operational process as an exact S2 query. One entrant
can bring derivatives, more people are selected than ultimately receive visas, and
eligibility is resolved later. Other affirmative-action and admissions policies add
minimum or reserved quotas, overlapping gender/geography attributes, matching, and
stability requirements. Those are precisely the nearby cases S2 cannot solve.

Execution can reuse S1 ranking one hierarchy level at a time, but admission is harder:
DeciDB must prove laminarity from keys, functional dependencies, and masks. Observing
that current data happens to be nested is not enough.

**Pilot gate:** encode one fully specified operational upper-only quota policy, then add
negative cases for lower quotas, crossing attributes, weighted membership, and shared
constraints. Promote S2 only if exact upper-only laminar models recur, rather than
being an artificial simplification of the policies users actually need.

### A4 — bounded two-parameter least squares

Gain-and-offset calibration is a genuine two-parameter least-squares workload. NIST
documents the linear calibration model and use of
[slope and intercept](https://itl.nist.gov/div898/handbook/mpc/section3/mpc366.htm),
and its sensor handbook recommends a least-squares straight line for calibration. A
fleet of sensors creates many independent keyed regressions, which makes aggregate
sufficient statistics and a constant candidate set attractive.

Calibration may recur per device, deployment, or maintenance interval, but the sources
do not show that it is executed as a database optimization query; metrology and sensor
management software normally owns the fit. The pilot must establish both active bounds
and relational ownership rather than infer them from fleet size.

The evidence is weaker for the part that distinguishes A4 from native regression:
finite coefficient bounds that are meaningful and sometimes active. Practical
calibration also adds temperature, humidity, nonlinear response, drift, errors in both
axes, or coupled reference-free calibration. Unconstrained straight-line fits are
already available as database regression aggregates.

**Pilot gate:** find a real multi-device dataset with physically justified gain and
offset bounds, including groups with active bounds and singular or nearly singular
regressors. Measure against both the solver path and native regression aggregates. Do
not implement the rule on unconstrained-calibration evidence alone.

### N1 — norm-ball support and projection

N1 is an important primitive, especially in machine learning:

- DP-SGD performs per-example [L2 gradient clipping](https://arxiv.org/abs/1607.00133)
  on every training step.
- Duchi et al. developed [L1-ball projection for high-dimensional learning](https://doi.org/10.1145/1390156.1390191),
  including sparse text settings.
- The fast-gradient-sign method derives a linear adversarial step from
  [support over an L-infinity ball](https://arxiv.org/abs/1412.6572).

These are exact or very close mathematical matches and can operate at enormous scale.
The unresolved issue is product fit: they normally run inside vectorized tensor
systems, not as relational DECIDE queries. Weighted norms, layer-wise overlapping
groups, active coordinate boxes, and nonlinear objectives also break the present rule.

**Pilot gate:** start only with grouped **signed L2 projection** onto a uniform-radius
ball over tall relational vectors—the exact subrule supported by the DP-SGD evidence.
The query must carry the explicit symmetric coordinate bounds needed for DeciDB to
prove the signed domain; do not silently reinterpret the default nonnegative domain.
Compare the relational formula with the current solver path and with a native
vector/tensor baseline. DP-SGD runs this operation every training step, but tensor
runtimes normally own it; promote N1 only if an in-database workflow is credible. Do
not infer demand for L2 support, nonnegative projection, or the L1/L-infinity variants
from the popularity of clipping.

## Foundation, defer, and drop decisions

### D1 — narrow foundation, not a generic feature

Independent keyed problems are common. Batch recommendation naturally separates by
user; other plausible keys include tenant, order, department, store, scenario, or time
bucket. S1 per user and R1 per independently budgeted account should not require one
monolithic solver model when the bound query proves that the components are disjoint.

But there are two materially different proposals hidden inside D1:

1. **Homogeneous keyed execution:** prove that all factors use the same exact key and
   apply one leaf rule per group.
2. **Generic heterogeneous decomposition:** build a full interaction graph, dispatch
   different leaf rules per component, merge infeasible/unbounded outcomes, and
   assemble mixed typed assignments.

Only the first is currently justified. Real retail and allocation models often add
shared inventory, a total budget, substitution, or limits across stores. Those factors
reconnect apparently independent keys.

**Decision:** build exact identity, factor membership, and homogeneous keyed-component
proofs as shared infrastructure. Let broader D1 emerge only when actual queries contain
components that cannot be represented by one homogeneous proved partition key,
including but not limited to components using different supported leaf classes.

### A5 — use monotonicity as simplification

Monotone corner reasoning has real uses in worst-case tolerance analysis: if response
is monotone throughout a parameter box, the adverse point is a corner. The same
literature also explains why this is fragile—sensitivities can change sign over the
box, and many engineering responses are black-box simulations rather than expressions
whose signs DeciDB can prove. Published work on
[monotonicity and circuit corners](https://doi.org/10.1109/81.873869) illustrates both
the corner method and its dependence on a valid monotonicity argument.

As a whole-query class, A5 often says only “put every value at the improving endpoint,
then check feasibility.” The more useful role is presolve: fix a proved monotone
decision, substitute it, recompute components, and perhaps expose S1, A1, or R1.

**Decision:** do not schedule A5 as a standalone leaf. Reconsider a narrow monotonicity
pass only after normalized sign facts already exist for higher-priority rules.

### A2 — real aggregates, weak direct-solve product value

Mean and median are ubiquitous location estimators, and the weighted-median branch also
matches one-dimensional facility location. NIST describes the
[mean and median as standard location estimators](https://www.itl.nist.gov/div898/handbook/eda/section3/eda351.htm).
The common A2 branches therefore have strong mathematical and practical legitimacy.

They are weaker evidence for a DECIDE feature than for the underlying statistics.
Weighted mean is a simple `SUM(weight*value)/SUM(weight)` expression, and DuckDB already
provides unweighted median and quantile aggregates in its
[aggregate catalogue](https://duckdb.org/docs/current/sql/functions/aggregates).
The displayed weighted-L1 branch is different: DuckDB does not expose an equally
obvious weighted-median aggregate, so that branch deserves a query-corpus check. Real
location problems nevertheless tend to become geometric median, multiple facilities,
capacity, assignment, zoning, or candidate-site selection. The midrange is nonrobust,
and no compelling operational workload was found for the tolerance-aware L0 gap scan.

**Decision:** defer A2 as a family. Treat weighted L1 median as the only branch worth
explicit demand collection; add it only if a real optimization-form query appears or
the machinery comes almost for free from another shared-scalar rule. Do not implement
mean, unweighted median, midpoint, and L0 merely for catalogue completeness.

### A3 — legitimate niche, wrong current priority

Multi-affine vertex evaluation is used in uncertainty analysis. Research on
[multi-affine biochemical systems](https://pmc.ncbi.nlm.nih.gov/articles/PMC9485104/)
uses hyperrectangle vertices to bound or verify chemical-reaction, gene, metabolic,
and oscillator models.

The evidence is theorem-strong but product-weak. These are usually verification tasks
in scientific tools, may involve rational sensitivities or additional physical
constraints, and are useful only at small dimension because the plan has `2^k`
corners. At that same small dimension, the general solver model is also small.

**Decision:** keep the proof in the catalogue, but require a repeated many-component
SQL workload before any recognizer work.

### N2 — useful inner primitive, no standalone query

Linear support over a diagonal ellipsoid appears as a worst-case uncertainty direction
in ellipsoidal robust optimization and as a diagonally scaled trust-region primitive.
The classic robust-optimization foundation is described by
[Ben-Tal and Nemirovski](https://doi.org/10.1287/moor.23.4.769).

The full application generally optimizes another decision outside that inner problem
and adds budget, portfolio, or domain constraints. Full covariance, coordinate bounds,
or ellipsoid projection rather than linear support also invalidates N2.

**Decision:** defer. It may become an inexpensive sibling if N1 infrastructure ships,
but shared code is not sufficient evidence of user demand.

### S3 — plausible equal-value admission, weak exact evidence

S3 fits “admit as many jobs as possible before one total runtime budget” when every
job has equal value and there are no release times, ordering rules, or dependencies.
That is understandable, and the cheapest-prefix plan is attractive.

Credible operational examples rarely stay that simple. Shopify describes a large
[time-budgeted test-selection system](https://shopify.engineering/test-budget-time-constrained-ci-feedback),
but tests have different failure likelihood and confidence value. Project portfolios
add strategic value, labor hours, dependencies, and multiple budgets. Crowdsourcing
and wireless admission add assignment, deadlines, energy, or channel constraints.

**Decision:** defer until a large, repeated, equal-priority, one-resource workload is
provided. There is no current DeciDB benchmark showing whether a solver already
presolves this class cheaply.

### R2 — exact real models, unfavorable expansion economics

R2 has better exact application evidence than its priority suggests. In the
equal-unit-cost, fixed-total variant of stratified sampling, an overall integer sample
size is allocated across strata under bounds to minimize separable variance; Statistics
Canada describes the broader
[integer allocation problem](https://www150.statcan.gc.ca/n1/pub/12-001-x/2015002/article/14249/02-eng.htm).
Its general unequal-cost budget formulation is outside R2.
U.S. House apportionment assigns identical seats through decreasing state priority
values; the Census Bureau documents the operational
[method of equal proportions](https://www.census.gov/topics/public-sector/congressional-apportionment/about/computing.html).
Allocating identical servers among independent queues is another exact marginal-unit
pattern when expected delay is discrete convex in server count; Weber proves the
[corresponding marginal-allocation result](https://pubsonline.informs.org/doi/10.1287/mnsc.26.9.946).
It is not evidence for general staffing.

The current naive relational construction emits and sorts
`W = SUM(hi_i - lo_i)` marginal rows while a solver retains only `n` integer variables.
An implementation can cap useful increments by the remaining resource and emit at most
`W_eff = SUM(MIN(hi_i - lo_i, K))`, but that can still be large. The strongest exact
examples are also small or infrequent, while practical staffing and capacity allocation
adds skills, shifts, heterogeneous costs, time-varying demand, or multiple resources.

**Decision:** defer. Reconsider only with a recurring workload and a strict expansion
cap, or after finding a compressed marginal representation that avoids one row per
unit.

### R4 — solve the fairness users mean

Proportional rationing is real. The initial
[COVAX vaccine allocation mechanism](https://www.who.int/publications/m/item/fair-allocation-mechanism-for-covid-19-vaccines-through-the-covax-facility)
sought to advance countries toward equal population coverage. But its
[allocation explainer](https://www.who.int/docs/default-source/coronaviruse/allocation-of-covax-f-vaccines-explainer-v3-db.pdf?download=true&sfvrsn=516b3714_16)
also continues allocating to other participants after one recipient reaches its
requested cap. Real network and cluster fairness similarly uses progressive filling,
weights, minimum shares, hierarchies, or multiple resources; Hadoop's
[Fair Scheduler](https://hadoop.apache.org/docs/stable/hadoop-yarn/hadoop-yarn-site/FairScheduler.html)
is a representative deployed example.

Current R4 maximizes only the first minimum. Once one `cap_i/demand_i` binds, the
objective fixes the best minimum level but does not determine how leftover resource is
distributed. The proposed rewrite chooses the canonical all-common-level optimum and
may leave resource unused; other primary-optimal assignments may distribute leftovers
arbitrarily. A lexicographic solution is among the scalar-objective optima, but the
scalar objective does not select or guarantee it, and the proposed common-level rewrite
does not compute it.

**Decision:** defer current R4 and investigate capped weighted progressive filling as
a distinct class with a different user-visible objective. Do not use the popularity of
max-min fairness to justify the current scalar-min rule.

### A6 — drop until a formulation-level customer example exists

Aggregate target tracking is real in energy, production, and flexible-load control.
The exact A6 class, however, says that only one affine sum matters and every box-feasible
allocation attaining the target is equally good. Real dispatch formulations nearly
always add comfort, cycling, energy requirements, time dynamics, price, degradation,
ramping, or fairness. For example, published
[thermostatically controlled-load tracking](https://www.mdpi.com/1996-1073/12/9/1757)
combines aggregate tracking with device-level operational requirements. Those terms
distinguish allocations on A6's residual valley.

The relational prefix fill also chooses an arbitrary tied allocation according to row
order. That may be legal under primary-objective equivalence but operationally
unacceptable when the omitted preference is the reason a user asked for optimization.

**Decision:** remove A6 from the near-term roadmap. Reopen it only with a concrete
query whose owner confirms that no secondary allocation criterion is required.

### S4 — a benchmark shape, not a demonstrated workload

S4 chooses exactly `k` items under one budget, maximizes only the largest selected
reward, and fills the other `k-1` positions with the cheapest items regardless of
quality. No convincing real formulation with those semantics was found.

Roster, project, and portfolio selection are the closest intuitive domains, but they
maximize the value of the whole set and add positions, skills, dependencies, locks, or
multiple resources. A representative
[NFL lineup optimizer](https://www.rotowire.com/daily/nfl/optimizer.php?type=main), for
example, combines projected performance with salary and roster restrictions. Those
are general knapsack or assignment models. The Q9 benchmark shows only that one
relational anchor plan is feasible on this easy instance, not that users need the
class. The tracked medians are `0.204 s` with Gurobi and `19.659 s` with HiGHS, but the
selected cost is far below the budget, so the instance does not exercise a difficult
anchor-feasibility boundary. No direct-path timing from the exploratory probe is
retained as durable evidence.

**Decision:** drop S4 from the first implementation waves. Retain the proof as possible
future evidence, not as a roadmap commitment.

### O1 and O2 — useful mathematics with the wrong output contract

One-dimensional ordered matching and transport are real. MatchIt documents optimal
pair matching that minimizes total pair distance and defaults to a scalar propensity
score; only its equal-size, one-to-one, no-caliper, no-strata subcase fits O1.
[Typical matching options](https://kosukeimai.github.io/MatchIt/articles/matching-methods.html)
such as unequal sides, variable ratios, calipers, exact strata, and multivariate
distances do not. One-dimensional Wasserstein distance and transport are common in
statistics and machine learning; the Python Optimal Transport
guide notes that [1D transport is solved by sorting](https://pythonot.github.io/user_guide.html)
and that its sparse option enables very large problems because the solution is sparse.
SciPy's common interface returns only the
[scalar Wasserstein distance](https://docs.scipy.org/doc/scipy-1.16.1/reference/generated/scipy.stats.wasserstein_distance.html).

That evidence argues against the current O1/O2 rules. The useful output is `n` selected
pairs, at most `n+m-1` positive arcs in the admitted monotone transport, or one scalar
distance. The
current DECIDE child is a complete cross product and must preserve one decision value
for all `n*m` pairs, including zeros. That dense contract discards the attractive
sparse API and imposes a scale ceiling, although it does not by itself prove that the
direct rewrite loses to a solver—the solver path also consumes and returns the dense
child. Practical matching commonly requires the options above; physical transport
adds networks, arc limits, fixed costs, commodities, and time.

**Decision:** drop O1 and O2 from the near-term direct-solve roadmap. A future operator
that legally returns only selected pairs, nonzero flow, or the Wasserstein objective
could be valuable, but that is a different observable contract rather than evidence
for these rules. Any claim about a dense-output speed crossover requires an end-to-end
measurement.

## What the current performance evidence does and does not show

The tracked solver measurements establish an opportunity ceiling, not direct-path
speedups:

| Workload | Class | Gurobi query | HiGHS query | Relational core to validate |
|---|---|---:|---:|---|
| P1, 1M independent linear decisions | A1/A5 | `0.626 s` | `0.806 s` | Projection/endpoints |
| P2, 1M independent quadratic decisions | A1 | `0.243 s` | `300.275 s` solver limit | Projection/stationary point |
| P3, 1M one-resource quadratic decisions | R3 | `1.004 s` | `300.329 s` solver limit | Breakpoint event sort and scan |
| P4, 1M decisions / 249,987 groups | S1 + keyed D1 | `0.548 s` | `82.499 s` | Partitioned TopN/window assignment |
| Q11, 136K continuous allocations | R1-shaped; positivity unproved | `0.235 s` | `0.936 s` | Density order and prefix fill after an exact positivity premise |
| Q9, 15K anchor choices | S4 | `0.204 s` | `19.659 s` | Anchor and cheapest-fillers plan |

The solver columns are the existing medians recorded in
[the research notes](07_research_notes.md). A solver-limit row is incomplete and is not
the time to a proved optimum. Q11's current schema/query does not prove its resource
coefficient positive, so its row is a scale and shape observation rather than a query
the optimizer may already admit to R1. Exploratory local probes of the central relational
kernels were encouraging, but their SQL, raw runs, build, and hardware were not
preserved as a repository artifact. Precise probe timings are therefore deliberately
excluded from this durable evidence table. They must be repeated in a controlled
harness with admission proof, full guards, validation, mapping, and readback before any
ratio is called a speedup. P3's current data also has unusually few distinct breakpoint
values.

Two conclusions are already safe:

1. Recognition must occur before solver-neutral model construction and backend loading,
   or much of the removable work remains.
2. Performance cannot rescue a class with no credible workload. Q9/S4 is the clearest
   example.

## Validation program and go/no-go gates

### 1. Build an observed-query corpus before any production commitment

Collect actual optimization requirements and query shapes from DeciDB users or systems
in recommendation, dispatch, numerical repair, calibration, quota selection,
capped-simplex projection, and one-resource allocation. Faithfully reconstructed public
workloads are useful for execution validation, but they do not count as evidence that
anyone will naturally submit the problem through DECIDE. For every observed query,
record:

- the exact normalized class, if any;
- the first realistic requirement that breaks it;
- whether a user would naturally choose DECIDE instead of ordinary SQL;
- expected row, group, segment, and output sizes; and
- whether complete assignment output is actually consumed.

Count near misses separately from exact matches. A class should not move from pilot to
implementation because many papers mention its domain while their real formulations
fall outside its `Valid when` contract.

### 2. Implement one vertical slice, not a generic framework

Use S1 to validate the complete `Match → Prove → Rewrite → Map → Cost → Explain`
contract described in [the research notes](07_research_notes.md). The prototype must:

- recognize from binder-resolved identities and exact facts;
- execute without constructing a solver model;
- return every typed decision value with correct row/entity/scalar fan-out;
- preserve infeasible, empty, NULL-`PER`, `WHEN`, tie, and error behavior;
- expose the chosen rule and proof in `EXPLAIN`; and
- fall back atomically before execution on any proof miss.

Only after that slice works should A1 and R1 reuse the foundation. Avoid designing
generic heterogeneous D1 orchestration before two leaf rules actually require it.

### 3. Measure complete plan economics

For every candidate, report separately:

- normalization and eligibility analysis;
- generated relational execution;
- assignment validation and mapping;
- result readback;
- current model construction, backend loading, solve, and solver readback; and
- peak memory and intermediate cardinality.

Include scale sweeps over groups, ties, segments, integer width, and pair cardinality.
Compare both Gurobi and forced HiGHS. Mark timeout, solver limit, unsupported, and
memory-skip outcomes as incomplete rather than treating them as optimal timings.

### 4. Require class-specific promotion evidence

| Candidate | Evidence required before promotion |
|---|---|
| **S1 + keyed D1** | At least one observed per-key top-k optimization requirement; end-to-end P4; rejection tests for diversity, weighted budgets, crossing constraints, and cross-key coupling |
| **A1** | At least one observed independent bounded DECIDE requirement; full P1/P2 mapping and numeric guards; demonstrate that implementation stays a small reuse of common infrastructure |
| **R1** | At least one observed divisible one-budget requirement, real explicit offer/segment data, and a catalog/query proof of positive resource coefficients; second-resource and nonconcavity rejection; integrated segment-cardinality sweep |
| **R3** | Real capped-simplex or quadratic-dispatch data; adversarial breakpoint/numeric suite; certified residuals against both backends |
| **S2** | One operational upper-only laminar policy and evidence that lower/crossing quotas are not the dominant requested form |
| **A4** | Real grouped calibration with meaningful active bounds and singular groups |
| **N1 signed-L2 projection** | A credible in-database vector workflow, explicit signed-domain bounds, and comparison with a tensor/vector baseline, not only with a solver |

### 5. Use an evidence threshold for every additional class

A deferred class should enter implementation planning only when all of the following
are available:

1. at least one concrete formulation that exactly matches the class;
2. a dataset or generator preserving that formulation at useful scale;
3. an explanation of why DECIDE is preferable to direct ordinary SQL or a specialized
   library;
4. an end-to-end prototype showing that full semantic guards and output mapping retain
   material benefit; and
5. a bounded recognizer and plan-size story.

This threshold deliberately favors a few high-confidence rewrites over a broad
catalogue whose maintenance and correctness surface would outgrow its actual use.

## Final roadmap

The evidence supports the following conditional order:

1. **Phase 0 demand gate:** collect at least one observed optimization-form workload
   for a first-wave class; use reconstructed public workloads only as validation.
2. **Foundation:** exact identity/coefficient facts, typed assignment mapping, outcome
   preservation, narrow homogeneous keyed decomposition, `EXPLAIN`, and a no-solver
   execution assertion.
3. **First architecture proof:** S1 plus homogeneous keyed D1.
4. **Candidate first wave:** A1 and the narrow explicit-segment form of R1.
5. **Measured pilots:** R3, S2, A4, and N1 signed-L2 projection.
6. **No current commitment:** A2, A3, A5 as a leaf, N2, S3, R2, and R4.
7. **Remove from the near-term roadmap:** A6, S4, O1, and O2.

The main strategic conclusion is that a narrow direct-solve vertical-slice prototype
is justified, but a production portfolio is not yet proven. The plausible core is a
small number of common, high-scale query shapes executed through a rigorous shared
proof-and-mapping layer. New rules—and production promotion of the first rules—should
be pulled in by observed queries, not pushed in because an exact theorem exists.
