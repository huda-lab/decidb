# Stage 05 — DECIDE optimizer

Chooses the **mathematical formulation** for every construct the solver cannot take
literally: ABS, MIN/MAX, AVG, `<>`, bilinear products, and composed reducers, and
then **flattens the finished tree into the prepared linear form** that execution
consumes. It assumes canonical input and produces canonical output; it never
decides shape, parses SQL, executes relations, or calls a solver.

**Key source files**

- `src/optimizer/decide/decide_optimizer.cpp` (~330 lines) — the pass dispatcher
  and the helpers the passes share; each pass body lives in a sibling file:
  `decide_rewrite_norm_in.cpp`, `decide_rewrite_notequal_avg.cpp`,
  `decide_rewrite_abs.cpp`, `decide_rewrite_minmax.cpp`,
  `decide_rewrite_bilinear.cpp`, `decide_rewrite_scoped.cpp` (the value rewrite),
  `decide_bound_absorption.cpp`
- `src/optimizer/decide/decide_linear_form.cpp` (~1,700 lines) — the flattening
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

`DecideOptimizer::OptimizeDecide` runs eleven passes, and the order is load-bearing:

| # | Pass | Why here |
|---|---|---|
| 1 | `RewriteNorm` | Lowers bound NORM markers before later passes see their ABS, POWER, MAX, or L0 links. |
| 2 | `RewriteInDomain` | Lowers bound DECIDE-variable IN markers to their indicator formulation. |
| 3 | `TagAbsForBigM` | Marks the ABS nodes, in the constraints and the objective, that will need a Big-M envelope. Must precede `RewriteAbs`, which replaces the nodes. |
| 4 | `RewriteAbs` | Creates auxiliaries replacing ABS nodes; must be first of the remaining rewrites so later passes see plain variables. |
| 5 | `RewriteBilinear` | McCormick linearization for Boolean × anything. |
| 6 | `RewriteScopedReducers` | The value rewrite: a VALUE-shape comparison's reducers become helper decisions. After ABS and bilinear, whose auxiliaries may stand in a reducer's body; before the MIN/MAX passes, which never see these reducers. |
| 7 | `RewriteComposedMinMax` | Detects composed (additive, mixed-reducer) MIN/MAX **before** single-term MIN/MAX handling. |
| 8 | `RewriteMinMax` | Classifies and rewrites single top-level MIN/MAX in constraints and objectives. |
| 9 | `RewriteNotEqual` | Creates `<>` indicators. |
| 10 | `RewriteAvgToSum` | Last of the rewrites, so every reducer that reaches it is settled. |
| 11 | `AbsorbVariableBounds` | Must be last of all — see below. |

`RewriteComposedMinMaxObjectiveTop` runs within the composed pass for the
objective side. Setting `DECIDB_BENCH` prints `optimizer_ms` for the whole block.

A twelfth pass, `BuildDecidePreparedModel`, belongs to this stage but is
**triggered later** — see §1a. It also flattens each `ReducerValueLink`'s body into
`inner_terms`, as it does a composed term's.

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
filter applies, which key scope (`EntityScopeInfo`) its reducer's own `per` names — is a
fact about types and structure and is settled here. The pass reads no data.

**Why it is here and not at execution.** Doing this algebra needs a binder: every
rebuilt subtree goes through `RebindOperator` → `FunctionBinder::BindScalarFunction`
so the operator is resolved for the children it is actually given. Performing it
downstream of the binder meant reconstructing bound nodes by hand from another
node's `FunctionData`, which does not fail on a type mismatch — it reinterprets the
children's physical representation and returns a plausible wrong number. That
workaround is gone.

**A clause's scope** (stage 04 §3.10). `AnalyzeConstraint` reads the wrappers the binder
nests around a comparison. A `when c:` wrapper (`WHEN_CONSTRAINT_TAG`) gives
`DecideConstraint::when_condition`. A `per K:` wrapper (`PER_CLASS_TAG`, kept over every
shape) gives `class_key`, empty for `per ()`, and `per_key`: `ONE_ROW_PER_CLASS` over the
per-row shape, `NAMES_CLASSES` over the aggregate shape and an easy MIN/MAX, whose rows are
their `by` groups and their members and only stand for the classes (D28). The
comparison's `by` stamp gives `group_key`, an index into `LogicalDecide::group_keys`. An
easy MIN/MAX (`MINMAX_EASY_REWRITE_TAG` on the comparison, its reducer kept:
`was_minmax_easy`) extracts its body's terms with the reducer's `when` as their filter,
one row per member (`ExtractEasyExtremumTerms`). The filter is attached once, after
extraction, to every term the body produced — linear, bilinear and quadratic alike
(`AttachReducerFilter`, shared with the aggregate shape) — rather than threaded through
the extraction branches: a branch that forgot it (the decision-free cast arm once did)
left one term unfiltered, and stage 08 then read every kept row as a member, so the rows
the reducer's own `when` dropped reached the model as `constant >= K` and a feasible query
was called infeasible. A comparison classified `INVALID`, or still in the VALUE shape, is
an `InternalException`: stage 04 refuses the first and the value rewrite replaces the
second. So is a term that holds a decision under anything but a bare reference
(`ExtractTerms`): no rewrite lowered it, and reading it as its decision would drop the
construct without a word.

