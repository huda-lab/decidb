# Stage 05 — DECIDE optimizer

Chooses the **mathematical formulation** for every construct the solver cannot take
literally: ABS, MIN/MAX, AVG, `<>`, bilinear products, and nested reducers, and
then **lowers the finished clauses into the tree's terms** (stage 08's `DecideTree`), which execution
consumes. It assumes canonical input and produces canonical output; it never
decides shape, parses SQL, executes relations, or calls a solver.

**Key source files**

- `src/optimizer/decide/decide_optimizer.cpp` — the pass dispatcher
  and the helpers the passes share; each pass body lives in a sibling file:
  `decide_rewrite_norm_in.cpp`, `decide_rewrite_notequal_avg.cpp`,
  `decide_rewrite_abs.cpp`, `decide_rewrite_minmax.cpp`,
  `decide_rewrite_bilinear.cpp`, `decide_rewrite_scoped.cpp` (the value rewrite),
  `decide_singleton_row_folding.cpp`
- `src/optimizer/decide/decide_linear_form.cpp` — the flattening
- `src/include/duckdb/optimizer/decide/decide_optimizer.hpp`
- `src/include/duckdb/optimizer/decide/decide_optimizer_internal.hpp` — helpers
  shared across the pass files
- `src/include/duckdb/optimizer/decide/decide_linear_form.hpp`
- `src/include/duckdb/planner/decide/decide_tree_terms.hpp` — the term records, held in the tree's nodes

**A pass marks what it changes, and the body walk canonicalizes it** (Template Method, as DuckDB's rules report
`changes_made`). A rewrite walks the bodies with `LogicalDecide::RewriteBodies` and sets `DecideOwnedBody::changed`
where it replaces part of a clause's body or of the objective's; when the walk is over, each marked clause goes back
through `LogicalDecide::CanonicalizeClause` and a marked objective through `LogicalDecide::SetObjective`, which
re-canonicalize and re-verify that one record and give it its shape. A clause a pass adds enters through
`LogicalDecide::AddClause`. The canonicalizer's refusals are raised there, at the pass that made the shape:
`RewriteRhsAggregates` replaces a data reducer by a column of its value, and a factor that varies per row on an
objective reducer is refused when its walk ends. `RewriteNorm`, `RewriteScopedReducers` and `RewriteFrameRanges` mark
the same way. A pass that only names a definition on a kept call or sets a clause fact (a product's or an ABS's kept
call, the relation of an easy extremum's other half, a folded bound's mark) replaces nothing and marks nothing.
`DefineReducerBodies` names its definitions after its walk, so it reinstalls the objective itself. A debug build
verifies every clause when the plan is made (`VerifyCanonical`).

---

## 0. The solver — and the formulation — are chosen before anything is rewritten

`OptimizeDecide` opens by calling `ChooseDecideSolver` (`decide_solver_gate.cpp`),
which settles two things and records both on the plan:

| Recorded | What it is |
|---|---|
| `LogicalDecide::solver_backend_name` | *Which* backend, from `SelectSolverBackend()`. A name, not a handle — see [`../03_logical_plan/done.md`](../03_logical_plan/done.md) §4. |
| `LogicalDecide::use_native_constructs` | *Which constructs the backend can state itself*, read once off that backend's `SolverConstructSupport`. |
| `LogicalDecide::force_native_constructs` | *The policy* governing them: a declared construct is used unless its lowering is measured faster. False ships; true is the test-only `DECIDB_NATIVE_CONSTRUCTS=force`, which states every declared construct natively. |

All three happen here, ahead of every pass, and the rest are the reason for the first.
The passes below decide **how to express** a construct, and the right answer depends
on what the backend accepts natively. A solver with a native `ABS` states it without a Big-M, and the rule below says
when stage 06 takes that over the lowering.

**Choosing a formulation is this stage's job, and it is settled here rather than
where the rows are built.** Stage 08 reads the fields above and translates; it does not
read `Constructs()` itself. Deciding here keeps the layer boundary and lets a rewrite
*see* the choice. Seeing it matters where a rewrite must refuse a query that no form of the backend can state: a
non-linear body's MIN/MAX on a backend with no native MIN/MAX.

**Capability is not the same question as policy, and MIN/MAX is where they come
apart.** A general constraint and a Big-M family encode the same thing, so where both
are available the choice is a performance one — and, measured, the lowering wins it.
A general constraint relates *columns*, so every member expression that is not already
a column has to be pinned to a fresh one that presolve cannot substitute away; and
`z = MAX(t..)` is an *equality*, so the backend expands both directions while the
lowering emits only the one the clause needs. Measured on the Q9 benchmark shape at 30K
rows, the native arm is 1.8x slower; on a `MAX(e) >= K` constraint at the same size, 41x,
and its cost grows with row count while the lowering stays flat. Neither arm branches, so
there is no search quality to buy back.

So for MIN/MAX the lowering is taken wherever a Big-M exists, and native is reserved for the case with no valid
Big-M, which is the case it was built to answer and the only one where the lowering must refuse the query outright.
Whether a given clause has a Big-M depends on evaluated coefficients, so stage 08 answers *that* — it applies this
policy to data only it can see, exactly as the `<>` range collapse does. It does not re-decide the policy.

**One rule serves every construct (`NativeConstructPolicy`): native is the default, and a declared construct is lowered
only where its lowering is measured faster.** MIN/MAX is lowered by the measurement above. An exact ABS is not: on
Gurobi on Q10 (200K rows) the lowering ran 12.0-12.2 s against 12.2-12.3 s native with the same objective, a tie, so
there is no gain to give up the native construct for. Where no finite Big-M exists, native is the only form of either
construct, and a backend without the native construct refuses that case.

One consequence is recorded where it lands: **stage 05 allocates no indicator variable
for a MIN/MAX, an ABS or a `<>` at all.** A stage that cannot see the data cannot know which form a
clause takes, so allocating here would give the native form a binary per data row that
nothing references. Each construct allocates its binaries in the pass that emits the
rows reading them, in the global block. A MIN/MAX is a helper decision here and stage 06 emits its extremum link
(`EmitExtremumLink`); an ABS is a record here and stage 06 emits its rows (`EmitAbsEnvelopes`, `EmitAbsConstraints`);
`RewriteNotEqual` records only the **account** (`AddRewriteAccount`): the clause's `source_clause_id` and the text
to call it in a diagnosis.

The choice is made **once** and rides the plan: `LogicalDecide` → `PhysicalDecide` →
the solve → every diagnostic re-solve. Nothing downstream selects again — stage 08
applies the recorded policy to the data, which is a different act from choosing it. Once a rewrite has consulted the backend's capabilities, a second selection
that answered differently would run a model on a solver it was not built for.

None of the three is **serialized**. Which solver a host has is a fact about the host,
not about the query, so a plan deserialized elsewhere re-resolves both rather than
carrying a choice that machine cannot honor.

This stage is required, not an optional speedup. It chooses the backend, reduces each
data bound to its group's tightest value with its value checks, and folds bounds into
column boxes. A plan it never saw (`SET disabled_optimizers='decide_optimizer'`,
`PRAGMA disable_optimizer`, or the unoptimized run of `PRAGMA enable_verification`) is
refused at physical planning (`plan_decide.cpp`): *DECIDE needs the optimizer 'decide_optimizer', which is
disabled* (Invalid Input Error, no caret). So `ChooseDecideSolver` runs exactly once per plan, and the operator
always runs against one backend and one formulation.

See [`../07_solver/done.md`](../07_solver/done.md) §2 for what selection reads, and
`SolverConstructSupport` / `SolverModelClass` for what a pass may ask about the
backend.

### The model-class gate

`RequireDecideSolverSupport` (`decide_solver_gate.cpp`) runs once the prepared linear
form exists — right after the lowering walk (`DecideTreeConstruction`), the first point at which every
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
  semantic one, and the message says what the query does and that this installation does not support it, naming no
  solver: `squaring or multiplying decisions in a constraint is not supported on this installation`
  (`ThrowDecideSolverGap`, one text; `../../language/errors.md` § 3). The installation is the place, so a host with the
  solver answers the same query. The caret is on the clause: `FirstQuadraticClauseLocation` reads the span of the first
  clause with a product group, and asserts the query has one. `DECIDB_FORCE_SOLVER` gives the same text, since the
  forced solver is the installation.
- **Not model load.** A refusal there would fire after a full scan and a full model
  build, and nothing about the four classes needs a single row: `quadratic_constraints` is tree shape (a constraint with a product group), `miqp` is declared
  integrality, `nonconvex_quadratic` is judged per product group of the objective
  (`IsNonconvexProductGroup(square, weight_sign)`) and `singular_quadratic` is whether a group couples
  two decisions. A product of two forms is always non-convex. The objective is a minimization, so a square is
  non-convex when the sign of its weight there is negative (a positive weight written under `MAXIMIZE`, a negative
  one under `MINIMIZE`) or is not known before the
  query runs. The plan knows a constant weight's sign (`ScaleSignAtPlanTime`) and not a data weight's, which counts as
  0. So `MINIMIZE SUM(w * POWER(x, 2))` is refused on HiGHS: *"a square scaled by a value whose sign is known only when
  the query runs is not supported on this installation"*, at the objective. A group couples two decisions when it is a product, or a square of a form over two or more
  decisions.

`ClassifyDecideModel(const DecideModelShape &)` (`decide_solver_capabilities.hpp`) is the one classifier of a model's
shape into the class a backend must take. `DecideModelShape::AddTerm(objective, square, weight_sign, decisions)` fills
it, and two readers do: the gate from the prepared groups (`ReadPreparedModelShape`, which `DeriveDecideModelClass`
classifies), and the built model (`SolverModel::ModelClass`) from its matrix. An integral column and a coupled Q
matter only beside a quadratic objective: a MILP with quadratic rows is QCQP, not MIQP.

`DeriveDecideModelClass` is a *prediction* of what `SolverModel::ModelClass()` will
report once the rows are in, and it must never predict less. Where it cannot be exact
it over-reports. `MayHaveIntegralColumn` reads every helper this stage adds by its own type, as a declared decision
is read, and the three lowerings that add a binary during execution off what this stage leaves: a `<>` is admitted
over whole-numbered decisions only, a MIN/MAX held by indicator rows leaves a value link, and an exact ABS leaves its
record (an `ABS` entry of `LogicalDecide::helper_definitions`). An exact ABS is lowered only where the backend states no ABS natively, which
stage 06 decides, so on a backend that does the check over-reports. An ABS that is not exact adds only its envelope, so
`SUM(ABS(...))` beside `SUM(POWER(...))` under `MINIMIZE` leaves the QP continuous, and HiGHS takes it.
A square of a form over two decisions counts as coupled whatever its coefficients turn out to be, and a product counts
as coupled without checking whether the built Q ends up with a nonzero off-diagonal. Direct solve's reader errs the
other way: it reports less when unsure, skipping terms under MIN and MAX and nested parts and reading integral columns
from declared domains only. `SolveModel` re-derives the class from
the built model and asserts the backend covers it, so a prediction that ever comes up
short fails loudly instead of handing a backend a model it cannot load.

---

## 1. Pass order

