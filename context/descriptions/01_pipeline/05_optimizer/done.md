# Stage 05 — DECIDE optimizer

Chooses the **mathematical formulation** for every construct the solver cannot take
literally: ABS, MIN/MAX, AVG, `<>`, bilinear products, and composed reducers, and
then **flattens the finished tree into the prepared linear form** that execution
consumes. It assumes canonical input and produces canonical output; it never
decides shape, parses SQL, executes relations, or calls a solver.

**Key source files**

- `src/optimizer/decide/decide_optimizer.cpp` (~210 lines) — the pass dispatcher
  and the helpers the passes share; each pass body lives in a sibling file:
  `decide_rewrite_norm_in.cpp`, `decide_rewrite_notequal_avg.cpp`,
  `decide_rewrite_abs.cpp`, `decide_rewrite_minmax.cpp`,
  `decide_rewrite_bilinear.cpp`, `decide_bound_absorption.cpp`
- `src/optimizer/decide/decide_linear_form.cpp` (~1,600 lines) — the flattening
- `src/include/duckdb/optimizer/decide/decide_optimizer.hpp`
- `src/include/duckdb/optimizer/decide/decide_optimizer_internal.hpp` — helpers
  shared across the pass files
- `src/include/duckdb/optimizer/decide/decide_linear_form.hpp`
- `src/include/duckdb/planner/decide/decide_prepared_model.hpp` — the form itself,
  owned by stage 03 because `LogicalDecide` is what carries it across the boundary

Every constraint it emits re-enters through `LogicalDecide::AddConstraint` and
every objective through `LogicalDecide::SetObjective`, both of which
re-canonicalize and re-verify. `AppendConstraint` is a thin forwarder to the
former.

---

## 0. The solver — and the formulation — are chosen before anything is rewritten

`OptimizeDecide` opens by calling `ChooseDecideSolver` (`decide_solver_gate.cpp`),
which settles two things and records both on the plan:

| Recorded | What it is |
|---|---|
| `LogicalDecide::solver_backend_name` | *Which* backend, from `SelectSolverBackend()`. A name, not a handle — see [`../03_logical_plan/done.md`](../03_logical_plan/done.md) §4. |
| `LogicalDecide::use_native_constructs` | *Which constructs the backend can state itself*, read once off that backend's `SolverConstructSupport`. |
| `LogicalDecide::force_native_constructs` | *The policy* governing them: use a declared construct everywhere, or only as a fallback. False ships; true is the test-only `DECIDB_NATIVE_CONSTRUCTS=force`. |

All three happen here, ahead of every pass, and the rest are the reason for the first.
The passes below decide **how to express** a construct, and the right answer depends
on what the backend accepts natively — a solver with a native `ABS` needs no Big-M
envelope, so lowering it would be work that only loses accuracy.

**Choosing a formulation is this stage's job, and it is settled here rather than
where the rows are built.** Stage 08 used to make that call itself, reading
`Capabilities()` at execution time; it now reads the fields above and translates. The
layer boundary was the thing lost by deferring it, along with the ability of a rewrite
to *see* the choice. Seeing it matters — a rewrite that knows ABS will be stated
natively can skip the sign indicator that only a Big-M envelope has a use for (§ABS
below), which is not expressible while the arm is picked two stages later.

**Capability is not the same question as policy, and MIN/MAX is where they come
apart.** A general constraint and a Big-M family encode the same thing, so where both
are available the choice is a performance one — and, measured, the lowering wins it.
A general constraint relates *columns*, so every member expression that is not already
a column has to be pinned to a fresh one that presolve cannot substitute away; and
`z = MAX(t..)` is an *equality*, so the backend expands both directions while the
lowering emits only the one the clause needs. On the Q9 benchmark shape at 30K rows the
native arm ran 1.8x slower; on a `MAX(e) >= K` constraint at the same size, 41x, and
its cost grew with row count while the lowering stayed flat. Neither arm branched, so
there was no search quality to buy back.

So for MIN/MAX, native is the **fallback**: taken only where the lowering has no valid
Big-M, which is the case it was built to answer and the only one where the lowering
must refuse the query outright. Whether a given clause has a Big-M depends on evaluated
coefficients, so stage 08 answers *that* — it applies this policy to data only it can
see, exactly as the `<>` range collapse does. It does not re-decide the policy.