The objective's `when` wrapper gives `DecideObjective::when_condition`. An objective
`AVG` with a `by` key -- the inlined `sum(per k: avg(e) by (k))` of the value rewrite --
carries that key on each of its terms (`DecideTerm::avg_group_key`), so execution divides
by each group's count. A decision outside the objective's reducers
— query-wide or not — is a `NONE` term, counted once at execution, and so is a product of
two query-wide decisions (`MAXIMIZE SUM(x) + s * u`, a bilinear term with `reduction`
NONE; inside a reducer it is SUM and weighted by every row, `SUM(w * s * u)`). Any other
product of decisions left there is refused (*"... multiplies decisions outside a reducer,
which is not supported yet"*), since it has no once-only reading in the model.
`RewriteBilinear` has already linearized a product with a `BOOL` decision by then.

### Frame selectors

**A range over decisions** becomes a helper decision first, in `RewriteFrameRanges`
(`decide_rewrite_frames.cpp`), which runs before every other rewrite so none copies a
range. It enters through the value rewrite's door: one walker finds both rewrites' sites
with the sign each carries (`CollectDecisionSites`, a reducer's or a range's), one rule
gives a MIN/MAX helper its sides (`ExtremumSides`), and one allocation makes every helper
(`AddValueHelper`), so a third kind of helper cannot miss a guard. The value rewrite
records which link each of its reducers holds (`Helpers::links`), since a range's link may
stand before its own; read by position, a reducer's second occurrence -- the copy an ABS
envelope holds -- once took a range's helper. Placement's clause id is asserted against the
comparison's, and a range on a comparison's bound is an internal error: the marker is replaced by the helper, an ordinary keyed decision to every pass
after it, and the canonical shape stands. The helper has a scope of its own, one instance
per **window** of the frame — the range several anchors share: its timeline when neither
end depends on the anchor, else its position (peer), split per anchor where it reaches an
`else` that reads the anchor's own row — which execution builds from the frame map
(`EntityScopeInfo::value_link` naming a `ReducerValueLink` whose `frame` is set). The link
holds the payload (`inner_expr`) and, flattened below, the `else` (`fallback_terms`); stage
06 defines the helper per window, an equality for SUM and AVG, an extremum link for MIN and
MAX. A MIN/MAX helper's sides are read off the way its comparison pushes it, through `+`,
`-`, binder casts, data factors of known sign and the members of a SUM or AVG; both sides
under anything else (ABS, a norm, a MIN or MAX: D73). It is whole-numbered when the range
is, by the binder's test (the type payload and `else` share), so a strict bound on it steps
exactly and one on a fractional range was refused at bind time.
EXPLAIN reads *"a helper decision per timeline"* or *"... per position"*
(`FRAME_HELPER_PER_TIMELINE`, `FRAME_HELPER_PER_POSITION`). `count` over decisions never
gets here: the binder refuses it.

The passes before flattening carry a point frame's marker as one term (the value rewrite
lifts none: `CollectReducerSites` skips it), so an ABS envelope, a `<>` disjunction or a
MIN/MAX member holds the frame where the clause did. Flattening lowers it
(`../../00_project_overview/syntax_reference.md` §5.6; D72):

- **A frame over decisions** is its terms (`ExtractFrameTerms`): each term `c·x` of its
  payload becomes `READ(c)·x` with `DecideTerm::frame_read` set — the decision read at
  the row the frame selects — and each term `c'·y` of its fallback becomes
  `FALLBACK(c')·y`, read at the row itself. A data factor on the frame multiplies every
  coefficient (`ExtractScaledFrameTerms`). Inside a reducer the frame's terms take the
  reducer's filter and scale like any; no helper is made.
- **A frame over data**, wherever execution evaluates an expression — a bound, a constant
  term, a coefficient (`LowerFramesInPreparedModel`) — becomes one `VALUE` column of the
  marker's type: the payload at the selected row, the fallback at the anchor, so no
  arithmetic joins them and a DATE or BOOLEAN payload reads as itself. A range over data
  becomes one `REDUCED` column of its reducer's type, the marker itself (the bound
  aggregate) carried for execution to run over each window's values, its `else` beside it.
  Every frame's `else` gets `FALLBACK` columns (`ExtractFallbackTerms`, shared by points and
  ranges), listed per frame in `frame_fallback_columns`, which the signature keys a reached
  `else` by (`RecordFallbackColumns`). One in a bound-side reducer's argument
  that may read nothing adds its `PRESENT` column to that reducer's own `when`, so
  execution drops the member as it drops any its `when` drops.
- `VALUE`, `REDUCED`, `READ`, `FALLBACK` and `PRESENT` are chunk columns after the input's
  (`DecidePreparedModel::frame_columns`, from `frame_column_base`), which execution
  computes; a coefficient or bound reading one is an ordinary expression.
  `frame_payload_terms` lists each frame's payload terms (decision, `READ` column, sign),
  recorded once, for the peer check (D67); `frame_fallback_decisions` lists its fallback's
  decisions, for the signature field (D76).