Two steps run on each `LogicalDecide` before these passes (`OptimizeSubtree`). First,
`RewriteRhsAggregates` (`decide_rewrite_rhs_bounds.cpp`) replaces each data-only reducer on
a bound's right side (`sum(x) <= sum(cap)`) with a placeholder column and records it on the
node (`BoundDecide::bound_aggregates`; equal reducers share one). The path that answers the
node builds each record as a side table of the source (`DirectAddBoundTables`, on both paths):
the same DuckDB aggregate over another read of the source, grouped by the reducer's `by` key
(under the key's collation, D69) over the rows the clause's `when` keeps, and joined back to
every read by the key. It reads its argument as that SQL aggregate does: a NULL is
skipped, and only a NaN is checked (`COUNT` counts one). A reducer nested in a bound's
reducer (`avg(per t.h: max(t.v) by (t.g, t.h)) by (t.g)`, guide §5.5) is recorded first,
grouped by its path key over the rows its path keeps (`DecideReducerPath`), NULL on a row
in no group of it; the table built after it reads its column, so the reducer around it
skips that member as a NULL. A reducer over data alone
on a clause's left side or in the objective, inside one over decisions (`sum(x - avg(t.v) by (t.h)) by (t.g)`) or in
none (`abs(x - max(t.id) by (t.g))`), is recorded the same way, over its path from the reducers around it
(`ReplaceDataUnderDecisions`; the empty path in none), its column replacing it
in the argument and in the NULL test `SkipNullData` put in the filter. It is read there as a coefficient, so its
argument's check refuses an infinity as well as a NaN (`read_as`, `DecideValueRole::COEFFICIENT`); a bound's refuses a
NaN only. The column takes the query location of the reducer it stands for (`DecideReplaces`), so a message
quotes `max(cap) by (g)` as written and never calls it a column. The guard over it that is NULL outside the
reducer's groups holds a `BoundDecideNullTestExpression` (`RhsBoundRewriter::InKey`), which settles membership only,
so a signature reads the value as its key alone; one whose record has the whole-query key is one value for the whole
query in every clause that reads it (`IsDecideQueryWideValue`), which the tree reads whole. A frame selector's value
is a column stage 08 computes from the rows it selects, so no side table reads one: a frame in a row-wise bound stays
in it, read as a column is, so an infinite value is a bound as a column's is (guide §3);
in a bound of a clause over groups it stays only where it has one value per group -- a
`first` or `last` frame whose timeline key the group key holds -- and execution checks its NaN as it reads it. One
that needs its group's tightest value (D85) sets its clause's `tightest_frame_bound`
(`BoundDecideClause`, which `RewriteMinMax` copies to an easy half), and execution takes that value from the rows it reads (stage 08), in the groups
that hold an instance. A column's tightest value is the group's MIN or MAX in a side table.
Bound reduction relies on two facts a later pass can change: the clause's shape, since only the aggregate shape takes
the group's tightest value, and the comparison's direction, which says which value is the tightest. For a bound that
may differ within a group it records both on the clause (`bound_read_shape`, `bound_read_type`), and a debug build
checks them after every pass, in the one place every pass runs (`RunPass`, `DecideOptimizer::VerifyBoundReads`). The
check is the rule: a clause is one row per group exactly when it was read as one, and a clause read as one keeps its
relation unless that was `=`, whose bound is the tightest of both directions. One pass ends the record:
`RewriteScopedReducers` unsets it on a clause whose every reducer becomes a helper (a hard MIN or MAX), because the
helper is one instance per group and each row of the group states it against the bound the group read. Nothing else
is verified this way.
An `=` bound's checks are scope checks made before any rule runs: its NaN check, then its `=` check (two values in
one group, `AddDisagreementCheck`): one check per `by` group, whose message names the group and points at the bound
(R15), made before any rule can; the NaN check stands with it, so every path raises the same error of the two. Every
other data bound's NaN check is a scope check too. A check copies the value it reads whole (`NanCheck`,
`AddAbsValueCheck`): the tree reads a frame by its `frame` index, a reducer by its node and a query-wide value by its
entry of `computed_values` or its record. The `=` check speaks only
where each of the clause's reducers with a `when` of its own keeps a row of the group: a group with none holds no
instance and constrains nothing, so
its bound is not read (guide §7.4), as a frame's is not. A reducer over data whose argument reads a frame stays a reducer over the rows it
reads, naming its entry of `LogicalDecide::frame_aggregates` by its `frame_aggregate` field (its path key and its own `per` key,
`MakeFrameAggregate`), and a norm of one also checks its body (`ArgumentMessage`) at the rows the reducer reads,
since a norm's value can hide a NaN (an L0 count's SIGN reads it as 0); the tree makes it a G whose operand reads the
frames (`DecideTree::frame_aggregate_nodes`), which stage 08 folds. With several threads the
source hands its rows on in an order that varies with the thread schedule. The DECIDE operator reads them in that order,
so the model's row and column order and the group a scope check names first can differ between runs of
one query. Neither changes the answer's optimum. Tests that compare models run on one thread
(`DeciDBCli.dump_model`, `golden/capture.sh`). Then the direct-solve rules may replace the node with
ordinary operators (`../../direct_solve/README.md`); a node they leave takes the passes
below.

`DecideOptimizer::RewriteForSolver` runs the passes below, and the order is load-bearing. The table includes the
tree's early definition-function preparation (7), which reserves helper indices before the value rewrite and the
term splitter use them:

| # | Pass | Why here |
|---|---|---|
| 1 | `TagAtomicRemovalGroups` | Gives each source clause a written-order identity before any one-to-many rewrite, so a diagnostic repair removes a clause's rows together. |
| 2 | `RewriteFrameRanges` | Each range frame over decisions becomes a helper decision per window before later rewrites copy clauses, so they read an ordinary keyed decision. |
| 3 | `RewriteNorm` | Lowers the NORM marker, including L0 indicator metadata, and clears the written order (`norm`) of desugared L1/L2/infinity norms. |
| 4 | `RewriteInDomain` | Lowers bound DECIDE-variable IN markers to indicator metadata. |
| 5 | `RewriteBilinear` | The one bilinear pass: retains Boolean products and records their expansions and product F-node specs. A product a later pass exposes is kept where it appears (8, 11), through the same entry. |
| 6 | `RewriteMinMax` | Marks the easy identity of a MIN or MAX (`max(e) <= K`) and splits an equality on one, before any pass reads the clause's direction. |
| 7 | `DecideTreeConstruction::PrepareDefinitionFunctions` | Registers definition F-node specs and reserves scoped helper columns before the value rewrite consumes their reads. |
| 8 | `RewriteScopedReducers` | The value rewrite: a VALUE-shape comparison's reducers, and every other MIN or MAX the easy identity left, become helper decisions. |
| 9 | `RewriteNotEqual` | Creates `<>` indicators. |
| 10 | `AccountAvg` | Records the account of each AVG over decisions, which stays an AVG. |
| 11 | `RewriteAbs` | Decides which ABS is exact from its body's owner, once the rewrites that join a helper's readers (2, 7, 8) have run. Allocates the ABS helpers, records each ABS in `helper_definitions` for the tree and stage 06, and adds the check of each ABS argument's data. It emits no row and no indicator. Hands each body whose ABS got a helper back to the bilinear entry. |
| 12 | `FoldSingletonRows` | Must be last of all the rewrites — see below. |
| 13 | `ChooseHelperForms` | Sets each reducer helper's form (`DecideReducerValueDefinition::form`) from where the plan reads it, a fact joined over the helper's readers, so it runs once every rewrite has added its readers. It rewrites nothing. |

The `optimizer.decide` span of `DECIDB_PROFILE=1` times the whole block.