One consequence is recorded where it lands: **stage 05 allocates no indicator variable
for a MIN/MAX or a `<>` at all.** It briefly allocated one on both arms, because a stage
that cannot see the data cannot know which arm a clause takes and stage 08 could add
columns to the global block but not row-scoped ones — so the native arm carried a binary
per data row that nothing referenced. Both constructs now allocate their binaries in the
pass that emits the rows reading them, in the global block, so the question never arises:
`EmitHardMinMaxClause` and `FindNotEqualConstraints` record only the **marking** — which
clause this is, what it reduces with, and the text to call it in a diagnosis — and the
clause index rides the tag.

The choice is made **once** and rides the plan: `LogicalDecide` → `PhysicalDecide` →
the solve → every diagnostic re-solve. Nothing downstream selects again — stage 08
applies the recorded policy to the data, which is a different act from choosing it. That is not
tidiness: once a rewrite has consulted the backend's capabilities, a second selection
that answered differently would run a model on a solver it was not built for.

None of the three is **serialized**. Which solver a host has is a fact about the host,
not about the query, so a plan deserialized elsewhere re-resolves both rather than
carrying a choice that machine cannot honor. If the DECIDE optimizer is disabled
outright (`SET disabled_optimizers='decide_optimizer'`), physical planning calls
`ChooseDecideSolver` itself — the pass still belongs to this stage, it is merely
triggered from there — so the operator always runs against exactly one backend and
one formulation. It is a no-op once a name is recorded, so that fallback can never
overwrite a choice the rewrites were already selected against.

See [`../07_solver/done.md`](../07_solver/done.md) §2 for what selection reads, and
`SolverConstructSupport` / `SolverModelClass` for what a pass may ask about the
backend.

### The model-class gate

`RequireDecideSolverSupport` (`decide_solver_gate.cpp`) runs once the prepared linear
form exists — right after `BuildDecidePreparedModel`, the first point at which every
constraint's shape and the objective are settled. It compares what the query demands
against what the chosen backend declares.

A **construct** capability is an optimization: a lowering always exists, so a `false`
flag just means this stage lowers as it always has. A **model class** is a gate:
nothing lowers a quadratic constraint or a non-convex objective into linear rows, so
the only honest answer is refusal.

Three of the four model classes are about what a backend can *express*. The fourth,
`singular_quadratic`, is about what it can express *correctly*: HiGHS loads a
rank-deficient Q without complaint and then answers it wrong — stopping partway along
the flat valley of optima, or failing outright, on roughly half of them. It is gated
for the same reason as the others (there is nothing to lower it into) but for a
different cause, and it should be lifted when HiGHS's QP solver improves rather than
treated as a permanent property of the backend.

The refusal is here rather than at bind time, and rather than at model load, for two
different reasons:

- **Not bind time.** The same SQL is legal on every machine. What differs is whether
  this machine has a solver that can run it — so this is a planning failure, not a
  semantic one, and the message blames the host and names the solver to install.
- **Not model load.** The old refusals lived in the HiGHS backend and fired after a
  full scan and a full model build. Nothing about the four classes needs a single
  row: `quadratic_constraints` is tree shape, `miqp` is declared integrality,
  `nonconvex_quadratic` is the sign of `quadratic_sign` against the sense — a
  plan-time constant, since this stage already refuses a scale factor it cannot fold —
  and `singular_quadratic` is a count of distinct `variable_index` values across the
  objective's `squared_terms`, two or more meaning the square couples two decisions.

`DeriveDecideModelClass` is a *prediction* of what `SolverModel::ModelClass()` will
report once the rows are in, and it must never predict less. Where it cannot be exact
it over-reports: `MayHaveIntegralColumn` treats any construct that still owes work at
execution as producing an auxiliary column, rather than enumerating which auxiliaries
are binary — a list that would rot silently. `HasCoupledQuadraticTerms` over-reports
the same way, counting a bilinear objective as coupled without checking whether the
built Q ends up with a nonzero off-diagonal. `SolveModel` re-derives the class from
the built model and asserts the backend covers it, so a prediction that ever comes up
short fails loudly instead of handing a backend a model it cannot load.

---

## 1. Pass order