- A frame over decisions inside a composed MIN or MAX is flattened like any term: stage 06
  reads each term's variable as execution numbers it, a frame read included. One beside a
  MIN or MAX in the objective is refused as any decision there is (*"DECIDE: 'at(first: x)
  over (t.period)' beside MIN or MAX is not supported yet"*, syntax_reference.md §10.2).
- **Members a frame drops** (D81, D82). A reducer over decisions a frame may drop a member
  of is a helper (the canonicalizer's VALUE shape and objective tag). As `DecideFrameSpec`
  does for a frame, its link records, at plan time, what makes it absent and where it is
  read: `member_frames` (the reducer's `MEMBER_FRAME_TAG_PREFIX` tags), `absence_frame` (a
  member frame, else a nested helper's -- `ReplaceReducer` returns each nested link -- or,
  for a range's link (`LiftRange`), its frame where it may read nothing) and `read_at_root`
  (a row stating the clause's instance reads it outside its reducers: a top-level site of
  a row that is no member row, a range not inside a reducer), whence `MayBeAbsent`,
  `SkipsInstancesOf` and `MayLeaveObjectiveUnset`. The linear form joins each member frame's
  `PRESENT` column to the link's own `when` (`JoinPresence`), so execution lists a member
  only where its frames select. A nonlinear operand beside a member frame has no linear
  definition and is refused naming the frame; `InlineSumOfGroupSums` keeps such a reducer a
  helper. Two internal checks hold the routing: a reducer summed in place has no frame in its
  argument that may select nothing (`AssertMembersListed`, read off the argument), and no
  term inside a reducer summed in place reads a helper that may be absent
  (`AssertNoAbsentHelperSummed`).
- **Where a helper is read** (phase 7, D83). Last of all, the linear form decides once, over
  everything it flattened, where stage 06 may write a helper's members in place of its
  column (`MarkInlinableHelpers`, `ReducerValueLink::inlining`): into its readers, when
  every one is a linear row of a plain clause (no `<>`, hard MIN/MAX, Big-M ABS pair,
  product or square) or the linear objective (not a flat MIN/MAX's, whose terms stage 06
  links column by column); or into one outer link (`inline_outer`), when that link's
  members are its only reader and, if a MIN or MAX, it is pinned on one side, so each member
  stands in one row. No clause may be folded into the helper's box (`LogicalDecide::
  HasAbsorbedBound`: the bound would be lost with the column). A reader of any other kind --
  a composed MIN/MAX term, a quadratic group, a McCormick link, a frame's payload, a clause
  folded into a box -- keeps the column. Whether inlining pays is the data's, which stage 06
  counts (`06_model_formulation/done.md`, "Inlining a helper").
- **Member rows.** The rows the ABS, Boolean-product and L0 rewrites add for a construct
  inside a reducer over decisions define its members: each rewrite walks with a
  `DecideReducerPath`, whose `innermost` names that reducer, and `MarkMemberRow` tags the
  row `MEMBER_ROW_TAG` and with the reducer's member frames (`CopyClauseProvenanceTags`
  carries both onto a row derived from a member row). An ABS's envelope reads the rows the
  path keeps (the clause's `when` and every enclosing reducer's own), where it read every
  row its clause keeps and so evaluated data the reducer's own `when` excludes; a Boolean
  product's rows read decisions alone and keep no `when`. The linear form makes each a
  `DecideConstraint::defines_members` row whose `when` joins its member frames' `PRESENT`
  columns: it is never skipped with its clause's instances, which skipped the members a
  reducer beside a root frame reads (phase 4's wrong answer for `sum(abs(x - 3)) +
  at(previous: x) over (t.period) <= 4`).
- **Absorption.** A clause that may skip an instance -- a frame at its root that may select
  nothing, or a helper it reads there that may be absent (`LogicalDecide::
  ClauseMaySkipInstances`) -- holds only where it does not, so `AbsorbBoundsInExpression`
  leaves it a row.

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
| `filter` | The reducer's own `when`. `sum(when a: x)` and `sum(when b: x)` are one column over two row sets |
| `avg_scale` | AVG divides by the group's row count; merging before that scaling applies the division to both |
| `qualifier_scope_idx` | Selects a de-duplication mask (the reducer's own `per`, or `SUM(D: ...)`) — again a statement about which rows contribute |
| `avg_group_key` | An inlined `sum(per K: avg(e) by (G))` divides each term by its G group's size, another AVG of the same decision by the relation's: merged, one divisor stood for both (`sum(per t.g: avg(x) by (t.g)) - avg(x)` cancelled to 0) |
| `frame_read` | A frame's payload term reads its decision at another row: `x - at(previous: x)` names one decision at two rows, and merged would cancel to 0 |

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
It is false when solver pressure or the clause's shape pins `aux` to `|inner|`
naturally — an objective term the sense pushes down (a positive ABS under
`MINIMIZE`, a negative one under `MAXIMIZE`), or a constraint shape that already
upper-bounds `aux`. When true (a constraint hard direction, an objective term the
sense pushes up, or a term of unknown sign), `abs_maximize_links` carries the
auxiliary and a binary sign indicator, and execution derives the two upper-bound
rows from the tagged lower-bound constraints.

`TagAbsForBigM` decides this by reading a constraint as `E <op> 0` and asking
whether each ABS term's signed position pushes its auxiliary down, and the
objective the same way with the sense standing in for the relation: `MINIMIZE E`
pins what `E <= K` pins, `MAXIMIZE E` what `E >= K` pins. So `MINIMIZE -abs(e)` and
`MAXIMIZE abs(e)` take the envelope, `MAXIMIZE -abs(e)` does not (until 2026-10-23 only
the sense was read, and `MINIMIZE 0 * sum(x) - abs(sum(x) - 9)` was "unbounded").
`CollectAbsWithSign` folds signs through `+`, unary and binary `-`, casts,
aggregate bodies, and constant factors. A factor whose value is not known until
execution — a data column, as in `SUM(w * ABS(x - t))` — yields sign 0, which
never matches the pinning direction and so forces Big-M, in the objective as in a
constraint: `MINIMIZE SUM(w * ABS(x - t))` with a negative `w` on one row answers
rather than reporting "unbounded", at the price of needing a finite box for the
decisions inside the ABS on the lowering arm. A factor's sign is read by
`PlanTimeSignOf` (`decide_cast_policy`), structurally rather than only by folding: a
product with a zero constant among its factors is zero whatever its data factors hold
(`0 * w`), a negation flips, a quotient follows its numerator. A factor that is exactly
zero makes the term vanish: nothing pins it and nothing boxes it, and neither is
anything nested in it. The MIN/MAX rewrites' `ScaleSignAtPlanTime` and
`ScaleVanishesAtPlanTime` read the same classifier, so every pass agrees on what a
factor does. A MIN or MAX on the path pushed its
hard way (`MINIMIZE min(abs(e))`, `MAXIMIZE -min(abs(e))`, `min(abs(e)) <= K`) also
yields sign 0 beneath it: the extremum is encoded with a Big-M over its members, which
needs each member's box, and an auxiliary held by pressure alone has none. Pushed its
cheap way (`MINIMIZE max(abs(e))`) the extremum's envelope bounds every member and the
plain envelope stands. A MIN or MAX under a factor that is exactly zero
(`ScaleVanishesAtPlanTime`) is classified easy in every direction by the composed
rewrite: the term vanishes, nothing pushes its extremum, and the cheap envelope asks for
no member box, so `sum(x) + 0 * min(abs(x - 2)) <= 5` and `MAXIMIZE 0 * -min(abs(x - 2))`
answer instead of demanding a bound on the ABS auxiliary.

The two envelope rows keep the clause's `when` (`ScopeHelperRow`), so they read only the
rows the clause keeps; the objective's keep the objective's `when`.

**The auxiliary's scope.** An auxiliary has the scope its term had when the clause's shape
was decided. Beside a clause's reducers, an ABS whose argument holds one value for the
query (`abs(s - 1)` over a query-wide `s`, `DecideCanonicalizer::IsQueryWideExpression`,
the test `ClassifyScoped` puts such a term in the aggregate row by) gets one query-wide
auxiliary, and so does its sign indicator; any other ABS, a reducer's member included,
gets one per row. A per-row auxiliary there turned the aggregate row into the shape of a
value rewrite after that rewrite had run, and the clause failed internally (A07).

**An ABS inside an ABS.** `FindAndReplaceAbs` rewrites the argument before the node, so
an inner ABS becomes its own auxiliary first and the outer's envelope rows refer to it
(an ABS left in a row was read by term extraction as its argument, and the sign
vanished). The pairs are appended innermost first, the order `DeriveAbsAuxiliaryBounds`
boxes them in: the outer range needs the inner box. An ABS beneath another is pinned
exactly, because the outer ABS is not monotone in it: over `x` in `[-1, 1]`,
`abs(abs(x) - 2) <= 0` with an inner auxiliary held by its lower envelope alone would hold
at 2. `TagAbsForBigM` is the one place that decides this, as it decides every other
pinning: `CollectAbsWithSign` records an ABS and then descends into its argument with
sign 0, so a nested ABS never classifies as pinned, and `TagAbsSubtreeForBigM` tags the
nested ones under a BETWEEN, IN or equality too. The rewrite only reads the tag. So a
dead ABS -- under a factor of exactly zero, which the walk skips -- and everything nested
in it get no envelope and demand no box (`MINIMIZE sum(0 * -abs(abs(x) - 2))` answers with
no upper bound on `x`). The label is rendered before the argument is rewritten, so each
account names the ABS as written.

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

The binder keeps `norm(...)` as an aggregate-shaped DECIDE marker, so it takes a
`by (G)` and reads the rows its clause's `when` keeps as any reducer does. The optimizer lowers
L1, L2, and infinity norms to `SUM(ABS)`, `SUM(POWER(_, 2))`, and `MAX(ABS)`;
L0 emits its existing Boolean indicator and exact forward/reverse links.

**A marker is lowered wherever it stands.** `RewriteNorm` descends through every
expression container, not only comparisons and conjunctions, because a norm is a
scalar and a regularizer is normally written as one term of a larger expression —
`MINIMIZE SUM(cost*x) + 0.5 * norm(x - base, 1)`, `2 * norm(e, 1) <= K`. The
descent is `ExpressionIterator::EnumerateChildren`, the same generic recursion
`RewriteAbs` and `RewriteAvgToSum` use. A marker cannot sit beneath another
aggregate, because the binder rejects `SUM(norm(...))` before this pass runs.

**The marker must not outlive this layer**, and `VerifyNormMarkersLowered` asserts
that over `LogicalDecide::EnumerateExpressions` at the end of `OptimizeDecide` —
after the composed MIN/MAX pass, which lifts sub-expressions out of the trees into
its own vectors. This assertion is not defence in depth. The marker is a real `SUM`
aggregate with the order recorded in its alias, so an unlowered one is not
recognisably broken downstream: extraction reads it as the `SUM(e)` it is built on,
the norm silently leaves the model, and the query returns a confident wrong answer.
Until 2026-09-08 a marker under arithmetic did exactly that.

A bound `x IN (...)` stays a native `COMPARE_IN` marker until this pass. It then
uses the singleton, Boolean-domain, or cardinality/linking formulation. The two size
optimisations that need to read the domain values — the `{0,1}` Boolean shortcut
and the `x >= min(domain)` tightening on the general encoding — evaluate each
value with the cast policy's folder, so a written-out expression such as
`x IN (1-3, 2)` qualifies exactly like the literal `x IN (-2, 2)` does. Neither
affects correctness: the indicator encoding is exact with or without them. Every row
the IN generates keeps the clause's `when`, as an L0 norm's links do (`EnterClauseScope` /
`ScopeHelperRow` in `decide_optimizer.cpp`), so a row the `when` drops is never read; under
a `per K:` the rows are emitted once per kept row, not per class (`todo.md`). They are
structural (`MarkFormulationConstraint`): a repair never loosens one on its own, and a
DROP removes them with their clause, whose `removal_group_id` they carry.

### Bilinear — `RewriteBilinear`

| Operand types | Formulation |
|---|---|
| Boolean × Boolean | AND-linearization: `w <= b1`, `w <= b2`, `w >= b1 + b2 - 1` |
| Boolean × other | Partial McCormick: `w <= x` at plan time, `w <= U·b` and `w >= x - U·(1-b)` at execution time via `BilinearLink` (needs runtime bounds) |
| Non-Boolean × non-Boolean | Left in place for the Q-matrix path in the physical operator |

`ExtractMultiplicativeCoefficient` walks a chain shaped
`coeff * ... * decide_var * ... * coeff` and combines the non-variable factors. Each
factor must be such a chain: the product becomes `coefficient * w`, so a factor that is no
multiple of its one decision is refused (*"DECIDE: 'b * (y - 1)' multiplies a BOOL
decision by an expression that is not a multiple of one decision, which is not supported
yet; write the product out term by term, as b * y - b for b * (y - 1)"*). It was read as
the decision alone, dropping the `- b`, and answered with the clause violated.

### The value rewrite — `RewriteScopedReducers`

A comparison whose decision side one model row per `by` group cannot hold is in the
**VALUE shape**: a per-row or keyed decision beside a reducer (deck example 1,
`ship <= 0.20 * sum(ship)`), a factor on a reducer that varies by row, reducers with
different `by` keys, a reducer inside another expression. Stage 04 decides the shape once
(`CanonicalConstraintClass::VALUE`, `VALUE_REWRITE_TAG`, stage 04 §3.10); this pass reads
the tag and turns the clause into an ordinary per-row one, so stage 06 needs no new
constraint path. It runs after the ABS and bilinear rewrites, whose auxiliaries may stand
in a reducer's body, and before the MIN/MAX passes, which never see these reducers.

For each decision reducer `agg([when c'] [per K']: e) by (G)` on the comparison's left
side (`CollectReducerSites`; a data-only reducer is a number and stays) the pass:

1. replaces every reducer inside its argument first, the same way, innermost first
   (`syntax_reference.md` §5.5): each one's path is this reducer's with its own `when` and `by` added
   (`DecideReducerPath`, `planner/decide/decide_reducer_path.hpp`, which stage 04 reads
   for the D8 checks too), so the argument is affine in helpers, decisions and data;
2. allocates a **helper decision** `__value_<n>__` through `AddAuxiliaryDecision`. It is
   INTEGER when the reducer's value is whole by declared types, every constant inside
   counted (`DecideValueIsWholeNumbered`, shared with stage 02), so a strict bound on the
   rewritten row still steps on the lattice; else DOUBLE. It is query-wide
   (`DecideVarScopeInfo::Scalar()`) when its **path key** -- every enclosing `by` key and
   its own -- is `by ()` throughout, else keyed by a scope of its own
   (`EntityScopeInfo::value_link`), whose classes are the path key's groups among the
   rows its path keeps -- the clause's `when` and every enclosing reducer's -- built by
   stage 08. A path key that is a new union of keys is a `group_keys` entry the pass adds
   (`FindOrCreateDecidePathKey`); a reducer at the top reads its own `by` key and the
   clause's `when`. Its label is the reducer's SQL, `by` included (`aux_var_expressions`,
   `ReducerValueLink::label`), so EXPLAIN, DIAGNOSE and a refusal's remedy name it by what
   it holds;
3. records a `LogicalDecide::ReducerValueLink`: the reducer kind, its body (its nested
   reducers helpers by now), its own `when` and `per`, its path key and path rows, the
   clause id and removal group;
4. replaces the reducer node by a reference to the helper, in place, as `RewriteAbs` does.

The comparison then holds no decision reducer and the tag is removed: it is one row per
kept row, or per class under `per K:`. Stage 06 states each helper's definition per group,
an equality for SUM/AVG and an extremum link for MIN/MAX
([`../06_model_formulation/done.md`](../06_model_formulation/done.md), "Helper decisions of
the value rewrite"). Deck example 1 costs N rows and one helper, rather than N rows of N
terms. The user-facing account of the three shapes is
[`syntax_reference.md`](../../00_project_overview/syntax_reference.md) §6.4.

A strict `<`, `>` or a `<>` steps onto the whole numbers, which is exact for the rewritten
row only when the helper is whole, so over a helper that can be fractional it is refused
here, with the binder's message (*"Comparison '>' is not supported here: 'sum(ship + 0.5)'
can take fractional values in this constraint ..."*). The aggregate shape accepts the same
reducer, because there its constant moves to the bound when the reducer is summed in place
(`sum(x + 0.5) > 4` is `sum(x) > 4 - 0.5 * n`).

One helper per **occurrence** of a reducer in a clause: a second site holding the same
reducer (`Equals`, alias included) under the same clause id, path rows and path key
reuses the first helper (`Helpers::Find`). The rows `RewriteAbs` derived from one clause each hold a copy
of the reducer (`aux >= sum(e) by (g) - 40`, `aux >= 40 - sum(e) by (g)`), and two
helpers pinned on different sides would let the pair drift apart; rows of different
clauses never share one, since the definition carries one clause's provenance.

A MIN/MAX helper records which side pins it (`need_envelope` / `need_closing`), from the
sign its term carries once the side is read as a sum — through `+`, `-`, binder casts and
the factors on it (`ScaleSignAtPlanTime`; 0 for a data factor, a decision factor or an
enclosing expression) — and the comparison, read as an `ExtremumPush`: a term pushed
down needs a MAX's envelope (`value >= member`) or a MIN's closing side (`value >= some
member`, with indicators), a term pushed up the other one, and an unknown sign, `=` or
`<>` both. A shared helper takes every side any of its rows needs. A SUM/AVG helper is an
equality and needs neither. A nested MIN/MAX is pushed as the SUM or AVG around it is,
through the sign it carries in that reducer's argument; under a MIN or MAX it is pinned
from both sides (D73), which is exact whatever reads it.

**The objective** (D75). Stage 04 tags each objective reducer over decisions that holds a
reducer (`VALUE_REWRITE_TAG` on the reducer). The pass walks the objective's sites as a
clause's, with `OBJECTIVE_ROW_TAG`'s provenance and the sense standing in for the
relation (MAXIMIZE pushes a positive term up), and makes each tagged reducer and every
reducer inside it a helper; the objective then reads the outer helper once, as any
decision outside its reducers, and is reinstalled through `SetObjective`. One shape is
written directly instead (`InlineSumOfGroupSums`): `sum([when c] per K: inner([when c']
e) by (G))`, `inner` a SUM or AVG with no `per` and no reducer inside, every column of
`K` in `G` -- each `G` group one member -- is `inner` with `c` joined to `c'`, the
objective summing `e` itself, or averaging it per `G` group (its `by` stays, which
`avg_group_key` reads). That is what keeps `sum(per g: sum(power(x - t, 2)) by (g))` a
convex quadratic objective: a helper's definition must be linear.

A body that still multiplies decisions after `RewriteBilinear` — a product of two
non-Boolean decisions or a square — is refused here with "not supported yet"
([`syntax_reference.md`](../../00_project_overview/syntax_reference.md) §10.2): its
definition would be a quadratic equality. Among nested reducers the message says so
(*"... squares or multiplies decisions inside nested reducers, which is not supported
yet"*, D74), the objective's direct sums aside. The test is
on what survives the ABS and bilinear rewrites, whose auxiliaries are linear.

Every auxiliary decision column the passes allocate — an ABS auxiliary and its sign
indicator, a McCormick product, an L0 and an `IN` indicator, a value helper — goes
through `AddAuxiliaryDecision` (`decide_optimizer_internal.hpp`): the column reference,
its Boolean flag, its scope (recorded only once some decision is not row-scoped, the
convention every reader follows) and its label (F6). The label is the expression the
column stands for, rendered as a plan prints it (`RenderForPlan`, which sees the labels
of the auxiliaries earlier passes made): `abs(e)`,
`(e >= 0)` for the sign indicator, `(b * x)`, `(e <> 0)` for an L0 indicator, `(x = v)`
for an `IN` indicator, `sum(e) by (g)` for a helper. Each rewrite also records what it
made of a construct as a fact (`AddRewriteAccount` → `LogicalDecide::rewrite_accounts`:
the subject as the plan prints it and a `RewriteAccount::Kind`, under the clause id or
the objective): the AVG scaled into a sum, the `norm` lowerings (a sum of absolute
values, a sum of squares, a maximum of absolute values), the ABS auxiliary and how it is
pinned (solver pressure, a sign indicator, or the native absolute value), the Boolean
product (AND rows or McCormick), the `IN` and L0 indicators, the value helper (per group
or query-wide), a MIN/MAX in its cheap direction (a bound on each member), the extremum
column of a hard MIN/MAX, of a composed term and of an objective's level (pinned by
indicator rows or not), and the `<>` disjunction. The renderer words them in one place
(`RenderDecideRewriteAccount`, `03_logical_plan/done.md`). The passes start the
objective's walk with `OBJECTIVE_ROW_TAG` as its source alias, which
`CopyClauseProvenanceTags` carries onto every row they add for it, so EXPLAIN can print
those rows under the objective.

### MIN/MAX — `RewriteMinMax`

**Constraints.** An *easy* direction (`MAX(...) <= K`, `MIN(...) >= K`) holds when every
member does, so it needs no Big-M: the comparison keeps its reducer and is tagged
`MINMAX_EASY_REWRITE_TAG`, and the linear form emits one row per member. The reducer's
`when` picks the members, while a bound-side reducer is reduced over every kept row of the
`by` group; hoisting the `when` onto the clause would reduce the bound over the members
only. A `max(x) <= K` over a bare decision with no `when` and no reducer filter is then a
column bound, which bound absorption folds into the box (below). A *hard* direction
rewrites to a `SUM` tagged with the clause (`EmitHardMinMaxClause`), which keeps the
reducer's `by`; stage 06 creates the extremum column and any binaries its link needs.
Equality splits into both: the easy half is a copy of the comparison under the clause's
`when` and `per K:`, the hard half the rebuilt `SUM`. Which way a clause pushes a term —
down under `<=` for a positive term, up for a negative one, the mirror under `>=`, nowhere
under `=` or an unknown sign — is read once, in `ExtremumPushOf` (`decide_cast_policy.hpp`),
by this pass, the composed pass and the value rewrite alike; MAX pushed down and MIN pushed
up are the cheap directions. A factor of unknown sign leaves the clause to the composed
pass, which claims it first. `<>` on a MIN/MAX is refused.

**A square or product of decisions inside a MIN or MAX** (what the ABS and bilinear
rewrites leave nonlinear) is refused wherever its members would be stated as linear
rows -- the hard half of a constraint, either half of an `=`, a composed clause's terms
(the SUM beside the MIN or MAX too), a MIN/MAX objective (`RefuseNonlinearReducer`) --
since each would drop the square: *"DECIDE: 'max(power(x, 2))' squares or multiplies
decisions inside MIN or MAX, which is not supported yet; it is accepted only as a bound
every member meets on its own: max(...) <= K or min(...) >= K"*, and *"... beside
another term"*, *"... beside MIN or MAX"*, *"... in the objective"*. The easy direction
stays: each member's row is a quadratic row (`power(x, 2) <= 16`), exact on Gurobi.

Stage 06 takes the extremum the hard half names over its members' coefficients and states
the clause's own relation over it, so a negative factor goes inside the hard half's
reducer (`build_hard_lhs`): the hard `-2 * MAX(e) <= K` is stated as `MIN(-2 * e) <= K`.
A positive one stays outside.

**Objectives.** A MIN/MAX objective is detected and the easy/hard classification is
precomputed into typed metadata — `flat_objective_agg` / `flat_objective_is_easy` — so
the physical layer reads the decision rather than re-deriving it. A nested one is a
helper decision by now (the value rewrite, D75). `RewriteMinMaxObjectiveTree` is split out from
`RewriteMinMaxObjective` because the rewrite writes through a pointer that walks *into*
the tree past the `when` wrapper and casts, and has early returns: the objective must be
detached, rewritten, and reinstalled through `SetObjective`.

### Composed MIN/MAX — `RewriteComposedMinMax`

An additive LHS mixing `SUM`/`AVG` with `MIN`/`MAX` terms cannot go through the
single-term path. Each term's metadata is extracted into
`composed_minmax_constraints` (or `composed_minmax_objective_terms`) as a
`ComposedMinMaxTerm`, and the comparison is replaced with a no-op `TRUE`
placeholder so the physical layer owns emission. Non-comparison leaves are legal
under C0 precisely so that placeholder verifies.

`ScopeComposedTerms` folds the clause's `when` into each term's filter, in front of the
term's own, so stage 08 reads only the rows the clause keeps; under a `per K:` the
clause's one row names the key's classes (`per_key` `NAMES_CLASSES`, `class_key`, D28).
Stage 06 steps a strict outer bound onto the lattice. The canonicalizer has refused such a
clause with a `by` or a bound that is not a constant (stage 04 §3.10); `AssertComposable`
asserts it. A decision beside a MIN or MAX is refused here, for the constraint and the
objective alike, quoting it (*"DECIDE: 's' beside MIN or MAX is not supported yet"*).

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

The continuous auxiliaries and the family each is boxed by:

| Auxiliary | Reduces over | Box |
| --- | --- | --- |
| flat `z` | the objective's rows | per-row family |
| `z_k` (composed term) | rows the term's filter admits | that term's own family |

A value helper, nested or not (a nested objective included), is a decision column, boxed
by stage 06 from its members' reach and from the box of the helper it stands in
(`DeriveReducerValueBounds`, `DeriveReducerMemberBounds`,
`../06_model_formulation/done.md`).

> Fixed 2026-08-18. All five sites of the time had been declaring their auxiliary
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

Each `COMPARE_NOTEQUAL` is marked, not rewritten: its clause index rides the tag
(`NE_CLAUSE_TAG_PREFIX`) and its label, the clause as the plan prints it
(`sum(ship) by (D.region) <> 20`), goes to `ne_clause_labels`. Stage 06 allocates the
binaries and emits the disjunction rows once bounds are known. A hard MIN/MAX's label in
`minmax_clause_labels` is spelled the same way (`max(ship)`).

The two rows a `<>` becomes hold a linear left side. The pass runs after
`RewriteBilinear`, so a product with a `BOOL` decision is already an auxiliary; a square
or a product of decisions still on the left (`DecideExpressionDegree` above 1) has no
formulation and is refused here (*"DECIDE: <> over a product or a power of decisions is
not supported yet: power(x, 2) <> 9"*). `LinearizeNotEqual` (stage 06) throws an
`InternalException` if one reaches it.

### AVG → SUM — `RewriteAvgToSum`

Replaces a decision-bearing `AVG` with `SUM`, tagged `AVG_REWRITE_TAG` so
coefficient evaluation divides the terms by the number of rows the reducer reads in its
group: the kept rows of its `by` group, less those its own `when` drops, one per class
under its own `per`. A **decision-free** `AVG` is
skipped: there is nothing to linearize, and rebinding it as `SUM` would redeclare
it with `SUM`'s integral type while its value stays fractional. Keeping the tag on
objective terms is what makes a mixed `AVG(a) + SUM(b)` preserve true AVG
semantics.

### Bound absorption — `AbsorbVariableBounds`

`x <= 10` is one fact about a column, so it belongs in that column's box rather than
in `num_rows` identical model rows. A box is also strictly better for the solver:
smaller model, tighter presolve. The pass folds every simple `x OP const` (a `BETWEEN`
arrives as its two comparisons, split by the binder) into `absorbed_lower_bounds` /
`absorbed_upper_bounds` on `LogicalDecide`, and tags the comparison `ABSORBED_BOUND_TAG`
so physical extraction skips it.

A bound is read by value, not by spelling. It qualifies when the LHS is one decision
variable under casts — not inside a reducer — in any affine spelling whose constants
are known before the query runs (`2 * x`, `x * 2`, `-x`, `x / 2`, `2 * (x - 1)`:
`PeelAffine` reads the side as `scale * x + offset`), and the RHS is anything that folds
to a finite number then: a literal under casts, or the `1 + -3` canonicalization leaves
of `x - 1 >= -3`. The shift moves across, the factor divides the bound and, when
negative, reverses the relation, so `x - 1 >= -3`, `2 * x >= -4`, `-x <= 2`,
`2 * (x - 1) >= -6` and `x >= -2` are one fact, the column bound `x >= -2`, and each
makes `x` signed the same way (D41 by value, 2026-10-23; before that only a literal RHS
qualified and `x - 1 >= -3` kept `x >= 0`). A side that is data — a column, a subquery,
a column factor — is not a bound known before the query runs: the comparison stays a
row.
Recursion goes through `AND` and a `per K:` wrapper, whose key children hold no
comparison, so `per row: x >= -5` is absorbed like `x >= -5`. **A `when`-guarded
comparison is never absorbed**: `when c: x <= 0` is conditional per row, not a domain,
so the pass stops at the wrapper. This is why it keeps its own descent rather than
`ForEachConstraintLeaf`. Nor is a comparison of a clause that reads, outside its reducers,
a frame that may select no position (`syntax_reference.md` §5.6): it holds only where the
frame selects, a condition as a `when` is. The row an ABS lowers from `abs(x - at(previous:
x) over (...)) <= -1` is the case: absorbed, its empty box called the query infeasible even
where every instance is skipped and none holds it.

An easy `max(x) <= K` / `min(x) >= K` over one decision, with no filter on the reducer
(`TryMatchEasyExtremumTarget`, `MINMAX_EASY_REWRITE_TAG`), bounds every row its reducer
reads, and with no `when` that is every row: it is the column bound `x <= K` / `x >= K`
and is absorbed the same way, in the same affine spellings outside the reducer
(`2 * min(x) >= -4`, `-min(x) <= 2`, `2 * (min(x) - 1) >= -6`), so a negative `K` makes
`x` signed, as `x >= -5` does. The member itself is a bare decision: `max(2 * x) <= K`
bounds the member `2 * x` and stays one row per member. The reducer stays in the tree,
so EXPLAIN and DIAGNOSE read the clause as written.

- `<=` tightens the upper bound (`min`), `>=` the lower (`max`), `=` intersects both.
  Intersecting rather than assigning is what makes `x = 5 AND x = 10` invert the box
  and be caught, instead of silently resolving to whichever was written last.
- Strict `<` / `>` on an integer step onto the lattice as every row does, carrying the
  user's typed literal so the diagnosis re-quotes `< 10` rather than the normalized
  `<= 9`: `x > 2.5` is `x >= 3` (`floor(k) + 1`), `x < 2.5` is `x <= 2` (`ceil(k) - 1`).
- Strict `<` / `>` on a REAL variable are deliberately **not** absorbed. Stage 02
  refuses them (`ValidateDecideNoIntegerStepComparisonOnReal`); the pass declines rather
  than step a bound off the lattice, so one reaching it would stay a row and trip the
  model builder's invariant check (`ApplyComparisonSense`).
- A non-finite constant is **not** absorbed either: the box has one sentinel per
  direction and cannot hold it.

Lower bounds start at `ABSORBED_LOWER_UNSET` (-1e30) rather than 0, so an explicit
negative bound is honored instead of clamped up. Stage 08 resolves anything still at
the sentinel to the default 0 floor.

**A recorded bound is flattened aside too.** The box is how a written negative bound
makes a decision signed, so every form of a bound reaches it. But a clause is one knob
per kept row or class in an expanded diagnosis (D28), which one column bound cannot say.
So the comparison of every recorded bound (not a BOOLEAN restatement, below) is also
tagged `ABSORBED_CLAUSE_ROWS_TAG`, and the linear form flattens it into
`DecidePreparedModel::absorbed_constraints` instead of skipping it, throwing an
`InternalException` if their count differs from `user_absorbed_bounds` (the repair model
would open the column with nothing enforcing it). Stage 08 evaluates those under DIAGNOSE
only and hands them to the repair model alone: they are its only rows for an absorbed
bound.

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

Each recorded bound is a `UserBoundSpec` in `user_absorbed_bounds`, with its clause ids
and, for a strict one, the typed literal: stage 08 re-opens its direction in the rigid box
and in the elastic box, and quotes its clause when it contradicts the variable's domain. A
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
(`2 * SUM(x*p)`, `2 * MAX(x*v)`) and converges every spelling onto one. A decision cannot
be that factor: stage 04 refuses `b * SUM(x)` (*"'b' is a decision, so it cannot multiply
SUM(x)"*). **No pass here pushes a factor back in**, except into the hard half of a
MIN/MAX, whose factor's sign is known (§2, MIN/MAX).

`MIN`/`MAX` are order statistics, so a negative factor turns one into the other —
`MAX(-2x)` is `-2·MIN(x)`. Folding therefore needs the factor's sign, which an
uncorrelated scalar subquery does not supply until the query runs. Kept outside, a
known sign picks the easy or the hard form, and in the hard form which extremum the link
takes: the hard half takes a negative factor inside (§2). Only an unknown sign is safe
outside, because the composed path pins both sides. `ScaleSignAtPlanTime`
(`decide_cast_policy`, shared with the canonicalizer) returns +1, -1, or **0 for "not
known until the query runs"**, and 0 must be treated as "take the expensive but exact
form", never as an error. A scale of -2 flips the direction `z` is pushed exactly as a
subtraction would, so a known sign participates in `is_easy` alongside `sign`; an
unknown sign forces `is_easy` false, and the indicator layer pins `z` to the true
MIN/MAX in both directions — correct whichever sign it turns out to have.

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
- **`when` filtering** — stage 08. Nothing is computed on a row a `when` drops: a
  reducer reads only the rows its clause's `when` and its own keep, a per-row clause
  emits no row there, and the objective's `when` folds into every term's filter. A
  dropped row gets `INVALID_INDEX` in `row_group_ids`, so it falls out of every group.

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
| The value rewrite (VALUE-shape reducers to helper decisions) | `src/optimizer/decide/decide_rewrite_scoped.cpp` |
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
| `per`, `when` and `by` semantics | `../../03_expressivity/scope/done.md` |
| Bilinear semantics | `../../03_expressivity/bilinear/done.md` |