**The form of a reducer helper** (`decide_helper_form.cpp`; context/opa_rewrite/decisions.md R42 to R44, R47). A helper the
value rewrite made is a column, or its members are written into what reads it. The pass (`HelperForms`) has two
steps. It reads the readers: every place the plan names the helper's column, counted over every expression the plan
holds (`LogicalDecide::EnumerateExpressions`, and a product's factors), and those of them that take a linear
expression in its place, read from every body (`EnumerateBodies`) and each ABS operand with the walk every push is
read from (`WalkPolarity`): how many linear rows read it, whether those rows lie within its groups, and the one other
helper whose member reads it. Then it chooses the form (`FormOf`).

The rule lists the readers that take a linear expression: a linear row of a clause, the linear objective, a member of
one other helper. A place the plan names the helper that is none of these states its column, so a reader the rule
does not know keeps the helper a variable (`HelperReaders::named` against `taken`).

| Form | Reducer | When |
|---|---|---|
| `WRITTEN_IN` | SUM, AVG | every reader takes a linear expression, and it is in a nest of sums (a SUM or AVG that is no frame reads it, or it reads one that is no frame), or it is a member of a MIN or MAX, or one row reads it and that row holds each instance once: a range whose window moves with the row (`DecideFrameSpec::Relative`), or a reducer whose reading clause has its rows within the reducer's groups |
| `ONE_MEMBER_WRITTEN_IN` | MIN, MAX | every reader takes a linear expression: a group of one member is that member |
| `ONE_ROW_WRITTEN_IN` | SUM, AVG | every reader takes a linear expression and one row of a clause reads it, where the plan cannot say that row holds each instance once: a group one row reads is written into that row |
| `VARIABLE` | any | any other |

So the helper keeps its column under a product or a square, a frame's payload, an exact ABS,
a `<>`, a row that is not linear, a bound on one decision (which may fold into the column's box), or a second
helper; and as a member of a MIN or MAX pinned on both sides, which states each member in two rows. A one-sided ABS states its operand in two rows, so it counts as two readers. A
range of a whole timeline is one instance that every row of the timeline reads, so the data settles it: it keeps
its column on a timeline of several rows.

A clause's rows lie within a helper's groups when it states at most one row for each group, by construction: the
objective, or a clause whose `per` key is among the helper's `by` keys and whose row signature
(`CollectSignatureFields`) names no per-row decision, no frame, no `by` key outside the helper's, and no data its
class key does not hold. Such data makes a row per value (D70), and each row would hold the group's members:
`sum(x) by (g) <= cap * y` over 8,000 rows in 4 groups is 15,820 nonzeros with a column per group and 7.8 million
written in. The plan cannot count such a clause's rows per group, so the data does (`ONE_ROW_WRITTEN_IN`, R47):
stage 08 counts the rows of the reading clause that name each group, writes a group one row reads into that row, and
keeps the column of a group several rows read. No model grows by it: a group written into its one row costs its
members there, against the same members in a definition row, a column and a read. So the link of a
facility-location model keyed by a column, `per g: sum(x) by (g) <= budget * open`, has no helper where each `g`
has one `budget`, and a helper for each `g` the data gives two. A per-row clause (`x <= share * sum(x) by (g)`)
reads each group once per member, so a group of several members keeps its column (N rows of two entries, not N rows
of a group's members) and a group of one member is written into its row. Keyed by the relation that holds the
budget (`per G: sum(x) by (G) <= G.budget * open`), the key holds `budget`, the clause is an aggregate clause, and
no helper is made at planning.

The pass states the form in the plan: the rewrite account of a `WRITTEN_IN` helper becomes
`Kind::VALUE_HELPER_INLINED`, so a plain EXPLAIN prints it. An account of a reducer helper names the helper by its
decision (`RewriteAccount::helper`), so two helpers that print alike each say what they became. Stage 08 applies the form where it lists the helpers
(`DecideModelEmitter::HandLink`) and decides nothing.

**Every pass that reads bodies reaches them through one enumerator** (`LogicalDecide::EnumerateBodies`, the Visitor
pattern, as DuckDB's `LogicalOperatorVisitor::EnumerateExpressions` enumerates a node's expressions). It visits
the objective, the clauses, then the helper definitions (`LogicalDecide::helper_definitions`) in table order, which is
the order the helpers were made, with one case per kind: a `DEFINITION`'s body, a `REDUCER_VALUE`'s argument, an
`L0_INDICATOR`'s three links; an ABS's operand and a product's expansion are copies of what a body holds, and no body.
It hands the callback a body
(`DecideOwnedBody`) with its expression, its owner, the push its owner gives it, the rows it is read on and, for a
clause, an L0 link or the objective, its record (`BoundDecideClause`), which holds its origin and scope. A pass filters by owner in its callback and loops over no container. A new
kind of helper is a case in the enumerator and nowhere else. The order is the order the columns a pass adds stand
in, which the model dumps pin. Four walks stay outside it, each for its own reason: `FindBoxedHelpers` walks the helper
graph by binding, not by body; folding looks a link up by its helper; the tree-side loops of the linear form and the
solver gate read the tree, not the containers; and `RewriteInDomain`, `RewriteMinMax` and folding loop over the clause records, because they read or write a
record's fields and not only its body.

A later stage-05 lowering, `DecideTermLowering`, belongs to this stage but is
**run during physical planning**, as the term half of the one lowering walk — see §1a. It lowers each `REDUCER_VALUE` definition's body
into its helper's G or O.

---

## 1a. Linear-form flattening — `DecideTermLowering`

Turns each canonical comparison and the objective into additive terms:
`sign * coefficient * variable`, and product groups for the terms of degree 2. This is where DECIDE does its linear
algebra — distributing `K * (1 - pick)` into `K - K*pick`, pulling coefficients out of `*`
chains, pushing a divisor into every produced coefficient, folding unary minus,
stripping binder casts, and multiplying a peeled reducer scale into everything the
reducer produced.

One walk lowers a clause once (`DecideTreeConstruction`): it makes the clause's nodes, then hands the same canonical
clause and its facts to the term half (`DecideTermLowering`: `Clause`, `Objective`, `Members`, `Fallback`), which the
walk owns. The output goes into the tree under construction, not into a model of its own: each
clause's `DecideConstraint` into the root the walk made of its comparison (a folded bound's into its
folded root), the optional `DecideObjective` into the objective's root, each helper's terms
into its G or O, and the frame columns they read into `DecideTree::frame_reads`. So every construct reaches execution
as lowered terms rather than one path re-deriving its own, and the solver gate reads its model class off the same
records (`RequireDecideSolverSupport`). This is the tree's lowering, one IR: the records stage 08 reads
are the ones it writes.

**A coefficient stays an unevaluated `Expression`.** `SUM(x * price)` yields the
coefficient `price`, which is a number only once a row exists. Everything else
about a term — which variable it names, its sign, which reducer produced it, which
filter applies, which key scope (`KeyedScopeInfo`) its reducer's own `per` names — is a
fact about types and structure and is settled here. The pass reads no data.

**Why it is here and not at execution.** Doing this algebra needs a binder: every
rebuilt subtree goes through `DecideRebindOperator` → `FunctionBinder::BindScalarFunction`
so the operator is resolved for the children it is actually given. Downstream of the
binder it would mean reconstructing bound nodes by hand from another node's
`FunctionData`, which does not fail on a type mismatch — it reinterprets the
children's physical representation and returns a plausible wrong number.

**A clause's scope** (stage 04 §3.10). A clause's record owns its scope as a `DecideClauseScope`, which the
lowered clause (`DecideConstraint`) copies (`ReadDecideClauseFacts`). `scope.when` is its `when c:`. A `per K:`
(kept on every shape) gives `class_key`, its key (`INVALID_INDEX` for `per ()`), and `scope.per`: the record holds
`NAMES_CLASSES`, and the tree makes it `ONE_ROW_PER_CLASS` over the
per-row shape, leaving `NAMES_CLASSES` over the aggregate shape and an easy MIN/MAX, whose rows are
their `by` groups and their members and only stand for the classes (D28). The
record's `group_key` is an index into `LogicalDecide::keys`. An
easy MIN/MAX (`minmax_easy` on the record, its reducer kept:
`was_minmax_easy`) extracts its body's terms with the reducer's `when` as their filter,
one row per member (`ExtractEasyExtremumTerms`). The filter is attached once, after
extraction, to every term the body produced — linear terms and product groups alike
(`AttachReducerFilter`, shared with the aggregate shape) — rather than threaded through
the extraction branches, where a branch that forgets it leaves one term unfiltered: stage
08 then reads every kept row as a member, the rows the reducer's own `when` drops reach
the model as `constant >= K`, and a feasible query is called infeasible. A comparison with no shape (`INVALID`), or still in the VALUE shape, is
an `InternalException`: stage 04 classifies every clause and the value rewrite replaces the
second. So is a term that holds a decision under anything but a bare reference
(term extraction, `DecideLinearFormBuilder`): no rewrite lowered it, and reading it as its decision would drop the
construct without a word.

The objective's `scope.when` gives `DecideObjective::when_condition`. An AVG term with a `by` key -- a
`sum(per K: avg(e) by (G))` the value rewrite read in place -- carries it as `avg_group_key`, so execution divides each
row by its `G` group's count; like terms merge only with the same one (`TermsAreLike`). A decision outside the
objective's reducers — query-wide or not — is a `NONE` term, counted once at execution, and so is a product or a square
of query-wide decisions (`MAXIMIZE SUM(x) + s * u`, `MINIMIZE (s - 3) * (s - 3)`, a product group with `reduction` NONE;
inside a reducer it is SUM and weighted by every row, `SUM(w * s * u)`). A decision is query-wide when it is declared
`per ()` or is the helper of a reducer with no `by`, a keyed scope with the empty key (`IsQueryWideDecision`):
`power(sum(x) - 3, 2)` reads the helper the value rewrite holds `sum(x)` in. A helper with more than one instance over
the kept rows is refused when the objective is read (`DecideObjectiveTwice`). Any other product of decisions left there
is refused (*"'x * y' multiplies decisions outside a reducer, which is not supported"*), since it has no once-only
reading in the model. So is a term that is not itself a reducer but holds one (`sum(x) * max(x)`,
`(sum(x) + 1) * max(x)`): no rewrite held its reducers in helper decisions, so they cannot be read as terms
(`ExtractObjectiveTupleTerm`). `RewriteBilinear` has already linearized a product with a `BOOL` decision by then.

A clause and the objective hold the same pair, their linear terms and their product groups (`DecideTermLists`), and
one extraction fills it for both: `ExtractAtomTerms` over the canonical atoms, `ExtractReducerTerms` for a reducer,
`ExtractBodyTerms` for a body. The objective's own rules are branches of it: a nest it reads in place, an AVG's `by`
key, and the refusal of a product outside a reducer.

### Terms of degree 2 — product groups

`DecideTermSplitter` (`decide_term_split.cpp`) reads one atom's body in the canonical spelling
(`../04_canonicalizer/done.md` §3.3b) as terms of one kind: `{sign, coefficient (null = 1, never a decision), degree}`
and, by degree, the decision a linear term multiplies, the one linear form a square squares, or the two a product
multiplies. A decision-bearing node it does not open (an `ABS`, a reducer, a marker a rewrite left) is an opaque term,
which each reader takes or declines. It never refuses: the shape check has already refused what the model has no
reading for (`../04_canonicalizer/done.md` §3.11), and an opaque term that reaches term extraction is an internal
error. The solver path, singleton-row folding and direct solve read the same terms, so a shape one of them admits
cannot mean something else to another.

One struct, `DecideProductGroup`, carries every term of degree 2 from here through stage 06 and stage 08, in a
constraint and in the objective. It stands for `Q += w · l · rᵀ` summed over the rows its reducer reads:

| Field | Meaning |
|---|---|
| `left`, `right` | Linear forms (terms of degree 0 and 1) in the row's decisions. A square has an empty `right` and multiplies `left` by itself. |
| `weight` | The coefficient, never a decision; null means 1. A reducer's factor (`2 * SUM(POWER(x, 2))`) is multiplied into it as into a linear term's coefficient, a factor known only when the query runs included. |
| `sign` | The term's sign. |
| `weight_sign` | The sign of `sign * weight` as the plan knows it (`ScaleSignAtPlanTime`): +1 or -1, and 0 for a weight known only when the query runs. The gate reads convexity from it. |
| `filter`, `avg_scale`, `qualifier_scope_idx`, `reduction`, `inputs` | As for `DecideTerm`: the reducer's own `when`, AVG's `1/N`, the key scope of its `per`, whether it is a SUM or counted once, and the values `weight` is computed from. |

`ReadDecideProduct` merges equal factors into one raised to the sum of their powers, and a factor equal to another
with every sign reversed into that factor too, flipping the product's sign once per odd power of it: `(x - s) * (s - x)`
is `-(x - s)²`, a square whose sign the gate reads for convexity, not a product of two forms.

The model builder writes a square as a symmetric rank-one update and a product as a general one
(`../06_model_formulation/done.md` §6). AVG scales the group's weight by `1/N`.

**Departure from the standard idea.** A rank-one update `Q += l · rᵀ` carries the weight inside `l`, so a square
with weight `w` is `l = w·f, r = f`. This stage keeps `weight` as a field of its own. A square's entry is `w·l_i·l_j`,
evaluated in that order, so its arithmetic is bitwise the same as the weight times the form's two coefficients, with
no weight folded into each coefficient of the form first; and the weight is evaluated once per group and row, not
once per coefficient of the form.

What the group admits: two or more squares in one objective, each a group summed into
the one Q; a data weight on a square (`w*POWER(x, 2)`, `w*x*x`), whose sign is known only when the query runs; and a
query-wide factor on a square. The last two are non-convex to the gate, so Gurobi only. A product outside every reducer
in the objective follows §1a above.

**Frame selectors** (`../../language/guide.md` §5.6, D78). The term splitter reads a frame over decisions as one
`FRAME` term: its factor (1 for a bare frame, the data factors around it, a divisor) and its payload's linear terms.
`AppendFrameTerms` turns it into the terms the model reads: each payload term `c·x` becomes `f·READ(c)·x`, its decision
read at the row the frame selects (`DecideTerm::frame_read`), and each term `c'·y` of the `else` becomes
`f·FALLBACK(c')·y`, read at the row itself. `READ`, `FALLBACK`, `VALUE` and `DEFINED` are columns stage 08 appends to
the input after its own (`DecideFrameReads::columns`, from `DecideFrameReads::column_base`), so every evaluator reads a
frame as data and none learns what one is: `READ` is `c` at the selected row, 0 where a missing position has an `else`
and NULL where it has none; `FALLBACK` is `c'` at a row whose position is missing, 0 elsewhere. NULL is no value
(D84): a comparison reading it skips that instance, the objective drops the term (`DropNullObjectiveTerms`). The
payload's terms are the frame's `READ` columns, and a position whose rows hold two payloads is refused at execution
(D73, `DecideDescentPosition::conflict`); the decisions its `else` reads are recorded per frame
(`DecideFrameReads::fallback_decisions`, for D82).

The tree owns the frame columns (`DecideTree::frame_reads`), and whoever reads a frame asks it for the column
(`DecideFrameReads::Slot`, which makes one when none reads that value that way yet; R50). The tree walk, as it makes
a frame's O, asks for the one value a frame over data reads and keeps its index on the node
(`DecideFrameNode::value_column`): its `VALUE` column (the payload at the selected row, the `else` at a row that
reaches it, NULL where neither stands, of the payload's own type) or, for a range, its `REDUCED` column (the frame's
reducer over the window's values). The term half asks for the `READ` and `FALLBACK` columns of the coefficients a
frame over decisions reads, and for an `else`'s as its O is made (`DecideTermLowering::Fallback`). The program
compiler asks for the `DEFINED` column a reducer's NULL test of a frame over decisions reads (stage 04), TRUE where
each of the frame's `READ` and `FALLBACK` columns holds a value. A frame over data stays its marker in a coefficient or
a bound, and the compiler reads it from its O as it compiles the expression (`Translate`), so no pass rewrites the
term half's records and the two readings cannot disagree on a frame's column. A frame over decisions inside a `MIN`/`MAX` of several terms is a term
like any other (`DecideBody::members`), its `frame_read` set: stage 08 hands stage 06 the decision and the instance
each term reads (`ReducerValueMembers::variables`, `::instances`), the instance the frame's selected row reads. A range over decisions reaches no term: it is a
helper by now (§2, `RewriteFrameRanges`), whose link also reads the range's `else` as terms, each coefficient a
FALLBACK column (`DecideBody::fallback`). What a signature reads of a reached `else` is recorded per frame
(`DecideFrameReads::fallback_columns`).

### Like-term collection

After flattening, every list that becomes a **linear** solver row is grouped:
constraint `terms`, the objective's `terms`, and each value link's terms and `else` terms. `2*ship + 3*ship`
becomes one term with coefficient `2 + 3`. A term contributes `sign * coefficient`, so a group merges as
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
| `qualifier_scope_idx` | Selects a de-duplication mask (the reducer's own `per`) — again a statement about which rows contribute |
| `frame_read` | `x - at(previous: x) over (...)` names one decision at two rows |

Constants (`INVALID_INDEX`) are left alone: a fixed offset folded into the RHS is
not a repeated column. Product groups are left alone too — they feed
an outer product rather than a linear row.

**This closes a trap, not a defect.** The model builder folds duplicate column
entries when writing the matrix row, so collection leaves the golden dump unchanged
and no result-level test can see it. It spares every other consumer from handling a
repeated variable index, such as an implied-bound derivation that reads a single
term's coefficient instead of the sum. `test_like_term_collection.py` pins the
*non*-merges, which are the observable direction.

**Why it is triggered from `plan_decide.cpp` rather than inside `OptimizeDecide`.**
The lowered terms hold *copies* of coefficient subtrees. `RemoveUnusedColumns`,
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

### The helpers' order

The links are listed innermost first (a range's, made first, then the reducers nested in it), which stage 08 relies on
when it writes a helper's members into a later one (`../06_model_formulation/done.md`, "A helper written where it is
read").

---

## 2. The passes

### ABS linearization — `RewriteAbs`

Detects `BoundFunctionExpression` for `ABS` over decision variables and allocates an auxiliary for each (INTEGER when
the operand is integral, else DOUBLE). The ABS node stays in its clause as a kept call that names its definition
(`BoundFunctionExpression::definition`), which term extraction reads
as the auxiliary. Each ABS is recorded in `LogicalDecide::helper_definitions` (kind `ABS`): the auxiliary, the operand, whether it is
exact, the rows it is read on, its scope, the clause it was written in, and its text as written. Stage 05 emits no row,
allocates no sign indicator and reads no `use_native_constructs`. An ABS is a pseudo-operation, as in LLVM's
legalization: stage 06 either states it natively or expands it (`../06_model_formulation/done.md`, "ABS"). The tree
gives each ABS an F node and builds its two envelope rows, `aux >= inner` and `aux >= -inner`, beneath it
(`RegisterHelperDefinitions`, `MakeAbsRow`), as it does for a definition; EXPLAIN prints both.

**The operand is recorded once the bodies are final.** `RewriteAbs` ends with `RecordAbsOperands`, run over every body
after the bilinear worklist drains, and it is the only place an ABS definition's `body` is set. A copy taken when the
helper is allocated would miss the definitions the bilinear entry makes the products name: a product with a `BOOL`
factor is linear once the entry keeps it, and a product with an `ABS` factor is kept only by the worklist described in the bilinear
section. An operand that still holds a square or a product of decisions is refused at the ABS with a
`NotImplementedException`, *"ABS over a product or a power of decisions is not supported"*. The test is
`RequireLinearOperand`, which asks `DecideTermSplitter::IsLinear` and which `RewriteNotEqual` calls too. The refusal
stands because the rows of an ABS state its operand as a linear form (`AbsEnvelope` in the emitter asserts the
envelope holds no product), so a product there needs a route of its own. The direct clause reader guards the terms
inside an ABS as it guards a part's terms (`GuardCoefficients` recurses into `term.inner`); a term of degree 2 there has
no coefficient of its own and the clause reader declines it (`HasUnknownTerm`), so the solver path refuses it here.

A product is kept where it fits the one constraint path and refused elsewhere.

**A NaN or infinite value is named where it stands.** Its message says the value is NaN or infinite and quotes it as
the user wrote it, with the caret on it (`DecideInvalidValueError`, `common/decide_value_messages.hpp`); it names no
row and suggests no edit. A coefficient keeps no list of the inputs it was merged from: the solver path throws the
error at the value's own location, and a direct plan raises it with the SQL function `__internal_decide_invalid_value`,
which reads that location from its own expression. A node a rewrite builds takes the location of the node it equals in
value (`DecideReplaces`); a like-term merge or a factor applied makes a coefficient with no location, whose error has
no caret. A coefficient has one message for NaN and infinity. A number
past a DOUBLE (`TOO_LARGE`) is raised only where DeciDB accumulates across rows. An `=` bound that takes two values
raises through `__internal_decide_bound_disagreement` on a direct plan, from the same builder.

The nodes the canonicalizer and the other rewrites build keep the location of the user text they replace
(`context/opa_rewrite/design.md` § 3, contract 5).

One location rule holds (`decide_source_provenance.hpp`, `DecideReplaces`): every replacement equal in value takes the
replaced node's location, `ReadQuotient`'s `(1/0) * x` and the like-term merge included. No structural proxy (a sum,
difference or quotient decomposition, a zero-divisor rule that names the value) picks a location. A coefficient has no
split between a NaN message and an infinity message, and `TOO_LARGE` is raised only where DeciDB accumulates across
rows.

An `=` bound that takes two values names its `by` group, not its class: one check per group, on a bound of a row-wise
clause and a frame bound alike (R15).

The data of an ABS argument is checked where the ABS is read, before the envelope turns
it into a bound on the auxiliary (`AddAbsValueCheck`): the sum of the argument's terms
that hold no decision (`-v` in `ABS(x - v)`) must be finite on every row the ABS is read
on. Each term is copied whole, so a frame among them is read at the row it selects, as a
coefficient's is. A row where the sum is not finite raises the message every number the model reads raises
(`DecideInvalidValueMessage`, shared with the direct-solve plans): the first of its values that
is NaN or infinite, as the user wrote it, or, when each is finite and their sum passes a
DOUBLE, the clause or objective with `computes a number too large for a DOUBLE`. So the
solver path and direct solve's A2 raise the same text, and no message names a bound the
user never wrote. The rows an ABS is read on are those its clause's `when` keeps that also
pass the `when` of each reducer around it (`SUM(when f: ABS(x - v))`), and the envelope
rows carry the same condition, so a row a reducer's own `when` drops is never read. The
check is a value check (`LogicalDecide::value_checks`), which the tree evaluates on the rows it reads.

Whether an ABS needs its exact (Big-M) encoding is an inherited attribute, in the sense of an attribute grammar: the
push of the body that holds it, read from the body's owner and handed down to the ABS. `FindExactAbs`, which
`RewriteAbs` calls first, walks every owner once and collects, into an `AbsExactSet`, the ABS nodes their owner's push
does not pin; `FindAbsAndAllocateAux` reads the set. `DecideHelperDefinition::exact` records the answer for one ABS: whether the
auxiliary needs its upper side pinned. It is false when the owner's push holds `aux` at `|inner|` by solver pressure or
by the clause's shape: an objective term the minimization pushes down, or a constraint shape that upper-bounds `aux`.
When true, stage 06 pins the auxiliary from above, with the backend's absolute value or a sign indicator and its Big-M
rows (`EmitAbsConstraints`).

The owners, and the push each gives its body:

- **A clause** (and an L0 indicator's links, which are clauses too) reads as `E <op> 0`. `<=` pushes a positive term
  down and a negative one up, `>=` the mirror, `=` and `<>` push nothing.
- **The objective** is a minimization and pushes a positive term down: the objective `E` pins what `E <= K` pins, and
  a `MAXIMIZE E` arrives as `-E`. So `MINIMIZE -abs(e)` and `MAXIMIZE abs(e)` take the exact encoding and
  `MAXIMIZE -abs(e)` does not. Reading the objective's direction alone would call `MINIMIZE 0 * sum(x) -
  abs(sum(x) - 9)` unbounded.
- **A definition** (the body of a non-linear reducer, held by its helper `t`) pushes by its relation: `t >= q` pushes
  `q` down, `t <= q` up, `t = q` neither.
- **A value link** (a reducer that became a helper) pushes its members by the sides it states, `ValueLinkPush`. A link
  that states both sides or neither pushes nothing. The envelope side alone (`value >= members` of a SUM, AVG or MAX)
  holds the members down; for a MIN it is `value <= members`, which holds them up. The closing side alone holds a
  SUM's or AVG's members up and a MIN's or MAX's none, since its closing side is the hard direction.

A body that several readers share, a helper's, has one owner: the definition that holds every side any reader needs.
So `x + sum(abs(x - 3)) <= 100` and `x + sum(abs(x - 3)) >= 50` share one helper pushed down by the first and up by
the second, which pins nothing, and both clauses read one exact ABS. The sides are known only after the rewrites that
join a helper's readers have run (`RewriteFrameRanges`, `PrepareDefinitionFunctions`, `RewriteScopedReducers`), so
`RewriteAbs` runs after all three.

Each owner's push is read through `WalkPolarity` (`decide_optimizer.cpp`), the one polarity walker: the value rewrite's
reducer sites (`CollectReducerSites`), the range frames' sides and the definition of non-linear reducer bodies read
their push from it too. It folds signs through `+`, unary and binary `-`, casts, aggregate bodies, and constant factors.
A factor whose value is not known until execution, a data column as in `SUM(w * ABS(x - t))`, yields sign 0, which never
matches the pinning direction and so forces the exact encoding, in the objective as in a constraint: `MINIMIZE SUM(w *
ABS(x - t))` with a negative `w` on one row answers rather than reporting "unbounded", at the price of needing a finite
box for the decisions inside the ABS on the lowering arm. A factor's sign is read by `PlanTimeSignOf`
(`decide_cast_policy`), structurally rather than only by folding: a product with a zero constant among its factors has
sign zero whatever its data factors hold (`0 * w`), a negation flips, a quotient follows its numerator. A sign is all
this reads: `0 * w` still varies by row as `w` does, and is NULL where `w` is. A BOOL decision as a factor is 0 or 1, so
it is never negative and keeps the sign of the term it scales. In the value rewrite a reducer beside another reducer
(`sum(x) * sum(y)`) reads sign 0 (`PolarityPath::beside_held`): the other factor's sign is the value of a decision
reducer, which the walk does not know. A factor of exactly zero is a number like any other and reads as positive, so
the term it scales takes the push and the encoding it takes with weight 1: a zero-weighted hard MIN, MAX or ABS needs
the bound its weight-1 twin needs.

**Helpers a hard extremum boxes.** A MIN or MAX link that states its closing side is encoded with a Big-M over its
members, which needs every member boxed. An auxiliary held by pressure alone has no box. So the members of such a link,
and every helper those members read, transitively through value links, column references and the bodies kept calls
name (`FindBoxedHelpers`), count as boxed, and an ABS in any of those bodies takes the exact encoding whatever pushes
the helper itself; the exact encoding is what boxes it. A MIN or MAX pushed its cheap way (`MINIMIZE max(abs(e))`)
states the envelope side alone, which bounds every member, so the plain envelope stands.

The two envelope rows keep the clause's `when` (`MakeAbsRow`), so they read only the
rows the clause keeps; the objective's keep the objective's `when`. The rows an ABS is read on are stored on its record,
and `MakeAbsRow` applies them.

**An ABS inside an ABS.** `FindAbsAndAllocateAux` rewrites the argument before the node, so
an inner ABS becomes its own auxiliary first and the outer's envelope rows refer to it
(term extraction would read an ABS left in a row as its argument, losing the sign). The records are appended innermost
first, the order `DeriveAbsAuxiliaryBounds` boxes them in: the outer range needs the inner box. An ABS beneath another
is pinned exactly, because the outer ABS is not monotone in it: over `x` in `[-1, 1]`,
`abs(abs(x) - 2) <= 0` with an inner auxiliary held by its lower envelope alone would hold
at 2. `MarkExactAbs` records an ABS and then descends into its argument with sign 0, so a nested ABS never classifies
as pinned, under any owner. The label is rendered before the argument is rewritten, so each
account names the ABS as written.

The exact encoding for a data factor of unknown sign is deliberately conservative: a row whose `w` happens to be
positive gets an envelope it does not need. Assuming such a factor positive is unsound, since
a negative `w` makes enlarging the auxiliary *relax* the row, and the constraint
silently stops binding. The cost, measured on a weighted-ABS
constraint over TPC-H `lineitem`, comparing a literal coefficient (pinned, no
Big-M) against an all-ones column (Big-M) so the two models are mathematically
identical: 0.16s vs 0.65s at 30K rows and 0.26s vs 1.29s at 60K, same optimum
both ways. A ~5x constant factor, still near-linear in row count — cheap enough
that deciding sign per row at execution time is not warranted.

### NORM and IN — `RewriteNorm` / `RewriteInDomain`

The binder keeps `norm(...)` as a reducer with no function (a marker), so it takes a
`by (G)` and reads the rows its clause's `when` keeps as any reducer does.
Canonicalization (stage 04) replaces L1, L2, and infinity norms with `SUM(ABS)`,
`SUM(POWER(_, 2))`, and `MAX(ABS)`, which keep their written order in the `norm` field for display only;
`RewriteNorm` clears it, so every layer after it shows the definition the solver
receives. L0 needs indicator variables, so it reaches this pass as a marker and emits
its Boolean indicator and exact forward/reverse links. Lowering swaps the marker for a
`SUM` of indicators under any factor the canonicalizer peeled (`2 * norm(e, 0) <= 6`), so
the pass detaches the objective and the constraints, lowers them, and reinstalls them
through `SetObjective` and `AddClause`; an edit left where it sits would not be
canonical (C4/C6/C7). A desugarable marker reaching this pass is an internal error.

**A marker is lowered wherever it stands.** `RewriteNorm` descends through every
expression container, not only comparisons and conjunctions, because a norm is a
scalar and a regularizer is normally written as one term of a larger expression —
`MINIMIZE SUM(cost*x) + 0.5 * norm(x - base, 1)`, `2 * norm(e, 1) <= K`. The
descent is `ExpressionIterator::EnumerateChildren`, the same generic recursion
`RewriteAbs` uses. A marker cannot sit beneath another
aggregate, because the binder rejects `SUM(norm(...))` before this pass runs.

**The marker must not outlive this layer**, and `VerifyNormMarkersLowered` asserts
that over `LogicalDecide::EnumerateExpressions` at the end of `OptimizeDecide` —
after the value rewrite, which lifts reducers out of the trees into helpers. This assertion is not defence in depth. The
marker is a real `SUM` aggregate with the order recorded in its alias, so an unlowered one is not
recognisably broken downstream: extraction reads it as the `SUM(e)` it is built on,
the norm silently leaves the model, and the query returns a confident wrong answer.

A bound `x IN (...)` stays a native `COMPARE_IN` marker until this pass. It then
uses the singleton, Boolean-domain, or cardinality/linking formulation. The two size
optimisations that need to read the domain values — the `{0,1}` Boolean shortcut
and the `x >= min(domain)` tightening on the general encoding — evaluate each
value with the cast policy's folder, so a written-out expression such as
`x IN (1-3, 2)` qualifies exactly like the literal `x IN (-2, 2)` does. Neither
affects correctness: the indicator encoding is exact with or without them. Every row
the IN generates keeps the clause's `when`, as an L0 norm's links do, and its `per K:` (`ScopeRowPerClause`, which copies both onto the row's record), so a row the `when` drops,
or whose key is NULL and so in no class (guide §6.2), is never read. Such a row takes the signature of the fields
it reads itself (D70), not the written clause's: over a data domain it reads per-row indicators, so it stays one
per row, as its indicators are. The
indicators are defined by the decision and its domain, so their scope is the join of the two
(`ScopeOf`: `JoinScopes` over the leaves an expression reads; a decision brings its declared
scope, a query-wide value is query-wide, any other column is row data), and their rows are
emitted once per instance of it (`ScopeRowPerInstance`: under `per K:` for a scope keyed by
`K`, `per ()` for a query-wide one). `per g: x IN (1, 2)` over a keyed `x` and three rows in
two classes is four indicators, not six; a data domain stays per row. They are
structural (`MakeFormulationRow`): a repair never loosens one on its own, and a
DROP removes them with their clause, whose removal group (`origin.removal_group_id`) they carry. A row that defines
an auxiliary standing for a value -- an ABS's envelope, a body helper's definition, an L0
indicator's links -- also names it (`BoundDecideClause::defines`, read into
`DecideConstraint::defined_var`), so stage 08 gives the auxiliary no value where the row reads
an instance with none (D88).

### Bilinear — `RewriteBilinear`

| Operand types | Formulation |
|---|---|
| Boolean × Boolean | AND-linearization: `w <= b1`, `w <= b2`, `w >= b1 + b2 - 1` |
| Boolean × other | McCormick: `w <= U·b`, `w >= x - U·(1-b)` and the `L` corners (needs runtime bounds) |
| Non-Boolean × non-Boolean | Left in place for the Q-matrix path in the physical operator |

The pass reads a product in the canonical spelling (`ReadDecideProduct`, `../04_canonicalizer/done.md` §3.3b): its
data coefficient and sign scale every term it builds, and its two sides are the two decision-bearing factors, or one
factor squared.

The pass records a `BilinearLink` for both Boolean rows and emits none itself; stage 06 emits the rows once per
instance of the auxiliary (`LinearizeBilinear`, `../06_model_formulation/done.md`).

**Each factor is read as an affine expression in the decisions**, through the term splitter
(`DecideTermSplitter::Split`, the one reader of what a decision expression is made of): data-coefficient × decision
terms and a data constant, through `+`, `-`, a data factor, a data divisor and casts. The product distributes over the
terms, so `b * (x - 1)` is `b * x - b` and `(2 * b - 1) * (x - 1)` has four terms. Each pair holding a Boolean gets one
auxiliary, and the pairs are merged (`(b + d) * (d + b)` holds `b * d` once); a Boolean squared is itself; a pair of
non-Booleans stays a product for the Q-matrix path. A factor that is not affine is left in place. A factor that holds a
reducer anywhere (`b * SUM(x)`, `POWER(SUM(x) - 3, 2)`, `max(x) + 1`; `DecideCanonicalizer::ContainsReducer`, the one
test every pass asks) is not a product of variables to this pass: the value rewrite holds each reducer in a helper and
hands the product back (`FindAndReplaceBilinear`). A frame's marker is no reducer (D80): `at(F: e)` selects one row per
row (deck pp. 43, 71), so this pass takes `x * at(previous: x) over (t.id)` as a product. A body its definition F node
owns is not such a reducer: the splitter reads it as its helper decision.

**The pass runs once; a product that appears later is kept where it appears**, through the same entry,
`FindAndReplaceBilinear`, which leaves a product that names its definition as it is. The value rewrite hands over each VALUE-shape
clause and the objective once their reducers are helpers. `RewriteAbs` queues each body in which it allocated an ABS
helper, since a product with an ABS factor is affine in the decisions only once the helper exists, and hands the queue
over after every ABS has its helper: a worklist, as LLVM's InstCombine revisits the users of a value it changed. The
columns those products add therefore follow every ABS helper. A range frame's value link is not read
(`BilinearReadsBody`): its payload is held as a frame reads it.

**The auxiliary's scope is `JoinScopes` of its factors' scopes**, the least upper bound, as DuckDB promotes a binary
operator's type: Global < Keyed < Row. Two query-wide factors give one query-wide column; a query-wide factor and a
keyed one, or two alike keyed factors, a column per key; anything else a column per row. Every other auxiliary takes
its scope the same way, from the leaves its expression reads (`ScopeOf`: a decision brings its scope, a query-wide value
is query-wide, any other column and any reducer is row data, since a reducer's value differs between groups; a reducer
the value rewrite holds in a helper brings the helper's own scope, one class per group, so
`sum(per t.g: abs(sum(x) by (t.g) - 4))` reads one ABS per class of g, where one per row made each row a variant): the ABS
auxiliary, the `__body_` helper, the L0 norm's and the IN's indicators. Their defining rows are
emitted once per instance (`ScopeRowPerInstance`, outside the clause's `when`), so `ABS(s - 4)` over a query-wide `s` is
one auxiliary and one row set, not one per row. The ABS Big-M and the L0 auto bound take their maximum over the rows
(`InstanceBox`), which holds for a shared column. `AddAuxiliaryDecision` registers a keyed auxiliary in
`keyed_scopes[..].scoped_variable_indices` itself, so no rewrite does.

### Non-linear reducer bodies — definition F-node preparation

Every extremum the later passes build reads its members as linear: a MIN or MAX is a helper of the value rewrite,
whatever clause or objective reads it. A body
that squares or multiplies decisions (`max(power(x, 2))`, `min(x * y)`) would lose its
square there. `DecideTreeConstruction::PrepareDefinitionFunctions`
(`decide_tree_definition.cpp`) registers one helper decision `t` per row (`__body_<n>__`,
labelled by the body) and stores its definition F-node spec. The source reducer stays in
place and reads that F value; tree construction emits the quadratic E row defining `t` by
the body `q` once its instances are known. The plan-time helper column is reserved before
later optimizer consumers need a stable index.

**The row is one-sided where the clause pushes the reducer one way**, as the
Plaisted–Greenbaum encoding (the polarity-aware Tseitin transformation) states a
definition only in the polarity it occurs in. SUM, AVG, MIN and MAX never decrease when a
member grows, so a member is pushed the way its reducer is, and the reducer's sign comes
from the same polarity walk the ABS classifier reads (`WalkPolarity`): the clause read as
`LHS - RHS <op> 0`, the objective as the minimization it is. Pushed down, `t >= q`; pushed up,
`t <= q`; neither (`=`, a factor whose sign only the rows decide, a dead term under a
factor of 0), `t = q`. A convex `q` pushed down stays convex. `t` is `INTEGER` when the
body is whole-numbered by declared types (`DecideValueIsWholeNumbered`), so a strict
`max(power(x, 2)) > 48` over an `INT` `x` steps to 49.

**One reducer is defined once** (structural sharing, as the encoding names one subterm once). An earlier rewrite may
have copied a reducer into several rows of one clause: `RewriteAbs` writes `abs(max(power(x, 2)) - 4) >= 1` as two
envelope rows, one pushing the MAX up and one down. The copies must keep reading one value, so tree preparation
collects every site first and groups the readers of one reducer (an equal reducer, its `by`, its own `per` and the
clause's `when`), across clauses and the objective; a group shares one helper whose row is the union of the readers'
sides, `t = q` when they are pushed both ways. A helper per copy, each one-sided, lets the two copies read different
values and answers `x = 2` there, where the MAX is 4. The definition belongs to no clause, as the value rewrite's does
(§ The value rewrite).

**Which bodies.** Every MIN or MAX whose body is not linear (`DecideDegreeOf`). In
a constraint that holds a MIN or MAX, every SUM and AVG term too, since the clause row
reads them as linear (`sum(power(x, 2)) + max(x) <= 30`); and in a constraint of the VALUE shape, whose helper is
defined over linear members (`sum(power(x, 2)) + gcap * s <= 30`). A SUM or AVG that no MIN or MAX reads needs no
native MIN/MAX: its rows are quadratic, which the model-class gate refuses on HiGHS. In the objective, a reducer
nested under a MIN or MAX (`max(per g: sum(power(x - t, 2)) by (g))`), which the value rewrite then holds in a
helper over the defined members; a SUM beside a MIN or MAX stays on the
objective's own quadratic path. A clause `RewriteMinMax` marked
easy (`max(power(x, 2)) <= 16`, `BoundDecideClause::minmax_easy`) is left alone: it bounds each member, which is one
quadratic row per member with no helper.

The row holds on the rows the reducer reads: the clause's `when` and the reducer's own
filter (`max(when g = 'A': power(x, 2))`), in its record's `scope.when`. It is a structural
row (`MakeFormulationRow` with no clause), rigid under DIAGNOSE. A row whose body reads a NULL is
skipped by the reducer as before; its helper row is skipped too and leaves `t` free on a
row nothing reads.

**A helper has no box.** It holds the body's value, which may be negative
(`power(x, 2) - 10 * x`), so it is a signed helper (`DecideHelperDefinition::Signed`), which
`FoldSingletonRows` gives no floor of 0, and nothing derives a ceiling. With no box there is no Big-M for an extremum over `t`, so
only a backend that states MIN/MAX natively takes it: stage 08 takes the native arm for a
family with no box (`../06_model_formulation/done.md`). When the backend chosen in §0
states no MIN/MAX (HiGHS, or Gurobi under `DECIDB_NATIVE_CONSTRUCTS=off`), tree function
preparation refuses
before a row is read, through the same sentence the model-class gate raises
(`ThrowDecideSolverGap`): `MIN or MAX over a squared or multiplied expression is not supported on this
installation`, at the reducer. A SUM term beside a MIN or MAX that squares or multiplies decisions takes the same
gate, as a reducer nested in one does. The refusal names the reducer and not a column to bound, because bounding
`x` would not give `t` a box.

EXPLAIN says how the helper holds the body: `power(x, 2) becomes a helper decision per row,
at least its value` (`BODY_AT_LEAST`; `at most`, `equal to` for `BODY_AT_MOST`,
`BODY_EQUAL`). The defining row itself is left out (a row a `DEFINITION` owns): it
restates the body the reducer already prints. The row carries the source clause of the
first clause that reads the helper (`DecideHelperDefinition::origin`), so a refusal can quote it, and
no removal group (§ The value rewrite).

**Exit contract.** `VerifyExtremumBodiesLinear` runs after the value rewrite and raises an
internal error when a member of a MIN/MAX value link is not linear. The easy identity
(`BoundDecideClause::minmax_easy`) bounds each member in its own quadratic row, holds no link, and is
exempt. A body tree preparation misses fails there by name instead of losing its square. Tests:
`test_minmax_body_definition.py`.

### The value rewrite — `RewriteScopedReducers`

A comparison whose decision side one model row per `by` group cannot hold is in the
**VALUE shape**: a per-row or keyed decision beside a reducer (deck example 1,
`ship <= 0.20 * sum(ship)`), a factor on a reducer that varies by row, reducers with
different `by` keys, a reducer inside another expression, a reducer holding a reducer.
Only a reducer over decisions counts (`ContainsDecisionReducer`): a reducer over data alone is a coefficient
(guide §5.5), so `abs(x - max(t.id) by (t.g)) <= 2` is per-row, as it is without `abs`.
Stage 04 decides the shape once
(`CanonicalConstraintClass::VALUE` in `BoundDecideClause::shape`, stage 04 §3.10); this pass reads
it and turns the clause into an ordinary per-row one, whose shape it writes (`ClassifyClause`), so stage 06 needs no new
constraint path. A clause's shape has these two writers and no other: the canonicalizer sets it once, and this pass,
which discharges the VALUE shape by putting helpers where the reducers were, classifies the clause it leaves. It runs after the bilinear rewrite, whose auxiliaries may stand in a reducer's body, and after
`RewriteMinMax`, which leaves the clauses of the easy identity with their reducer. A clause in any other shape takes
the same pass for its MIN and MAX only (§ MIN/MAX).

For each decision reducer `agg([when c'] [per K']: e) by (G)` on the comparison's left
side (`CollectReducerSites`; a data-only reducer is a number and stays) the pass:

1. allocates a **helper decision** `__value_<n>__` through `AddAuxiliaryDecision`. It is
   INTEGER when the reducer's value is whole by declared types, every constant inside
   counted (`DecideValueIsWholeNumbered`, shared with stage 04's integer step), so a strict bound on the
   rewritten row still steps on the lattice; else DOUBLE. It is keyed by a scope of its
   own (`DecideValueHelperOfScope`), whose classes are the groups of its path key among
   the rows its path keeps, built by stage 08: a path key `by ()` throughout is one class,
   or none when no row holds a member, so a read of it skips its instance (D68). Its label
   is the reducer's SQL, `by` included
   (`aux_var_expressions`, `DecideHelperDefinition::label`), so EXPLAIN and DIAGNOSE name it by
   what it holds;
2. records a `REDUCER_VALUE` entry of `LogicalDecide::helper_definitions`: the reducer node (its function, its
   body, its own `when` and `per`), its path key and the rows its path keeps;
3. replaces the reducer node by a reference to the helper, in place, as `RewriteAbs` does.

The comparison then holds no decision reducer and leaves the VALUE shape: it is one row per
kept row, or per class under `per K:`, and instances that read the same values are one row (a clause with no `per` that
reads no per-row decision has one instance per distinct signature, `per K` classes that agree on everything the clause
reads share one; `../08_execution/done.md` §4a). Stage 06 states each helper's definition per group,
a row for SUM/AVG and an extremum link for MIN/MAX
([`../06_model_formulation/done.md`](../06_model_formulation/done.md), "Helper decisions of
the value rewrite"). Deck example 1 costs N rows and one helper, rather than N rows of N
terms. The user-facing account of the three shapes is
[`../../language/guide.md`](../../language/guide.md) §6.4.

A strict `<`, `>` or a `<>` steps onto the whole numbers, which is exact for the rewritten
row only when the helper is whole. Stage 04 judges the step on the comparison as written, where a data addend inside a
`SUM` moves to the bound when the reducer is summed in place (`sum(x + 0.5) > 4` is `sum(x) > 4 - 0.5 * n`) and passes.
A helper holds the whole value, so over one that can be fractional the step is refused here (`RefuseFractionalStep`),
with stage 04's message (*"'>' compares 'sum(x + 0.5)', which can take fractional values"*): `sum(x + 0.5) + x > 4` is
refused and `sum(x + 0.5) > 4` is not.

**Nested reducers** (guide §5.5, D79-D81). The reducers inside a reducer's argument are
replaced first, each by a helper keyed by its path (`DecideReducerPath`,
`decide_reducer_path.hpp`): it reads the rows the clause's `when` and every enclosing
reducer's own `when` keep, grouped by every enclosing `by` key and its own, a key of the
table `FindOrCreateDecidePathKey` finds or adds. So what the outer helper holds is affine
in helpers, decisions and data, and links are stored innermost first. A member of a SUM or
AVG, a MIN or a MAX is pushed as its helper is, through the sign it carries in the
argument: a MIN or MAX does not decrease when a member grows, so it passes its push on
(D79). A body that would still square or multiply decisions there is refused (D80):
definition F-node preparation has registered every such body a constraint, or a MIN or MAX
of the objective, reads, so what is left is a nest of sums in the objective not read in
place. A nested reducer the passes before this one would rewrite out of its path -- one an
ABS, a POWER or a product with a decision reads, any node but `+`, `-`, a cast or a data
factor -- is represented by its reducer G node beneath the enclosing F node (deck p. 51).
The binder refuses a norm around one (stage 02).

**The objective's sums of group sums of squares** (`SwapObjectiveSummation`, the first pass, before
`RewriteRhsAggregates`): `sum([when c]: inner([when c'] e) by (G))` with no `per` on the outer sum and `e` not
linear reads each G group once per outer row of it, so it is one sum over the rows `c` keeps of `e` times a weight: the
group's rows the outer keeps, `sum(1) by (G)`, a reducer over data the bound-side pass computes on the outer's path;
divided by the group's own members for an AVG; 0 where `c'` (with its NULL tests) drops the row, and where the row is
in no G group. A square of a linear form takes the weight's root inside, so the objective stays one sum of squares (a
convex QP on both solvers); every other term takes the weight as a factor. Linear nests keep their helpers, which
stage 06 writes in (D89).

One helper per **reducer over one set of rows**: a second site holding the same reducer (`Expression::Equals`)
with the same path key, the same key scope of its own
`per`, over the same rows and under the same `per` key of its clause reuses the first helper (`Helpers::Find`),
whichever clause it is in and however it spells the reducer. The rows `RewriteAbs` derived from one clause
each hold a copy of the reducer (`aux >= sum(e) by (g) - 40`, `aux >= 40 - sum(e) by (g)`),
and two helpers pinned on different sides would let the pair drift apart; two clauses
holding the same reducer (`x <= 0.5 * sum(x) AND x >= 0.1 * sum(x)`) would say the same
thing twice. The definition belongs to no clause: it is the
definition of an auxiliary, not a constraint any clause wrote, so it carries no
source clause or removal group. A DROP of a clause that reads the helper leaves
it, which is harmless, since an equality on a helper nothing else reads always holds, and
DIAGNOSE reads it as part of the fixed model like every structural row. Each
clause that reads the helper still accounts for it in its own plan, once.

A helper records the way its readers push it (`DecideHelperDefinition::push`), which says which side pins it
(`NeedsEnvelope()`, `NeedsClosing()`), the epigraph form, from the
sign its term carries once the side is read as a sum — through `+`, `-`, binder casts and
the factors on it (`ScaleSignAtPlanTime`; 0 for a data factor of unknown sign or an
enclosing expression; a `BOOL` decision reads +1 and `-b` -1, since it is never negative,
`BooleanDecisions`) — and the comparison, read as an `ExtremumPush`. A SUM, AVG or MAX pushed
down needs `value >= members` (a MAX's envelope), pushed up `value <= members`; a MIN is the
mirror (its envelope is `value <= member`, its closing side `value >= some member`, with
indicators). An unknown sign, `=` or `<>` need both, the equality for SUM/AVG. A shared
helper takes every side any of its rows needs. Nothing reads a helper's value but the
clauses, so a slack value is harmless, and a clause over a square keeps its convexity.

**A `BOOL` factor on a reducer** (`b * sum(x)`, stage 04 §3.7) leaves a product `b * helper`
once the reducer is replaced. The pass hands it to `FindAndReplaceBilinear`, so the pass
order stays: one linearizer for every Boolean product. A decision's sign is not otherwise
known, so only the `BOOL` factor keeps the push.

**The objective** is walked the same way when stage 04 gave its record the VALUE shape
(a reducer under a `BOOL` factor): `MAXIMIZE b * sum(x)` replaces `sum(x)` by a helper
pushed by the term's sign in the minimization, and the product is one auxiliary the objective reads
once. A reducer stage 04 marked as holding a reducer (`holds_reducer`, D81) becomes a helper the same way,
the reducers inside it helpers keyed by their paths, so the objective reads the outermost
helper once: `MINIMIZE max(per g: sum(x) by (g))` is a helper per `g` and one over them.
A reducer a function reads (`0 * sum(x) + abs(sum(x) - 2)`; any node but a sum, a product or a cast) becomes a helper
the same way, as a reducer inside a comparison is the VALUE shape: the function reads one value, and `RewriteAbs`, which
runs later, would otherwise leave a raw aggregate in its envelope rows, which stage 06 reads per row.
`sum([when c] per K: inner([when c'] e) by (G))`, `inner` a SUM or AVG with no `per` and
no reducer inside, every element of `K` in `G`, and no function reading it, takes no helper and stays as written.
The tree lowers the nest, and the linear form reads it as terms (`decide_rewrite::ReadNestInPlace`, R35): each `G`
group is one member, so the sum of the members is
`inner` over every group, `c` joined to `c'`, and a square inside it stays a quadratic
objective (an AVG divides by its group's size, `avg_group_key`). A row in no `G` group --
a NULL in an element that groups NULL as no group -- is no member of any, so the sum read in
place tests each such element `IS NOT NULL` (D5). One a function reads (`abs(sum(per g: sum(x) by (g)) - 4)`)
takes its helper like any nest: read in place, it would stay a raw aggregate inside the function's rows. A reducer no
decision multiplies, no function reads and none nests in stays in the objective. A factor declared per row is
refused by the objective's scope check.

A body that multiplies decisions (a square, a product of two non-Booleans) is defined per row
by its definition F node, so the helper's members are linear; the model class decides whether
a backend takes the rows.

Every auxiliary decision column the passes allocate — an ABS auxiliary, a McCormick product, an L0 and an `IN`
indicator, a value helper — goes through `AddAuxiliaryDecision` (`decide_optimizer_internal.hpp`): the column
reference, its Boolean flag, its scope (recorded only once some decision is not row-scoped, the convention every reader
follows) and its label (F6). The label is the expression the column stands for, rendered as a plan prints it
(`RenderForPlan`, which sees the labels of the auxiliaries earlier passes made): `abs(e)` under the name the call was
written with (`@(e)`), `(b * x)`, `(e <> 0)` for an L0 indicator, `(x = v)` for an `IN` indicator, `sum(e) by (g)` for
a helper. Each rewrite also records what it
made of a construct as a fact (`AddRewriteAccount` → `LogicalDecide::rewrite_accounts`:
the subject as the plan prints it and a `RewriteAccount::Kind`, under the clause id or
the objective): the AVG scaled into a sum, the `norm` lowerings (a sum of absolute
values, a sum of squares, a maximum of absolute values), the ABS auxiliary, the Boolean
product (AND rows or McCormick), the `IN` and L0 indicators, the value helper (per group
or query-wide, a MIN/MAX's among them), a MIN/MAX in its cheap direction (a bound on each member), and the `<>`
disjunction. The renderer words them in one place
(`RenderDecideRewriteAccount`, `../03_logical_plan/done.md`). The passes start the
objective's walk with the objective's origin (`DecideRowOrigin::OfObjective`), which
every row they add for it takes, so EXPLAIN can print
those rows under the objective.

### MIN/MAX — one route

**Every MIN or MAX over decisions is a helper decision,** but for the easy identity below. In a constraint of any shape
and in the objective alike, `RewriteScopedReducers` replaces each site by a helper with one instance per `by` group (the
value rewrite's own helper, above), and the clause or the objective reads the helper where the reducer stood. A SUM or
AVG beside it stays in the clause's aggregate row; in the VALUE shape it becomes a helper too. So `sum(a*x) - max(b*x)
<= 40` is one row over the SUM's terms and the MAX's helper, and `MAXIMIZE sum(x) + max(b*x)` is an objective over both.
Nothing about a MIN or MAX depends on the terms beside it, on a `by` key or on a bound that is not a constant, so no
shape takes a route of its own.

**The helper's definition is one-sided**, the epigraph auxiliary of the Plaisted–Greenbaum encoding: the side its
readers push it against. Which way a clause pushes a term — down under `<=` for a positive term, up for a negative one,
the mirror under `>=`, nowhere under `=`, `<>` or an unknown sign — is read once, in `ExtremumPushOf`
(`decide_cast_policy.hpp`), by the easy identity and the value rewrite alike. The helper's definition stores it (`push`), a reader that
shares the helper widens it (`JoinExtremumPush`: the push where the readers agree, none where they do not), and
`NeedsEnvelope()` and `NeedsClosing()` read the sides off it. A MAX pushed down and a MIN pushed up are the cheap directions: the envelope alone holds (`helper >= member` for
a MAX, every member bounds it). Pushed the other way a MIN or MAX needs the closing side (some member reaches the
helper), which stage 06 states with indicators or natively; with no push it needs both. The objective is a minimization,
so its push is down times the term's sign: `MINIMIZE max(x)` takes the envelope, `MAXIMIZE max(x)` the closing side. A
factor stays outside the reducer (§3), so a negative one reverses the push and an unknown one gives none; a factor of
exactly zero reads as positive. Stage 06 emits one extremum link per group
(`EmitExtremumLink`, [`../06_model_formulation/done.md`](../06_model_formulation/done.md)); a shared helper takes every
side any of its readers needs.

**`<>` on a MIN or MAX** needs both sides and is read on the helper, as on any decision (`RewriteNotEqual`, below).

**The easy identity — `RewriteMinMax`.** One identity holds before any helper is made, because it is a rewrite to other
clauses and no formulation: `max(e) <= K` holds when every member does, and `min(e) >= K` the mirror, so neither needs a
helper or a Big-M. The comparison keeps its reducer and its clause is marked `minmax_easy`, and the linear form emits
one row per member. The reducer's `when` picks the members, while a bound-side reducer is reduced over every kept row of
the `by` group; hoisting the `when` onto the clause would reduce the bound over the members only. A `max(x) <= K` over
one decision with no `when` and no reducer filter is then a column bound, which bound absorption folds into the box
(below). The direction includes the term's sign and its factor's: `-2 * max(e) >= -8` is `max(e) <= 4`, the identity,
and `max(e) >= K` is not. An equality splits as BETWEEN does: `max(e) = K` is the easy half per member, a copy of the
comparison under the clause's `when` and `per K:` (`ScopeRowPerClause`), and the `>=` half, which stays in the clause
for the helper. A factor whose sign is known only when the query runs gives the term no direction, so the clause takes
the helper on both sides. The value rewrite skips a `minmax_easy` clause; definition F-node preparation does too, since
the easy half bounds each member in its own row.

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
deliberately no constant-free counterpart: it would be invalid wherever a family's rows
carry different constants (`../06_model_formulation/done.md`
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
| a value helper (a MIN or MAX's and a nested reducer's among them) | its group's members | each instance its own group's reach (`DeriveReducerValueBounds`) |

A helper over group sums does *not* share the per-row family: a group sum leaves any
single row's range as soon as the group holds more than one row. Its box is read off its
members' boxes, so an outer helper over inner helpers is boxed by theirs, and a bound
written only at the root flows back down a nest (`DeriveReducerMemberBounds`, stage 06).

Why every auxiliary gets its box: a free continuous column leaves the root simplex with
no box, so it walks to the answer one pivot at a time and the walk lengthens with row
count. Q9 (`MAXIMIZE MAX`) at 15K rows takes 27,293 root simplex iterations over 28.75s
with a free auxiliary and 61 iterations over 0.02s with a boxed one, at identical node
count, gap and optimum.

### `<>` — `RewriteNotEqual`

Each `COMPARE_NOTEQUAL` is accounted for, not rewritten: the pass records the clause as the plan prints it
(`sum(ship) by (D.region) <> 20`) as a rewrite account, and tags nothing. Stage 06 reads the kind off the comparison
(`COMPARE_NOTEQUAL`), allocates the binaries and emits the disjunction rows once bounds are known.

The two rows a `<>` becomes hold a linear left side. The pass runs after
`RewriteBilinear`, so a product with a `BOOL` decision is already an auxiliary; a square
or a product of decisions still on the left (`DecideDegreeOf` above 1) has no
formulation and is refused here (*"<> over a product or a power of decisions is not
supported"*, at the comparison). The test is `RequireLinearOperand`, which an ABS's operand takes too (see
`RewriteAbs`, above). This is the only check: stage 06 does not repeat it.

### AVG's account — `AccountAvg`

A decision-bearing `AVG` in a clause or the objective stays an `AVG` (R33): its kind tells the linear form to mark its
terms `avg_scale`, so coefficient evaluation divides them by the number of rows the reducer reads in its
group: the kept rows of its `by` group, less those its own `when` drops, one per class
under its own `per`. The pass records the `AVG_SUM` account EXPLAIN prints, and changes no expression, so nothing is
canonicalized again. A **decision-free** `AVG` is a value execution computes and has no account. Keeping the mark on
objective terms is what makes a mixed `AVG(a) + SUM(b)` preserve true AVG
semantics.

Under `<>` the division gives fractional coefficients, which the `<>` expansion's integer step refuses. So the linear
form keeps a left side whose linear terms are all AVG as sums and stamps the clause
(`DecideConstraint::avg_scales_bound`, `KeepAvgAsSumUnderNotEqual`): execution multiplies each group's bound by the
group's count. A mixed left side keeps the division.

### Range frames — `RewriteFrameRanges`

A range over decisions, `agg(from F1 to F2 ...: e) over (...)` (`../../language/guide.md` §5.6, D78), is a reducer
whose groups are the frame's **windows** -- the range several anchors share: one per timeline when neither end depends
on the anchor, one per position (peer) otherwise. `decide_rewrite_frames.cpp` replaces each by a helper decision with
one instance per window (a keyed scope of its own, its key the frame's timeline key for a
plan to name it by; stage 08 numbers the windows from the frame map) and a `REDUCER_VALUE` definition whose reducer is the frame's marker, one per
frame however many copies the clause holds. It goes through the value rewrite's door (`AddValueHelper`): the helper's push is read off the way its comparison pushes it, through `+`, `-`, data factors
and enclosing SUM, AVG, MIN and MAX (`WalkPolarity`, `CollectRangeSigns`), both sides under a norm (D79). It runs first among the
rewrites, so ABS, `norm`, `<>`, MIN/MAX and the value rewrite each read the helper as a keyed decision and the canonical
shape stands. A reducer's own `when` keeps the copy of the range its NULL test holds (stage 04), which the linear form
reads as the frame's DEFINED column. The value rewrite's helpers no longer stand at their own link's index
(`Helpers::links`): a range's link comes first. A row a later rewrite adds for a range's helper stays one per row
(`ScopeRowPerInstance`): no key's classes name its windows, and the rows of one window say the same. A range over data
stays a value of the row, which the linear form makes one REDUCED column.

### Singleton-row folding — `FoldSingletonRows`

`x <= 10` is one fact about a column, so it belongs in that column's box rather than
in `num_rows` identical model rows. A box is also strictly better for the solver:
smaller model, tighter presolve. The pass folds every bound on one decision that is known
before the query runs (a `BETWEEN` arrives as its two comparisons, split by the binder)
into one `DecideBoundBox` per decision (`singleton_row_boxes` on `LogicalDecide`), and
marks the clause `folded_bound` so physical extraction skips it.

A clause that skips rows does not fold: a `when` holds on some rows only, and a `per` whose key holds a written
column skips each row whose key is NULL (`language/guide.md` §6.2), which no box over the whole column says. Each
stays a model row, `per t.g: x <= 10` among them, whether or not a key is NULL: the pass runs before any row is read.

A comparison that holds a frame selector is no bound on one decision: it reads another row, or nothing, which no
interval over the row says. A comparison a rewrite left holding an auxiliary alone (`abs(x - at(previous: x) over
(...)) <= K` as `a <= K`) does fold: where the frame has no value the auxiliary's definition is skipped, so the box
binds nothing there (D88).

**A helper stands in for its function with the function's type** (`DecideTermSplitter::ReplaceTaggedFunctions`,
which singleton-row folding and `IsLinear` call). A function the optimizer gave a helper (an ABS, a definition) is
read as its helper cast to the function's type: the casts around the function are bound to that type, and a
helper's is its decision's, so an INT decision's `abs(x - 2)` is a BIGINT and its helper an INTEGER.

**One classifier reads every bound on one decision** (`DecideReadSingletonRow`,
`decide_singleton_row_folding.hpp`). This pass, the linear form (a data bound, stage 06)
and direct solve all call it, so the paths cannot disagree about which values a bound
admits. It follows MIP presolve: a row on one variable becomes a column bound, and an
integer variable's bound is rounded inward. A comparison is a bound on one decision when
the term splitter reads its decision side as `a * x + b`: linear terms of one decision over
decision-free coefficients (`x`, `2 * x`, `cap * x + w * x`, `0.1 * x`), whose coefficients
add up to `a`, and constant terms, which add up to `b` and move to the right side. An easy
`max(m) <= K` / `min(m) >= K` (`BoundDecideClause::minmax_easy`) is read as its row: the extremum
of one member is that member, so the reducer is replaced by its member (`2 * max(c * x)`
reads as `2 * (c * x)`). The canonicalizer has already moved a clause's shifts across and
merged like terms, so `x - 1 >= -3`, `2 * x >= -4`, `-x <= 2`, `2 * (x - 1) >= -6`,
`x + x >= -4`, `-min(x) <= 2`, `max(-x) <= 2`, `min(x + 1) >= -1` and `x >= -2` are each
the column bound `x >= -2` (D41, read by value).

The classifier returns the **interval of values the clause admits**, as two DOUBLE
expressions over the source row (±infinity on an open side). The interval is what DuckDB
says. A whole-numbered decision admits `n` exactly when the written comparison, with `n`
cast to the decision's type in place of the decision, evaluates true: `0.1 * x <= 0.3`
admits 3 (`0.1 * 3` is the DECIMAL 0.3), while `x <= 0.3 / 0.1` admits only 2 (the
DOUBLE quotient is 2.9999999999999996). The quotient `r / a` is within one of the answer,
and the written comparison picks among its neighbours. Past the decision's range of exact
candidates (2^52 for BIGINT, the INTEGER maximum for INTEGER) the rounded quotient is the
answer. No copy of DuckDB's DECIMAL or DOUBLE rules is written, so none can drift from it.
The coefficient's sign picks the side: `-x <= 2` bounds below. A zero coefficient admits
every value or none, as `0 <op> r` evaluates. A strict `x < 10` on a whole-numbered
decision admits 9, by the same check; stage 04 refuses a strict comparison on a REAL.

A bound folds when its coefficient and right side fold to numbers at plan time. A side
that is data (a column, a subquery, a column factor) is a data bound: the linear form
hands its interval to stage 08, which evaluates it per row, and the model builder writes
it into the column box there. On the side the clause binds, an interval end of `+inf`
below or `-inf` above admits no value (`x >= 'inf'`): the clause stays a row, and the
model is answered infeasible before any solve (`SolverModel::ProvesInfeasible`). A right
side of magnitude 1e20 or more in the open direction (`x <= 1e30`) binds nothing.
The pass reads each clause record, so `per row: x >= -5` is folded like `x >= -5`. A `per K:` whose `K` holds a column the user wrote
(null mode `NO_GROUP`) is not: a row whose column is NULL there is in no class and takes no instance of the clause
(`language/guide.md` §6.2), which a box on the whole column would give it, so the
clause stays a row. The model emitter checks such a clause's rows against the decision's domain instead
(`../08_execution/done.md`, "Domain contradictions are a static error"). A relation element, which
groups NULL as a value, folds. **A `when`-guarded
comparison is never folded**: `when c: x <= 0` is conditional per row, not a domain.

An easy `max(m) <= K` / `min(m) >= K` bounds every row its reducer reads, and with no
`when` and no filter on the reducer that is every row: it is a column bound and is folded
the same way, so a negative `K` makes `x` signed, as `x >= -5` does. A row the reducer's
filter leaves out is no member: the interval there is open on both sides, so a NULL in its
member is never read, and the clause does not fold at plan time. The reducer stays in the
tree, so EXPLAIN and DIAGNOSE read the clause as written.

- The box takes the admitted ends: `<=` tightens the upper bound (`min`), `>=` the lower
  (`max`), `=` intersects both. Intersecting rather than assigning is what makes
  `x = 5 AND x = 10` invert the box and be caught, instead of silently resolving to
  whichever was written last. `x = 2.5` on an INT admits nothing and inverts the box.
- A literal floor of `-inf` writes the solver's "no bound" (-1e30) as a written lower
  bound, so `x >= -'inf'` frees `x` below as `x >= -1e30` does.

Every bound goes through `DecideFoldSingletonRow` (`decide_numeric_policy.hpp`), the same
combiner A1 and S1 call. Lower bounds start at -1e30 rather than 0, so an explicit
negative bound is honored instead of clamped up. Whether a lower bound was written is a
separate flag (`lower_written`), not a reserved value: stage 08 gives the default 0
floor only to a decision with no lower bound written, and a written `x >= -1e30` is no
bound, like any bound of magnitude 1e20 or more. A helper that holds a value of either
sign (`Signed()`: a value rewrite's reducer, a defined non-linear body, a Boolean product) is
marked written before folding, so it starts with no floor and a bound written on it still
folds in.

Folding never enters a `when`: `when c: x >= -5` does not bound `x` on every row. Such a
bound replaces the default floor on the rows it keeps instead. The linear form marks the
constraint `lifted_floor` when `DecideSingletonRow::LiftsDefaultFloor()` holds: a lower
side whose interval is a constant on every row the clause reads (`constant_on_read_rows`,
which an easy MIN with a reducer filter has although it does not fold). Stage 06 lowers
the instances those rows reach (`../06_model_formulation/done.md` §2). A1 asks the same
predicate.

**A recorded bound is flattened aside too.** The box is how a written negative bound
makes a decision signed, so every form of a bound reaches it. But a clause is one knob
per kept row or class in an expanded diagnosis (D28), which one column bound cannot say.
So the comparison of every recorded bound (not a BOOLEAN restatement, below) is also
marked `folded_clause_rows`, and the linear form lowers it into
its root, a folded one (`DecideNode::folded`), instead of skipping it, throwing an
`InternalException` if their count differs from `user_singleton_row_bounds` (the repair model
would open the column with nothing enforcing it). Stage 08 evaluates those under DIAGNOSE
only and hands them to the repair model alone: they are its only rows for a folded
bound.

**Upper bounds start at the declared type's ceiling**, not uniformly at 1e30: a
BOOLEAN variable is seeded to `1.0`. The box is what stage 06 receives as
`SolverInput::upper_bounds`, and every Big-M derivation reads it through
`DecideRowTermRange`, which treats `>= 1e20` as unbounded. Seeding only the sentinel
would leave a declared `BOOL` looking unbounded to all of them, and an unbounded
contributor is refused, so `SUM(x) <> 2` over four BOOL decisions would be refused
outright.

Seeding it here is safe because folding only ever narrows (`min`), and because a
user restatement like `x <= 1` on a BOOL is a no-op against the intrinsic box. It also lets the `<>` range collapse see a BOOL's upper side:
`SUM(x) <> 9` over four BOOL decisions is unreachable and drops
to nothing instead of emitting a disjunction against a bound no assignment can meet.

Corpus query 85 pins the tight constant; queries 82–84 either collapse or use INT
decisions and so do not exercise it.

Each recorded bound is a `UserBoundSpec` in `user_singleton_row_bounds`: the bound as
written moved onto the decision (`r / a`, a strict one stepped by `DecideStrictBound`) and
its clause ids. Stage 08 re-opens its direction in the rigid box and in the elastic box,
and checks the box of the written bounds against the variable's domain, quoting the clause
that contradicts it. The written bound, not the admitted end, is what that check reads:
`x = -0.5` on an INT admits nothing, which the solve reports as infeasible, rather than
a domain refusal. A
variable's *intrinsic* domain is excluded: a BOOLEAN's `[0,1]` box is never
synthesized as a constraint, so it only appears here when the user restated it
redundantly, and `is_boolean_var` is consulted to skip that restatement. A genuine
BOOLEAN pin (`x <= 0`, `x >= 1`) does tighten the box and is recorded.

**Why it runs last.** `RewriteInDomain` emits a floor-lowering `x >= min_value` for
an all-constant `IN` domain whose minimum is negative — itself a foldable shape.
Fold before that pass and the floor stays at 0, the negative domain value becomes
unreachable, and the solver returns a *different answer* rather than an error.
Running last also guarantees every auxiliary variable created by `RewriteAbs`,
`RewriteBilinear` and the value rewrite already exists before the box is sized.

**Why here and not stage 04.** "Bound or row" looks like shape, which stage 04 owns,
but stage 04's contract rules it out three times: that pass is pure (it returns a new
tree; folding writes into bound arrays), it is total and never declines
(folding leaves every comparison that is not a bound on one decision as it is), and §5 explicitly excludes choosing a
solver formulation, which bound-versus-row is. Stage 05 already chooses formulations
with types in scope and is forbidden only from evaluating *data* — a foldable literal
is not data.

**Why the clause is marked rather than replaced.** `RewriteInDomain`
replaces a lifted clause's body with a `TRUE` placeholder, but that is the wrong
precedent here: `EXPLAIN` renders the constraint list from the records, and the user
wrote this bound. The mark (`folded_bound`) moves the decision upstream without changing what the user
sees.

---

## 3. A factor on a reducer stays outside

The canonicalizer peels a query-wide factor *outward* off a reducer
(`2 * SUM(x*p)`, `2 * MAX(x*v)`) and converges every spelling onto one. A decision cannot
be that factor: stage 04 refuses `b * SUM(x)` (*"'b' is a decision, so it cannot multiply
SUM(x)"*). **No pass here pushes a factor back in.**

`MIN`/`MAX` are order statistics, so a negative factor turns one into the other —
`MAX(-2x)` is `-2·MIN(x)`. Folding therefore needs the factor's sign, which an
uncorrelated scalar subquery does not supply until the query runs. Kept outside, a known sign decides which side of
the helper the clause needs, and an unknown sign is safe: the helper is pinned from both sides. `ScaleSignAtPlanTime`
(`decide_cast_policy`, shared with the canonicalizer) returns +1, -1, or **0 for "not
known until the query runs"**, and 0 must be treated as "take the expensive but exact
form", never as an error. A scale of -2 flips the direction the helper is pushed exactly as a
subtraction would, so a known sign enters `ExtremumPushOf` alongside the term's sign; an
unknown sign gives no push, and both sides pin the helper to the true
MIN/MAX — correct whichever sign it turns out to have.

The clause row reads the factor times the helper. The linear form multiplies a factor on a reducer into every term the
reducer produced (`ApplyScaleToExtracted`, §1a), and stage 08 evaluates it.

Folding without the sign check would be a silent wrong answer.

---

## 4. Optimizations not in this pass

Two things reduce problem size before the optimizer runs, and are worth knowing
because they dominate everything above:

- **`WHERE`-clause filtering** — standard DuckDB predicate pushdown, no
  DECIDE-specific code. Rows eliminated by `WHERE` never become decision
  variables, which for most queries is the single most impactful optimization
  there is. A filter written above a DECIDE stays above it: join ordering treats DECIDE as one relation, as it does a
  window (`relation_manager.cpp`), so a parent query's `WHERE` over a DECIDE whose source holds a join never moves
  into that source, where it would change the problem.
- **`when` filtering** — stage 08. Nothing is computed on a row a `when` drops: a
  reducer reads only the rows its clause's `when` and its own keep, a per-row clause
  emits no row there, and the objective's `when` folds into every term's filter. A
  dropped row is in no group, so no row the clause emits reads it.

Backend selection (Gurobi preferred, HiGHS fallback) and the shared time limit are
stage 07.

---

## 5. What this stage does not do

- Judge the polynomial degree of an expression, or any other algebra — the shape check, stage 04
  (`../04_canonicalizer/done.md` §3.11), which owns the one definition (`DecideDegreeOf`) and refuses degree > 2 on the
  canonical tree before any pass here runs. This stage *calls* that definition, never a copy of it. The term
  splitter reads the one spelling and never refuses; a decision-bearing term it leaves opaque and that reaches term
  extraction is an internal error. The passes here synthesize expressions the shape check never saw (the `IN`
  expansion, ABS linearization, `norm` lowering), which is why extraction asserts. A product with an `ABS` factor
  (`ABS(x) * y`) is read after `RewriteAbs` has replaced the `ABS` with its auxiliary, so it is a product of
  decisions like any other.
- Decide which side of a comparison a term sits on — stage 04. Every pass here keeps
  to that: ABS and bilinear replace decision-bearing atoms with
  decision auxiliaries; MIN/MAX and AVG keep decision terms on the left; `<>`
  changes only metadata; `FoldSingletonRows` only annotates.
- Evaluate anything against data, or emit solver rows. Flattening produces
  coefficient *expressions*; turning them into numbers is stage 08.

---

## 5a. Arms no test reaches, kept (BLOAT-29)

Each is a precondition on an earlier stage's promise ([`../architecture.md`](../architecture.md) § 3) or a typed read:

- **The binder.** One scope per declared decision, and every rewrite adds a scope with its decision
  (`AddAuxiliaryDecision`); a
  reducer's marker covers one expression (`RewriteNorm`); a clause's record holds its
  source clause (`origin.source_clause_id`), which `TagAtomicRemovalGroups` reads (`SourceClauseOf`) to find its clause.
- **The shape check.** A side of a degree-2 product is affine and no divisor holds a decision
  (`DecideTermSplitter::Split`); a factor's form is linear (`AppendLinearTerms`); a data factor has power 1
  (`FindAndReplaceBilinear`).
- **The canonicalizer.** A NORM marker is closed (`TryParseNormMarker`); the IN marker is built over a decision with a
  domain (`RewriteInDomain`); a canonical class is never `INVALID`.
- **Earlier rewrites.** Every reducer is a SUM before extraction; the value rewrite replaces every `VALUE` clause;
  `RewriteMinMax` keeps an easy clause's reducer; no NORM marker and no non-linear extremum member leave stage 05
  (`VerifyNormMarkersLowered`, `VerifyExtremumBodiesLinear`); the singleton-row bounds parallel the prepared
  constraints; `AddLowered` is asked for
  `>` and `=` only; a bound's upper end folds when its lower does (`FoldKnownBound`).
- **The solver gate.** A non-convex class has an objective group to name, a quadratic gap a quadratic row to quote, and
  `DescribeGap` is asked only for an unmet class.
- **Typed reads.** An `opaque` term has no coefficient to read; a null coefficient means 1; `FLOAT` beside
  `DOUBLE` in `IsIntegralColumnType`, as the model builder reads integrality; a data column beside a product of
  decisions in `ReadsOnlyQueryWideDecisions`.

## 6. Source map

| Concern | Location |
|---|---|
| The pass dispatcher and shared helpers | `src/optimizer/decide/decide_optimizer.cpp`, `decide_optimizer_internal.hpp` |
| `norm` / `IN` | `src/optimizer/decide/decide_rewrite_norm_in.cpp` |
| `<>` / AVG→SUM | `src/optimizer/decide/decide_rewrite_notequal_avg.cpp` |
| ABS | `src/optimizer/decide/decide_rewrite_abs.cpp` |
| The easy identity of a MIN or MAX | `src/optimizer/decide/decide_rewrite_minmax.cpp` |
| The value rewrite (VALUE-shape reducers to helper decisions) | `src/optimizer/decide/decide_rewrite_scoped.cpp` |
| Bilinear McCormick | `src/optimizer/decide/decide_rewrite_bilinear.cpp` |
| Non-linear reducer body F-node preparation | `src/optimizer/decide/decide_tree_definition.cpp` |
| Singleton-row folding | `src/optimizer/decide/decide_singleton_row_folding.cpp` |
| Linear-form flattening | `src/optimizer/decide/decide_linear_form.cpp` |
| Backend selection and the model-class gate | `src/optimizer/decide/decide_solver_gate.cpp` |
| Pass inventory and helper contracts | `src/include/duckdb/optimizer/decide/decide_optimizer.hpp` |
| The prepared form's structures | `src/include/duckdb/planner/decide/decide_tree_terms.hpp` |
| Where flattening is triggered | `src/execution/physical_plan/plan_decide.cpp` |
| Metadata the passes write | `src/include/duckdb/planner/operator/decide/logical_decide.hpp` |
| Canonicalizing entry points | `src/planner/operator/decide/logical_decide.cpp` |
| Per-function user-facing semantics | `../../language/functions.md` |
| `per`, `when` and `by` semantics | `../../language/scopes.md` |
| Bilinear semantics | `../../language/problem_types.md` |