`DecideOptimizer::OptimizeDecide` runs ten passes, and the order is load-bearing:

| # | Pass | Why here |
|---|---|---|
| 1 | `RewriteNorm` | Lowers bound NORM markers before later passes see their ABS, POWER, MAX, or L0 links. |
| 2 | `RewriteInDomain` | Lowers bound DECIDE-variable IN markers to their indicator formulation. |
| 3 | `TagAbsConstraintsForBigM` | Marks the ABS nodes that will need a Big-M envelope. Must precede `RewriteAbs`, which replaces the nodes. |
| 4 | `RewriteAbs` | Creates auxiliaries replacing ABS nodes; must be first of the remaining rewrites so later passes see plain variables. |
| 5 | `RewriteBilinear` | McCormick linearization for Boolean × anything. |
| 6 | `RewriteComposedMinMax` | Detects composed (additive, mixed-reducer) MIN/MAX **before** single-term MIN/MAX handling. |
| 7 | `RewriteMinMax` | Classifies and rewrites single top-level MIN/MAX in constraints and objectives. |
| 8 | `RewriteNotEqual` | Creates `<>` indicators. |
| 9 | `RewriteAvgToSum` | Last of the rewrites, so every reducer that reaches it is settled. |
| 10 | `AbsorbVariableBounds` | Must be last of all — see below. |

`RewriteComposedMinMaxObjectiveTop` runs within the composed pass for the
objective side. Setting `DECIDB_BENCH` prints `optimizer_ms` for the whole block.

An eleventh pass, `BuildDecidePreparedModel`, belongs to this stage but is
**triggered later** — see §1a.

---

## 1a. Linear-form flattening — `BuildDecidePreparedModel`

Turns each canonical comparison and the objective into additive terms:
`sign * coefficient * variable`. This is where DECIDE does its linear algebra —
distributing `K * (1 - pick)` into `K - K*pick`, pulling coefficients out of `*`
chains, pushing a divisor into every produced coefficient, folding unary minus,
stripping binder casts, and multiplying a peeled reducer scale into everything the
reducer produced.

The output is `LogicalDecide::prepared`, a `DecidePreparedModel`: a
`vector<DecideConstraint>` plus an optional `DecideObjective`. It also fills
`ComposedMinMaxTerm::inner_terms`, so every construct reaches execution as
prepared terms rather than one path re-deriving its own.

**A coefficient stays an unevaluated `Expression`.** `SUM(x * price)` yields the
coefficient `price`, which is a number only once a row exists. Everything else
about a term — which variable it names, its sign, which reducer produced it, which
filter applies, which entity scope qualifies it — is a fact about types and
structure and is settled here. The pass reads no data.

**Why it is here and not at execution.** Doing this algebra needs a binder: every
rebuilt subtree goes through `RebindOperator` → `FunctionBinder::BindScalarFunction`
so the operator is resolved for the children it is actually given. Performing it
downstream of the binder meant reconstructing bound nodes by hand from another
node's `FunctionData`, which does not fail on a type mismatch — it reinterprets the
children's physical representation and returns a plausible wrong number. That
workaround is gone.

### Like-term collection

After flattening, every list that becomes a **linear** solver row is grouped:
constraint `lhs_terms`, the objective's `terms`, and each
`ComposedMinMaxTerm::inner_terms`. `2*ship + 3*ship` becomes one term with
coefficient `2 + 3`. A term contributes `sign * coefficient`, so a group merges as
`sign_first * (coef_first ± coef_next ± ...)`, taking `-` exactly when a term's
sign differs from the group's.

**Naming the same variable is not sufficient.** A term also records *which rows it
applies to* and *which reducer produced it*, and `TermsAreLike` refuses to merge
across any of that:

| Field | Why it separates two terms |
|---|---|
| `reduction` | A reducer term and a row-invariant one are summed differently downstream |
| `filter` | The aggregate-local `WHEN`. `SUM(x) WHEN a` and `SUM(x) WHEN b` are one column over two row sets |
| `avg_scale` | AVG divides by the group's row count; merging before that scaling applies the division to both |
| `qualifier_scope_idx` | Selects a de-duplication mask (`sum(D: ...)`) — again a statement about which rows contribute |

Constants (`INVALID_INDEX`) are left alone: a fixed offset folded into the RHS is
not a repeated column. Quadratic and bilinear terms are left alone too — they feed
an outer product rather than a linear row.

**This closes a trap, not a defect.** The model builder already folded duplicate
column entries when writing the matrix row, so the emitted model was always
correct and no result-level test could fail. But every other consumer had to
remember that a variable index can repeat, and one did not: the implied-bound
derivation read a single term's coefficient instead of the sum. That is why the
golden dump is unchanged by this pass — and why `test_like_term_collection.py`
pins the *non*-merges, which are the observable direction.

**Why it is triggered from `plan_decide.cpp` rather than inside `OptimizeDecide`.**
The prepared terms hold *copies* of coefficient subtrees. `RemoveUnusedColumns`,
`ColumnLifetimeAnalyzer` and late materialization all run **after**
`OptimizerType::DECIDE_OPTIMIZER` and rebind the operator's own expressions;
copies taken during `OptimizeDecide` keep column bindings that no longer name the
right input columns, which silently reads the wrong data column into a
coefficient. `PhysicalPlanGenerator::CreatePlan(LogicalDecide &)` is the first
point at which bindings are final, and it is already the checkpoint where
`VerifyCanonical` runs for the same "everything is settled now" reason. The pass
runs immediately after that verification, so the ordering `verify → flatten` is
preserved. **Ownership is unaffected**: the code lives in
`src/optimizer/decide/`, and nothing in `src/execution/` performs the algebra.

---

## 2. The passes

### ABS linearization — `RewriteAbs`

Detects `BoundFunctionExpression` for `ABS` over decision variables, creates
auxiliary `REAL` variables, replaces the ABS nodes with references to them, and
emits `aux >= inner` and `aux >= -inner`.

`AbsPairInfo::needs_bigm` records whether the auxiliary needs an upper envelope.
It is false when solver pressure pins `aux` to `|inner|` naturally — a `MINIMIZE`
objective, or a constraint shape that already upper-bounds `aux`. When true
(constraint hard direction, or `MAXIMIZE` + objective), `abs_maximize_links`
carries the auxiliary and a binary sign indicator, and execution derives the two
upper-bound rows from the tagged lower-bound constraints.

`TagAbsConstraintsForBigM` decides this by reading the constraint as `E <op> 0`
and asking whether each ABS term's signed position pushes its auxiliary down.
`CollectAbsWithSign` folds signs through `+`, unary and binary `-`, casts,
aggregate bodies, and constant factors. A factor whose value is not known until
execution — a data column, as in `SUM(w * ABS(x - t))` — yields sign 0, which
never matches the pinning direction and so forces Big-M.

That is deliberately conservative: a row whose `w` happens to be positive gets
an envelope it does not need. Assuming such a factor positive is unsound, since
a negative `w` makes enlarging the auxiliary *relax* the row, and the constraint
silently stops binding. The cost was measured 2026-08-14 on a weighted-ABS
constraint over TPC-H `lineitem`, comparing a literal coefficient (pinned, no
Big-M) against an all-ones column (Big-M) so the two models are mathematically
identical: 0.16s vs 0.65s at 30K rows and 0.26s vs 1.29s at 60K, same optimum
both ways. A ~5x constant factor, still near-linear in row count — cheap enough
that deciding sign per row at execution time is not warranted.

### NORM and IN — `RewriteNorm` / `RewriteInDomain`

The binder keeps `norm(...)` as an aggregate-shaped DECIDE marker so normal
aggregate-local `WHEN` and `PER` binding remains available. The optimizer lowers
L1, L2, and infinity norms to `SUM(ABS)`, `SUM(POWER(_, 2))`, and `MAX(ABS)`;
L0 emits its existing Boolean indicator and exact forward/reverse links.

A bound `x IN (...)` stays a native `COMPARE_IN` marker until this pass. It then
uses the existing singleton, Boolean-domain, or cardinality/linking formulation,
copying an expression-level `WHEN` to every generated row. These helper rows are
structural for today’s infeasibility repair: they must never be independently
loosened. Atomic source-level DROP repair is recorded in `todo.md`.

### Bilinear — `RewriteBilinear`

| Operand types | Formulation |
|---|---|
| Boolean × Boolean | AND-linearization: `w <= b1`, `w <= b2`, `w >= b1 + b2 - 1` |
| Boolean × other | Partial McCormick: `w <= x` at plan time, `w <= U·b` and `w >= x - U·(1-b)` at execution time via `BilinearLink` (needs runtime bounds) |
| Non-Boolean × non-Boolean | Left in place for the Q-matrix path in the physical operator |

`ExtractMultiplicativeCoefficient` walks a chain shaped
`coeff * ... * decide_var * ... * coeff` and combines the non-variable factors.

### MIN/MAX — `RewriteMinMax`

**Constraints.** An *easy* direction (`MAX(...) <= K`, `MIN(...) >= K`) strips the
reducer and becomes a per-row constraint — one row per data row, no Big-M. A *hard*
direction creates a Boolean indicator per active row and rewrites to
`SUM` + linking rows (`EmitHardMinMaxClause`). Equality splits into both
directions. `WHEN` / `PER` wrappers are preserved; `out_was_easy` tells the caller
whether `PER` should be stripped, since an easy rewrite has already become
per-row.

**Objectives.** Flat and nested-`PER` patterns are detected and the easy/hard
classification is precomputed into typed metadata — `flat_objective_agg` /
`flat_objective_is_easy`, and `per_inner_agg` / `per_outer_agg` /
`per_inner_is_easy` / `per_outer_is_easy` / `per_inner_was_avg` — so the physical
layer reads the decision rather than re-deriving it. `RewriteMinMaxObjectiveTree`
is split out from `RewriteMinMaxObjective` because the rewrite writes through a
pointer that walks *into* the tree past `PER`/`WHEN` wrappers and casts, and has
early returns: the objective must be detached, rewritten, and reinstalled through
`SetObjective`.

### Composed MIN/MAX — `RewriteComposedMinMax`

An additive LHS mixing `SUM`/`AVG` with `MIN`/`MAX` terms cannot go through the
single-term path. Each term's metadata is extracted into
`composed_minmax_constraints` (or `composed_minmax_objective_terms`) as a
`ComposedMinMaxTerm`, and the comparison is replaced with a no-op `TRUE`
placeholder so the physical layer owns emission. Non-comparison leaves are legal
under C0 precisely so that placeholder verifies.

For the objective, `RewriteComposedMinMaxObjectiveTop` installs a **constant
placeholder** and supplies the coefficients from
`composed_minmax_objective_terms` instead — which is why a decision-free objective
is legal and returned unchanged by the canonicalizer.

### Auxiliary boxes — `AuxRange`

Every auxiliary column DeciDB introduces is created in `ilp_linearization.cpp`, through
one of two helpers: `AddGlobalContinuousAux` or `AddGlobalBinaryAux`. Nothing else in
the codebase touches `SolverInput::num_global_vars`.

A continuous auxiliary always stands for the extremum of a known family of row
expressions, so its box is derivable at the moment it is created — and the same walk
over the data that produces the Big-M constant produces it. `AuxRange` is that walk's
return value: `lo`/`hi` bracket the expression, constants included, since the auxiliary
is pinned against the whole thing. `Span()` — the bracket's own width, and the only
Big-M an extremum link may use — is derived from the range rather than the other way
round, which is what keeps the endpoints from being computed and discarded. There is
deliberately no constant-free counterpart: one existed until 2026-08-23 and was invalid
wherever a family's rows carried different constants (`../06_model_formulation/done.md`
§9).

`AddGlobalContinuousAux` takes an `AuxRange` and cannot be called without one. Infinite
bounds are reachable only via `AuxRange::unbounded` — set when a contributing decision
variable has no finite bound. `SolverModel::Build` asserts that pairing, so a new
creation site that skips the derivation fails loudly in a debug build instead of
quietly costing a benchmark. A free auxiliary *box* is sound; the same condition on the
Big-M side is not, and there it refuses the query rather than substituting a constant
(`../06_model_formulation/done.md` §9).

The five continuous auxiliaries and the family each is boxed by:

| Auxiliary | Reduces over | Box |
| --- | --- | --- |
| flat `z` | the objective's rows | per-row family |
| `z_g` (inner `PER` MIN/MAX) | rows of its group | per-row family |
| `w` (outer over `z_g`) | the `z_g` values | per-row family |
| `w` (outer over group sums) | per-group sums | **group-sum family** |
| `z_k` (composed term) | rows the term's filter admits | that term's own family |

The group-sum `w` is the one that does *not* share the per-row family: a group sum
leaves any single row's range as soon as the group holds more than one row. It is
derived from the actual per-group sums under the same inner-AVG scale the pinning rows
use — correct, and tighter than widening the per-row range by the row count.

> Fixed 2026-08-18. All five sites had been declaring their auxiliary
> `[-1e30, 1e30]`, the endpoints computed for Big-M and then dropped. Not a wrong
> answer — a performance cliff: a free continuous column leaves the root simplex with
> no box, so it walks to the answer one pivot at a time and the walk lengthens with row
> count. Q9 (`MAXIMIZE MAX`) at 15K rows spent 27,293 root simplex iterations over
> 28.75s; boxed, the same model takes 61 iterations and 0.02s, at identical node count
> (1), gap (0.000%) and optimum. The golden dump moved by exactly seven `col` lines with
> no row changed, and every one of the corpus's 87 models was re-solved to confirm an
> unchanged status and optimal value. Three corpus results shifted to a different
> optimal assignment at the same objective value.

### `<>` — `RewriteNotEqual`

Each `COMPARE_NOTEQUAL` gets a Boolean indicator recorded in
`ne_clause_labels`. The disjunction rows are generated at execution
time, once bounds are known.

### AVG → SUM — `RewriteAvgToSum`

Replaces a decision-bearing `AVG` with `SUM`, tagged `AVG_REWRITE_TAG` so
coefficient evaluation scales the terms by the right active-row count (total,
`WHEN` mask, group size, or aggregate-local filter). A **decision-free** `AVG` is
skipped: there is nothing to linearize, and rebinding it as `SUM` would redeclare
it with `SUM`'s integral type while its value stays fractional. Keeping the tag on
objective terms is what makes a mixed `AVG(a) + SUM(b)` preserve true AVG
semantics.

### Bound absorption — `AbsorbVariableBounds`

`x <= 10` is one fact about a column, so it belongs in that column's box rather than
in `num_rows` identical model rows. A box is also strictly better for the solver:
smaller model, tighter presolve. The pass folds every simple `x OP const` and
`x BETWEEN a AND b` into `absorbed_lower_bounds` / `absorbed_upper_bounds` on
`LogicalDecide`, and tags the comparison `ABSORBED_BOUND_TAG` so physical extraction
skips it.

A bound qualifies when the LHS is a bare decision variable — not inside a reducer —
and the RHS is a literal under any number of casts. Recursion goes through `AND` and
into child 0 only of `PER` and `WHEN`. **A `WHEN`-guarded comparison is never
absorbed**: it is conditional per row, not a domain.

- `<=` tightens the upper bound (`min`), `>=` the lower (`max`), `=` intersects both.
  Intersecting rather than assigning is what makes `x = 5 AND x = 10` invert the box
  and be caught, instead of silently resolving to whichever was written last.
- Strict `<` / `>` on an integer tighten by ±1, carrying the user's typed literal so
  the diagnosis re-quotes `< 10` rather than the normalized `<= 9`.
- Strict `<` / `>` on a REAL variable are deliberately **not** absorbed. The pass
  declines, the comparison stays a row, and `ApplyComparisonSense` in the model
  builder rejects it with a message naming the clause.
- A non-finite constant is **not** absorbed either: the box has one sentinel per
  direction and cannot hold it.

Lower bounds start at `ABSORBED_LOWER_UNSET` (-1e30) rather than 0, so an explicit
negative bound is honored instead of clamped up. Stage 08 resolves anything still at
the sentinel to the default 0 floor.

**Upper bounds start at the declared type's ceiling**, not uniformly at 1e30: a
BOOLEAN variable is seeded to `1.0`. The box is what stage 06 receives as
`SolverInput::upper_bounds`, and every Big-M derivation reads it through
`DecideRowTermRange`, which treats `>= 1e20` as unbounded. Seeding only the sentinel
left a declared `BOOL` looking unbounded to all of them, so they fell back to the fixed
1e6 Big-M that existed at the time — `SUM(x) <> 2` over four BOOL decisions took
`M = 1000000` where the identical feasible set spelled `x(INT) ... x <= 1` took `7`.
The ceiling was reaching only the model builder, which applies it far downstream when
sizing columns. (That fallback is gone now: an unbounded contributor is refused. So the
same gap today would not cost a loose `M`, it would refuse the query outright — the
seeding matters more, not less.)

Seeding it here is safe because absorption only ever narrows (`min`), and because a
user restatement like `x <= 1` on a BOOL was already treated as a no-op against the
intrinsic box. It also lets the `<>` range collapse see a BOOL's upper side, which it
could not before: `SUM(x) <> 9` over four BOOL decisions is unreachable and now drops
to nothing instead of emitting a disjunction against a bound no assignment can meet.

> Fixed 2026-08-17. The golden dump moved (Big-M constants shrank) with
> `baseline.dump.results` byte-identical — the models changed, no optimum did.
> Corpus query 85 pins the tight constant, since queries 82–84 either collapse or use
> INT decisions and so never exposed it.

Each absorbed bound is also recorded as a `UserBoundSpec` so infeasible diagnosis can
re-emit it as a loosenable row — absorbed bounds carry no row provenance otherwise. A
variable's *intrinsic* domain is excluded: a BOOLEAN's `[0,1]` box is never
synthesized as a constraint, so it only appears here when the user restated it
redundantly, and `is_boolean_var` is consulted to skip that restatement. A genuine
BOOLEAN pin (`x <= 0`, `x >= 1`) does tighten the box and is recorded.

**Why it runs last.** `RewriteInDomain` emits a floor-lowering `x >= min_value` for
an all-constant `IN` domain whose minimum is negative — itself an absorbable shape.
Absorb before that pass and the floor stays at 0, the negative domain value becomes
unreachable, and the solver returns a *different answer* rather than an error.
Running last also guarantees every auxiliary variable created by `RewriteAbs`,
`RewriteBilinear` and `RewriteMinMax` already exists before the box is sized.

**Why here and not stage 04.** "Bound or row" looks like shape, which stage 04 owns,
but stage 04's contract rules it out three times: that pass is pure (it returns a new
tree; absorption writes into bound arrays), it is total and never declines
(absorption declines on strict-`<`-over-REAL), and §5 explicitly excludes choosing a
solver formulation, which bound-versus-row is. Stage 05 already chooses formulations
with types in scope and is forbidden only from evaluating *data* — a foldable literal
is not data.

**Why the comparison is tagged rather than replaced.** `RewriteComposedMinMax`
replaces a handled comparison with a `TRUE` placeholder, but that is the wrong
precedent here: `EXPLAIN` renders the constraint list from this tree, and the user
wrote this bound. Tagging moves the decision upstream without changing what the user
sees.

---

## 3. A factor on a reducer stays outside

The canonicalizer peels a query-wide factor *outward* off a reducer
(`2 * SUM(x*p)`, `2 * MAX(x*v)`) and converges every spelling onto one. **No pass
here pushes it back in.**

`MIN`/`MAX` are order statistics, so a negative factor turns one into the other —
`MAX(-2x)` is `-2·MIN(x)`. Folding therefore needs the factor's sign, which an
uncorrelated scalar subquery does not supply until the query runs. Kept outside,
the sign is not a correctness input at all; it only selects which linearization is
cheaper. `ScaleSignAtPlanTime` returns +1, -1, or **0 for "not known until the
query runs"**, and 0 must be treated as "take the expensive but exact form", never
as an error. A scale of -2 flips the direction `z` is pushed exactly as a
subtraction would, so a known sign participates in `is_easy` alongside `sign`;
an unknown sign forces `is_easy` false, and the indicator layer pins `z` to the
true MIN/MAX in both directions — correct whichever sign it turns out to have.

`WalkComposedLhs` records the factor on `ComposedMinMaxTerm::scale` (with
`scale_divides` for the `AGG / factor` spelling). The physical layer applies it
where the term lands: multiplied into per-row coefficients for `SUM`/`AVG`
(`ApplyScaleToExtracted`), or into the auxiliary's contribution for `MIN`/`MAX`,
or distributed over the per-row form when the constraint linearizes the easy way.

A parsed-level fold used to do this **without** the sign check. It was a silent
wrong answer.

---

## 4. Optimizations not in this pass

Two things reduce problem size before the optimizer runs, and are worth knowing
because they dominate everything above:

- **`WHERE`-clause filtering** — standard DuckDB predicate pushdown, no
  DECIDE-specific code. Rows eliminated by `WHERE` never become decision
  variables, which for most queries is the single most impactful optimization
  there is.
- **`WHEN`-condition coefficient zeroing** — stage 08. Aggregate constraints
  multiply coefficients by the mask; per-row constraints omit the row entirely;
  objectives zero the coefficient. `WHEN`-excluded rows get `INVALID_INDEX` in
  `row_group_ids`, so they fall out of every group.

Backend selection (Gurobi preferred, HiGHS fallback) and the shared time limit are
stage 07.

---

## 5. What this stage does not do

- Judge the polynomial degree of an expression — stage 02, which owns the one
  definition (`DecideExpressionDegree`) and refuses degree > 2 on the bound tree
  before any pass here runs. This stage *calls* that definition, never a copy of
  it, and only to assert: `AssertSquaredInnerIsLinear` throws `InternalException`
  when the inner expression of a `POWER(..., 2)` or a self-product is not linear,
  and `ClassifyNormalizedProduct` does the same for a product with more than two
  decision factors. Both were `InvalidInputException` and reachable by a user —
  that was the leak, since a per-row constraint bypassed stage 02's gate
  entirely. They are kept because the passes here synthesize their own
  expressions (the `IN` expansion, ABS linearization, `norm` lowering) that stage
  02 never saw, so nothing upstream can vouch for them; an assertion is the only
  thing that can. See [`../02_binder/done.md`](../02_binder/done.md) §2.

  Two refusals in `ClassifyNormalizedProduct` stay user-facing and are *not*
  degree: a product factor that is not a bare decision (`ABS(x) * y`) and a
  same-variable product outside the recognised quadratic patterns (`x * (2*x)`).
  Both are degree 2, which stage 02 legitimately admits — they are formulation
  limits of this stage, which is exactly what this stage may decide.
- Decide which side of a comparison a term sits on — stage 04. Every pass here was
  audited against that: ABS and bilinear replace decision-bearing atoms with
  decision auxiliaries; MIN/MAX and AVG keep decision terms on the left; `<>`
  changes only metadata; composed MIN/MAX leaves a permitted placeholder;
  `AbsorbVariableBounds` only annotates.
- Evaluate anything against data, or emit solver rows. Flattening produces
  coefficient *expressions*; turning them into numbers is stage 08.

---

## 6. Source map

| Concern | Location |
|---|---|
| The pass dispatcher and shared helpers | `src/optimizer/decide/decide_optimizer.cpp`, `decide_optimizer_internal.hpp` |
| `norm` / `IN` | `src/optimizer/decide/decide_rewrite_norm_in.cpp` |
| `<>` / AVG→SUM | `src/optimizer/decide/decide_rewrite_notequal_avg.cpp` |
| ABS | `src/optimizer/decide/decide_rewrite_abs.cpp` |
| MIN/MAX, plain and composed | `src/optimizer/decide/decide_rewrite_minmax.cpp` |
| Bilinear McCormick | `src/optimizer/decide/decide_rewrite_bilinear.cpp` |
| Bound absorption | `src/optimizer/decide/decide_bound_absorption.cpp` |
| Linear-form flattening | `src/optimizer/decide/decide_linear_form.cpp` |
| Backend selection and the model-class gate | `src/optimizer/decide/decide_solver_gate.cpp` |
| Pass inventory and helper contracts | `src/include/duckdb/optimizer/decide/decide_optimizer.hpp` |
| The prepared form's structures | `src/include/duckdb/planner/decide/decide_prepared_model.hpp` |
| Where flattening is triggered | `src/execution/physical_plan/plan_decide.cpp` |
| Metadata the passes write | `src/include/duckdb/planner/operator/decide/logical_decide.hpp` |
| Canonicalizing entry points | `src/planner/operator/decide/logical_decide.cpp` |
| Per-function user-facing semantics | `../../03_expressivity/sql_functions/done.md` |
| Bilinear semantics | `../../03_expressivity/bilinear/done.md` |
