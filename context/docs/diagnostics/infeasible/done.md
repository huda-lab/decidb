# Infeasible

> Router terminal: **failed → infeasible** (`elastic` → report). See `../router/done.md`.

The feasible region is empty — no assignment satisfies every `SUCH THAT` constraint at
once. Under `DIAGNOSE`, the infeasible terminal builds a second
optimization, the **elastic program**, whose optimum is the least-change fix: which user
constraints to loosen, and by how much. Shared plumbing it builds on (the statement trigger,
provenance, and reporting relation) is in `../foundations/done.md`. The engine has a
lexicographic repair ladder, an objective-best re-optimization of the clauses it drops, and a
rank tie-break over **every** repair kind (removals, editable loosens and data offsets, with or
without an objective), so both backends name the same clause on a repair tie. Open work is in
`todo.md`.

## Engine seam: infeasible

The seam feeds the elastic engine only under `DIAGNOSE`.
`PhysicalDecide::Finalize`'s `DiagnosisTerminal::INFEASIBLE` arm calls
`DiagnoseInfeasible` (`decide_diagnostic_engines.cpp`), mirroring the unbounded arm. A
valid diagnosis is handed to the `PhysicalDecideDiagnose` operator above it; when the
engine cannot name an actionable relaxation, the same statement returns one
`edit_source='undiagnosed'` finding instead of an empty relation. An unprefixed failure
never enters the arm and raises the state-only `ThrowDecideSolveError`. A residual
`INF_OR_UNBD` (empty ray) is normalized to `INFEASIBLE` before the engine runs.

**Engine boundary (mirrors `DiagnoseUnbounded`).** `InfeasibleDiagnosisInput`
(`decide_diagnostic_engines.hpp`) is the unbounded input with one structural swap: it
carries the built **`SolverModel`** (the elastic transform will reshape its rows) in place
of a solved ray, alongside the `VarIndexer`, per-variable labels / is-aux flags,
`DecideDiagParams`, and an **injected solve callback**
`std::function<SolverResult(const SolverModel &)>`. The callback lets the engine run the
elastic re-solve without depending on the operator or the solver facade — `Finalize` binds
it to the prepared-model solver on the primary solve's backend (`SelectSolverBackend` is
deterministic, so the re-solve uses the same backend), with the diagnostic helper budget
`min(60s, primary solve limit)` and the same interrupt poll as the primary solve. If any
elastic pass hits that helper budget, the operator reports that diagnosis ran out of time
instead of hiding the attempt behind the generic static error. This keeps the engine
solver-agnostic and unit-testable, the same boundary as the unbounded engine's
`get_candidates` injection. No clause→row index is carried in the input — the engine reads
each row's `provenance` directly off the model.

**Model retention.** `SolveModel` builds the `SolverModel` as a local and discards it —
only the `SolverResult` returns — and `SolverModel::Build` *moves* the global constraints
out of the `SolverInput`, so the base model is gone by the time the arm runs and cannot be
faithfully rebuilt from the gutted input. To give the engine a model to transform,
`SolveModel` takes an optional out-param **`SolverModel *retained_model`**
(`ilp_solver.hpp`): `Finalize` passes it only when diagnosis is armed, and the built model
is moved into it after the solve completes (the post-solve disambiguation / ray helpers
stay file-private inside `ilp_solver.cpp`, so all solve orchestration keeps its single
home). The retained model is freed when `Finalize` returns.

**Labels.** Per-decide-variable labels and is-aux flags (for column provenance) are built by
one `build_var_labels` lambda in `Finalize`, shared by the UNBOUNDED and INFEASIBLE arms.

## Pre-solve verdict: a bound no assignment can reach

`DiagnoseInfeasible` answers one question before it builds the elastic program: does any
row demand `Ax >= +inf`, `Ax <= -inf`, or `Ax = ±inf`? A finite left-hand side never
reaches such a bound, so the row is infeasible on its own — no other clause is implicated
and no finite loosening closes the gap. `CollectUnreachableClauses` names every such
clause and `BuildUnreachableBoundDiagnostic` reports them; the elastic model is never
built.

**Why it cannot be left to the elastic solve.** The elastic transform wires slack into the
*left* side (`wire()`: `Ax − s ≤ b`, `Ax + s ≥ b`), so `b` is never touched. With `b = ±inf`
there is no finite `s` that repairs the row, and the slack column is capped at the `1e30`
sentinel. The solver then does one of two things, neither of them true: it saturates the
slack at `1e30` and reports it as a repair — rendering the offset back as `inf`, so the
"suggested change" is the text the user already wrote and the "amount" is an internal
sentinel — or it finds the elastic program infeasible and falls to
`BuildElasticInfeasibleDiagnostic`, which names no clause and blames "a fixed part of the
query" when the conflict is in a `SUCH THAT` clause. The verdict is structural, so it is
taken structurally.

**Direction decides.** Only the out-of-reach spellings are findings. `Ax <= +inf` and
`Ax >= -inf` are vacuous — they constrain nothing and can never be why a solve failed — so
`IsUnreachableBound` classifies by the direction the infinity points, the same rule
`ClassifyMinMaxBound` applies upstream.

**Which rows are read.** The gate is *does this row trace back to a clause the user wrote*,
not *is it elastically editable*. They are different questions: a hard MIN/MAX clause is
stamped `USER_MECHANISM` wholesale at `decide_linear_form.cpp` (it *will* fan into Big-M
rows), so a relaxability gate would miss `MIN(x) <= -inf` entirely. Only `STRUCTURAL` rows
are excluded, as internal definitions with no user text to quote. Reading a
`USER_MECHANISM` row is safe because a helper's bound is derived as `K ± M` with both terms
finite by construction — `ClassifyMinMaxBound` sends a group to the Big-M rewrite only when
`K` is finite, and `DecideTightPerRowBigM` refuses a non-finite `M` — so an infinite bound
on a mechanism row is always a user bound re-emitted per row.

**Both backends give this verdict, because neither reads the row.** HiGHS spells a one-sided
row bound by pairing the user's bound with its own ±1e30 infinity sentinel, so `Ax >= +inf`
would arrive as `lower = +inf, upper = 1e30`, an inverted pair `passModel` rejects. A model
with such a row has no solution and needs no solver to say so, so `SolveModel` answers it
INFEASIBLE before any backend loads it (`SolverModel::ProvesInfeasible`, which reads the same
`IsUnreachableBound` predicate) and retains the model for this diagnosis. An unprefixed query
reports the ordinary infeasible error on Gurobi and HiGHS alike; a vacuous infinity pairs
cleanly with the sentinel and solves on both.

**Output shape.** One finding per clause with `edit_source='unreachable_bound'`;
`clause` names the clause and `suggested_change` explains that no assignment can reach the
bound, and which way it moves: a bound at `+inf` is lowered, one at `-inf` raised
(`UnreachableClause::too_low`). `amount` is NULL because there is no finite repair
magnitude. A clause with a `per` key reports once per class it names, the key value in
`group`, in either scope; one with no `per` key reports once in the `query` scope and
once per kept row (`row`) in the `expanded` one (`FanOutByClass`, below). A clause fans
into one model row per relation row, all rendering the same text, so findings are
de-duplicated on `(label, group, row)` — the user wrote one clause and reads one
finding, or one per class. Clause text comes from the same `MakeClauseLabel` the elastic
edits use (factored out of `MakeLoosenEdit`), so a pre-solve finding and a post-solve
edit name a clause identically, its `when c per K:` qualifier included.

## Elastic engine: the stage-1 core and the elastic-infeasible signal

On an infeasible solve under `DIAGNOSE`, `DiagnoseInfeasible` builds and
solves a **second** optimization — the *elastic program* — whose optimum is the
least-change fix. Each relaxable user constraint gets a non-negative slack that lets its
RHS stretch. Drop-only source clauses are handled outside the elastic program: the engine
enumerates source-level removal groups by cardinality and physically omits a candidate set
before elasticizing the retained model. Within one cardinality layer, data-RHS offsets and
then editable source-literal loosening are minimized lexicographically.

```
outer:     min |G|                                (omitted source groups)
stage 1D:  D* = min Σ s_data       for fixed G
stage 1E:  E* = min Σ αᵢ s_edit    subject to D ≤ D*, fixed G

           Aᵢ x ≤ bᵢ + sᵢ ,  sᵢ ≥ 0              (relaxable rows + bounds)
           structural / mechanism rows rigid unless their whole group is omitted
           →  final support names LOOSEN edits; G names amount-less DROP edits
```

**Operator / engine split.** The engine is solver-agnostic and free of DuckDB
`Expression` types, so the work splits across the engine seam:

- **Operator (`physical_decide.cpp`).** Knows the `Expression`s and the sink state, so it
  handles **folded bounds**. A user `x <= 10` / `x BETWEEN a AND b`, or an
  easy `max(x) <= 10` with no `when` and no reducer filter (D41), is folded into the
  column box by `DecideOptimizer::FoldSingletonRows` (`decide_singleton_row_folding.cpp`)
  and becomes no row of the solved model, so on its own the row-based engine could not
  see it. The pass records each folded bound as a `UserBoundSpec {decide_var_idx, sense,
  k, strict, typed_k, source_clause_id, removal_group_id}` (a comparison → one spec, a
  `BETWEEN` → two) and tags its comparison `SINGLETON_ROW_CLAUSE_ROWS_TAG` beside
  `SINGLETON_ROW_BOUND_TAG`. Stage 05's linear form flattens such a clause aside
  (`DecidePreparedModel::singleton_row_constraints`, one per recorded bound, which it
  asserts); execution evaluates those clauses under `DIAGNOSE` only, numbered after the
  prepared clauses, and only the repair model carries their rows: one per kept row, or one
  per class under a `per K:` key, each `USER_PARAMETER` and, for a literal bound,
  `SHARED_SCALAR`. In the `query` scope the rows of one bound share one slack (the knob is
  one literal `K` → report the max overshoot); in the `expanded` scope each
  kept row or class reports its own (D28). `OpenElasticColumnBox` then widens the column
  in each folded direction, so the bound is enforced only by its loosenable rows.
  Default non-negativity (`lower = 0`) and the BOOLEAN `0/1` box stay rigid: a BOOLEAN
  variable's domain is applied directly to the solver column by `PhysicalDecide::Finalize`
  (never synthesized as `x >= 0` / `x <= 1` constraints — its runtime DuckDB type is still
  INTEGER, so it participates in arithmetic, but its *solver-facing* type is reported as
  BOOLEAN from `op.is_boolean_var` alone). Since the domain is never a constraint node,
  singleton-row folding only ever sees one if the user wrote it; it skips recording a BOOLEAN bound
  **only when it merely restates the domain** (an upper `>= 1` or a lower `<= 0` after
  integer-strict normalization, `ShouldRecord`). A genuine BOOLEAN **pin** (`x <= 0`,
  `x >= 1`, `x = 1`) IS recorded and reaches the repair model like any other user bound —
  its column opens only back to the intrinsic `[0,1]`, so the pin becomes loosenable while
  the 0/1 domain itself stays rigid. (Erasing pins wholesale would make the elastic model
  diverge from the user's query: silently missing diagnoses, or edits that leave the real
  query infeasible.) Reversed user bounds such as `5 >= x` are flipped by
  `DecideCanonicalizer` before folding, so they follow the same path as `x <= 5` and
  are reported as `x <= 5`.
- **Engine (`decide_diagnostic_engines.cpp`, `DiagnoseInfeasible`).** Pure model math.
  Copies the model, clears the user objective for the repair passes (zeroes linear objective,
  drops the quadratic objective, `maximize=false`), and adds a slack to every relaxable **linear**
  row (`IsRelaxableForElastic` ⇒ `USER_PARAMETER`). The active repair objective is installed
  per lexicographic tier. Quadratic rows and
  `STRUCTURAL`/`USER_MECHANISM` rows stay rigid; ABS pin rows
  are already `USER_PARAMETER` and are picked up automatically while their envelopes
  (`STRUCTURAL`) stay rigid. **Slack direction:** `≤` → `−s` (`Ax − s ≤ b`),
  `≥` → `+s` (`Ax + s ≥ b`), `=` → **two** non-negative slacks `−s⁺ + s⁻` (stays linear,
  uniform `sᵢ ≥ 0`). **Slack type:** REAL, `≥ 0`, uncapped, even for an
  integer RHS. It re-solves through the injected `solve_model` callback (same backend as the
  primary solve).

**Reading the result.** On `OPTIMAL`, every slack above `DIAGNOSTIC_RAY_EPSILON` is an
edit; the amount is the slack value (`=` reports the net `s⁺ − s⁻`). Each clause is labelled
from the source registry when the clause has an entry there (`SourceAwareLhs`: the written
spelling when canonicalization moved a term across the comparison, else the canonical one,
both in the plan-style echo), and otherwise by reconstructing its algebra from the
**original** row's coefficients over user-facing column names (`BuildColumnProvenance`) —
`x <= 5` → suggestion `x <= 10`. `BuildInfeasibleDiagnostic` emits one finding per edit:
`clause` = the clause as written, plus `suggested_change`, `amount`, `edit_source`, `group`
and `row`. How a fanout clause (data RHS, `per` aggregate) reads back
is governed by the slack-scope pragma — see "Slack-scope policy: query vs expanded" below.

**Elastic-infeasible signal.** If the elastic program is *itself* infeasible, the conflict
reaches rigid rows → `BuildElasticInfeasibleDiagnostic` renders a distinct outcome: one
finding, `edit_source='rigid_conflict'`, naming no clause and saying that loosening your
SUCH THAT limits cannot fix it. The verdict is honest because every user constraint is
relaxable in the repair model, folded bounds included (their clause rows, above). When
there are no relaxable rows at all, the engine returns invalid and the operator reports
`undiagnosed`.

The achievable-objective re-solve, atomic grouped removal, per-shape slack placement and
unit conversion are below.

## Slack-scope policy: query vs expanded

The `diagnose_decide_infeasible_slack_scope` session pragma (VARCHAR, default `query`,
validated at SET time in `decide_diagnostic.cpp`; read into `DecideDiagParams::slack_scope`)
answers two different user questions with the same engine:

- **`query` (default)** — *"What single SQL rule do I change?"* Every knob that fans out
  (a clause across its `per` classes or `by` groups, a data-backed RHS across its rows, a
  multi-instance cap) folds into **one** slack. The reported edit corresponds to the
  single literal the user would edit. A data RHS with no literal to loosen reports a
  **virtual offset**.
- **`expanded`** — *"Which generated rows/classes are actually tight?"* Every clause
  reports once per class it stands for (D28): a clause with a `per K:` key once per value
  of K (`edit_source='expanded_group'`, the value in `group`), a clause with no `per` key
  once per kept row (`edit_source='expanded_row'`, the row in `row`), and a `per ()`
  clause once; a data RHS stays per-row, each conflicting row exposing its exact
  overshoot. A debug/profile view, not necessarily a pasteable SQL edit.

**Mechanism (one folding rule, two block keys).** The policy lives entirely in
`BuildElasticModel` + `ReadElasticEdits` (`decide_diagnostic_engines.cpp`); the operator and
the rest of the engine are unchanged. `BuildElasticModel(base, slack_scope)`:

- **`folds()`** — a `SHARED_SCALAR` knob always folds; a `PER_ROW_DATA` row folds **only in
  query mode** (in expanded mode its rows stay independent, one slack each).
  A row that stands for its group's classes (`class_set`, an easy MIN/MAX's member rows
  included) folds within its group in both modes; a per-row row of a clause with no `per`
  key (`per_kept_row`) keeps its own slack in expanded mode whatever its shape.
- **`block_key()`** — query mode keys a block by `clause_id` alone (so all groups and
  classes of a clause collapse into one slack = the single literal edit); expanded mode
  keys by `(clause_id, group_key)` (so each `by` group or `per` class is its own slack). A
  folded data block still belongs to the data-offset lexicographic tier in both modes —
  only the *grouping* changes by mode, never the tier.

**Readback (`ReadElasticEdits`, threaded with `slack_scope`).** Per block:

- **query** — one clause-level edit with `group` and `row` NULL. A data RHS → a
  `virtual_offset` LOOSEN (`MakeVirtualOffsetEdit`:
  `x <= col + delta` / `>=` → `col - delta`, `=` by its signed slack, a strict `<` / `>`
  keeping its operator, where `col` is `ConstraintProvenance::rhs_label`,
  the RHS column name carried from the binder — fall back to the numeric representative RHS
  when absent). Any other literal knob → `source_literal`.
- **expanded** — a group's row reports once per class it stands for
  (`ConstraintProvenance::class_set` → `SolverModel::class_sets`, D28; `FanOutByClass` in
  `decide_diagnostic_render.cpp`, which the elastic readback and the unreachable-bound
  collector both read): per value of its `per` key (`expanded_group`, the value in
  `group`), or per kept row when it has no `per` key (`expanded_row`, the row in `row`);
  `per ()` (a `WHOLE_CLAUSE` class set) is one finding (`source_literal` or
  `virtual_offset`). The aggregate row of a `by` group, the hard MIN/MAX bound row, the
  easy MIN/MAX's member rows and a composed clause's row all carry their group's classes.
  A row that carries classes folds within its group in both scopes (`folds()`), so an
  easy MIN/MAX's members, bound by their group's tightest value, share one slack and the
  group's bound moves once. A per-row row of a clause with no `per` key keeps its own
  slack in the expanded scope whatever its shape (`per_kept_row`) and reports at its data
  row (`data_row`, `expanded_row`), a data RHS included; when canonicalization moved a
  term across its comparison, the finding offsets the bound as written. A bound the
  optimizer folded into a column box reports the same way through its own clause's rows
  An unreachable bound names its `per` key's values in both scopes
  and its kept rows in the expanded scope only. A quadratic row keeps its own slack
  and, in the expanded scope, reports the classes it stands for through the same
  `FanOutByClass`.

**Reporting contract.** Every LOOSEN finding carries
`edit_source ∈ {source_literal, virtual_offset, expanded_row, expanded_group}`. The
`group` and `row` columns encode the expanded scope directly. A data conflict is always
actionable: a `virtual_offset` (query) or an `expanded_row` profile (expanded), never a bare
"conflicts in M of N rows". In query mode the clause reads as the folded clause
(`per grp: sum(x) by (grp) >= 5`) with `group` NULL; expanded mode emits one finding per
failing class, same clause text, with the key value in `group`.

**An easy MIN/MAX over one decision is a column bound.** `max(x) <= K` (and
`min(x) >= K`) with no `when` and no reducer filter means `x <= K` on every row it reads
(D41), whatever its `per` key or `by`, so bound absorption folds it into the column box
like `x <= K` (`DecideReadOneDecisionBound`, `decide_singleton_row_folding.cpp`) and it
reaches the repair model through its own clause's rows. A member scaled or shifted by
constants is read by value (`max(2 * x) <= -10` is `x <= -5`). The `query` scope reports
one edit on the clause as written, by the max overshoot
(`per g: max(x) by (g) <= 2` → `per g: max(x) by (g) <= 4`); the `expanded` scope reports
it once per class of its `per` key, or once per kept row when it has none. An easy
MIN/MAX with a `when`, a reducer filter or a member scaled by data (`max(cap * x) <= K`)
stays rows under diagnosis: its member rows carry their group's classes and fold within
each group.

**Tests.** C++ structural (`test_decidb_diagnostic_engines.cpp`): `BuildElasticModel` folds a
grouped `SHARED_SCALAR` clause into one block in query mode and one per group in expanded; a
data RHS folds to one block in query and stays independent per-row in expanded. Python
differential (`test_query_diagnostics_relation.py`, both backends): data-RHS query virtual
offset (named column) + expanded per-row profile; `per` aggregate query fold (one clause
edit) + expanded per class; option validation for the pragma. TPC-H
(`test_query_diagnostics_tpch.py`): `per l_orderkey: sum(x) by (l_orderkey) >= 5` folds in
query and breaks out per failing order in expanded; `x >= l_quantity` reports the named
virtual offset in query and per-row in expanded. The per-class readback — a `per` key finer
or equal to the `by` key, no `per` key (per kept row), `per ()` (once), a `when` in front, a
bound folded into a column box, a keyed decision's bound, an unreachable bound, a
quadratic clause — is pinned in `test_scope_explain_diagnose.py`
(`../../testing/scope_suite.md`).

## Elastic engine: per-shape slack placement (shared blocks, `per` classes, multi-instance bounds)

Slack attaches at the **editable-knob** level, not the matrix-row level, for the
shapes where one user knob fans into several rows. **This is the reason the engine is
hand-rolled** — Gurobi `feasRelax` is one-slack-per-row and cannot share.

**The elastic transform is a pure function**, `ElasticModel BuildElasticModel(const SolverModel &base)`
(`decide_diagnostic_engines.{hpp,cpp}`): it zeroes the user objective, drops the quadratic
objective, and adds slacks, returning the transformed model + a `vector<BlockSlackRef>`
(`{rows, pos_col, neg_col, sense}`) recording the slack→row wiring. A **size-1 block is
one slack per row.** `DiagnoseInfeasible` calls it, solves via the injected callback, and
reads each block back. Exposing it is also the structural test seam.

**Shape marker (`ConstraintProvenance::shape`, `decide.hpp::ElasticShape`).** `UNSET` is the
invalid default; `BuildElasticModel` asserts that every relaxable row with a user `clause_id`
has been stamped explicitly. `SHARED_SCALAR` = the complete canonical RHS is one editable
query-wide value shared across every row the clause emits; `PER_ROW_DATA` = genuine per-row or
per-group **data** RHS (`x <= col`). Set in `ilp_model_builder.cpp`: linear and quadratic
builder sites read `EvaluatedConstraint::rhs_is_shared_scalar`. Physical evaluation sets it
directly for constants and otherwise consumes the canonical RHS's `QUERY_WIDE_BOUND_TAG`;
foldable composite expressions receive that tag at canonicalization. Other bounds are stamped
`PER_ROW_DATA`; an folded bound's clause rows carry its literal, so `SHARED_SCALAR`.

**Grouping (mode-aware — see "Slack-scope policy" above).** `BuildElasticModel` groups
relaxable rows into blocks via `folds()` + `block_key()`. A `SHARED_SCALAR` block gets **one
shared slack** wired into all N rows (`=` blocks get one shared `s⁺` and one shared `s⁻`); with
`eᵢ − s ≤ K` on every row the single `s` is driven to the **max overshoot**, so the reported
edit is the max (not the per-row sum) and the "which clause to relax" race is not skewed ≈N×.
This covers **easy MIN/MAX** (`max(e) ≤ K` → per-member `eᵢ ≤ K`), **per-row literal**
constraints, and **multi-instance bounds** (`x ≤ K` over N instances, through its clause's
rows). For a clause over **`per` classes or `by` groups** the block key depends on the
mode: **query** keys by `clause_id` alone, so all classes of `per g: sum(x) by (g) ≥ K`
fold into one slack = the single literal edit; **expanded** keys by
`(clause_id, group_key)`, so each class is its own slack (the per-class profile).

**Every clause has its own grouping id.** `block_key()` groups by `repair_group_id`, so
two clauses sharing an id share a slack — and a slack spanning two unrelated clauses can be
spent on one of them and billed to the other: a repair that does not repair
(`SUM(y) >= 100` loosened to `>= 81.6` on a query where `y <= 2` caps `SUM(y)` at 8,
because the slack the engine spent was the bound's). Execution numbers every clause it
evaluates once: the prepared clauses first, then (under `DIAGNOSE`) the folded bounds'
clauses, then the composed MIN/MAX clauses, each taking its position in that order. No
count of the model's rows is the id space, since formulation drops rows (a natively
stated construct, a `<>` moved into indicator rows), so the ids are fixed before
formulation runs. Pinned by `TestNativeConstructDiagnosis` in
`test/decide/tests/test_query_diagnostics_relation.py`, which applies the reported edit and
re-runs the query, and compares the native and lowered arms against each other.

**A grouped MIN/MAX clause folds like any other.** `LinearizeMinMaxConstraints`
emits one bound row per group, all carrying the user's single literal, and stamps them
from `EvaluatedConstraint::rhs_is_shared_scalar` — the same flag the linear builder reads,
so the native and lowered arms classify one clause identically. Left `UNSET` those rows
would never fold, and an infeasible `per g: max(e) by (g) >= K` would report one edit per
group against the same clause text in the `query` scope; only the loosest of them would
repair anything. A genuinely per-group bound still stamps `PER_ROW_DATA` and reports a
virtual offset.

**A hard MIN/MAX clause is loosenable on both arms.** `MAX(e) >= K` lowers to N rows of
`e_i - M*y_i >= K - M` plus a rigid `SUM(y) >= 1`. The N rows are the clause: lowering `K`
moves all of them together, so they are stamped `USER_PARAMETER` and share one slack, and
the disjunction row stays rigid (slackening it would satisfy the clause with no row at
all, a repair with no SQL edit behind it). Before this they were rigid too, so the clause
had no loosenable row and the diagnosis fell back to whatever column bound sat nearby —
the same SQL repaired differently depending on which solver the host had, and only the
Gurobi answer named the line the user wrote.

**What the reader is quoted: `DisplayRhs`.** A row's own RHS is often not the number the
user typed. Three things shift it and they are resolved in one place
(`DisplayRhs(prov, rhs, sense)`): a strict `<` / `>` carries the binder's δ and re-quotes
`typed_k`; a lowering adds a mechanism term (the `± M` above); and a constant LHS term
folds into the RHS at every builder site, since `SUM(x + c) >= K` is emitted as
`SUM(x) >= K - Sum(c)`. The last was wrong for as long as it existed and independently of
MIN/MAX: the label renders the LHS the user wrote, constant included, so `SUM(x + c) >= 100`
was reported as `SUM(x + c) >= 90` — a clause appearing nowhere in the query, whose
suggested edit could not be pasted back over anything. Both shifts accumulate on
`ConstraintProvenance::rhs_mechanism_offset`, the lowering's part stamped by the
linearizer and the folded constant added by the builder site that folded it.

**Edit conversion (the D3 helper).** Reading the slack support is centralized in
`MakeLoosenEdit(provenance, lhs, rhs, sense, amount)` (a literal knob) and
`MakeVirtualOffsetEdit(provenance, lhs, rhs_text, sense, delta)` (a query-mode data offset,
`x <= col + delta`) in `decide_diagnostic_render.cpp` — the one place per-shape rendering
lives: they render the sense, re-quote a strict `<`/`>` against the typed literal, and format
the suggestion. The LHS is rendered shape-aware by `FormatLhs` — plain `FormatTerms`,
AVG-collapsing (`FormatAvgLhs`), or SUM-collapsing (`FormatSumLhs`) — or `FormatQuadraticLhs`
for a quadratic constraint. A `ClauseEdit` carries a `ClauseEditKind` (`LOOSEN` vs `DROP`,
`decide_diagnostic.hpp`), `edit_source`, and optional `group` / `row` identity.

### Data bounds and the editable-knob preference

A `PER_ROW_DATA` clause (`x <= col`) has a per-row data RHS with no single literal to
"loosen to K". Under the slack-scope policy it reports a **virtual offset** in query mode
(`x <= col + delta`) or a **per-row profile** in expanded mode (see "Slack-scope policy"), and
the data offset lives in a separate lexicographic tier, so an editable literal is always
preferred when it can restore feasibility. Two parts:

- **Reporting.** `ReadElasticEdits` reads a data-RHS block per the slack-scope mode: query mode
  emits **one `virtual_offset` LOOSEN** for the folded clause (`x <= col + delta`), expanded mode
  emits **one `expanded_row` LOOSEN per conflicting row**. Either way it names the clause, not
  one line per row, and the conflict is actionable.
- **Editable-knob preference.** In a degenerate infeasibility an editable knob and a data row can
  be equally-optimal fixes, and the solver would split arbitrarily (a messy, non-deterministic
  mix). To prefer an *actionable* edit, stage 1 solves data offsets before editable loosening:
  it first freezes the minimum removal count, then freezes the minimum data-offset total, then
  minimizes editable loosening. Data conflicts surface only when editable loosening cannot
  restore feasibility with data offset total `D = 0`.
- **Scale-normalized editable weights.** Within the editable tier the stage-1 objective sums
  slacks across constraints in *incomparable units* — e.g. `SUM(buy) >= 30` (count) vs
  `SUM(buy*l_extendedprice) <= 100` (dollars). With a uniform weight of 1 the raw sum makes the
  small-magnitude row look cheapest, so the solver would gut the count floor to `>= 0` (select
  nothing, `achievable_objective = 0`) instead of loosening the genuinely-tight budget — a
  degenerate "require nothing" edit. Each **editable** slack is weighted `ref / rms(Aᵢ)`
  (`BuildElasticModel`, `decide_diagnostic_engines.cpp`), where `rms(Aᵢ) = √(Σcⱼ²/nnz)` is the
  row's **root-mean-square coefficient** — its *typical* magnitude. Dividing by it makes the
  objective track how far the constraint boundary moves *per unit of decision*, so the
  large-coefficient budget (barely off in normalized terms) is the cheaper edit. RMS (not the plain
  L2 norm `‖Aᵢ‖₂`) is deliberate: L2 grows with the number of terms, so it would make a
  many-variable aggregate floor (`x + y >= 10`, √2) numerically cheaper to loosen than a
  single-variable cap (`x <= 0`, 1) purely for having more terms — reintroducing the degenerate
  "gut the floor" preference. RMS is invariant to term count (both those rows have RMS 1), so it
  removes that bias while still capturing the data-magnitude mismatch E needs. `ref` = the smallest
  editable RMS; being a common factor it never changes *which* row wins (only conditioning), keeps
  every editable weight in `(0, 1]`, and leaves the usual all-equal-coefficient model at weight 1.
  This is a **within-editable-tier** normalization only: data offsets and removals are separate
  lexicographic tiers, while editable slack refs carry the `ref / rms(Aᵢ)` coefficient used by the
  editable pass and the stage-2 `E*` budget. Why not RHS-norm (`1/|b|`): the budget needs a *large absolute*
  loosening (100 → thousands), so dividing by the small RHS leaves it expensive and does not fix the
  degeneracy; coefficient-magnitude does. Structural weight assertion in
  `test_decidb_diagnostic_engines.cpp`; end-to-end flip on real TPC-H data in
  `test_query_diagnostics_tpch.py::test_E_loosen_should_not_be_degenerate` (both backends).
- **Boolean bound-folding revert.** The weights are only *reachable* once the
  diagnosis model can actually exercise the loosened constraint. `DecidePropagateImpliedBounds`
  tightens a variable's column box by folding a user row — e.g. `SUM(buy * price) <= 100` implies
  `buyᵢ ≤ 100/priceᵢ`. For a BOOLEAN (lowered to INTEGER with a `[0,1]` box) a fractional upper
  (`≤ 0.1`) silently pins the variable to **0**, so the relaxable budget could never be
  exercised and the only "fix" is gutting the *other* clause — the same degenerate symptom, from a
  different cause. The diagnosis bound-reset loop (`physical_decide.cpp`) reverts folded
  tightenings for every column, booleans included — resetting a boolean's box to
  `[0,1]` (upper → 1 if `< 1`, lower → 0 if `> 0`) instead of skipping them — so the slackable row
  is the sole enforcer. Never opens past 1 (that would unbound the variable). With this, E loosens
  the budget and reaches a nonzero objective; without it, the normalized weights point at the right
  clause but the model still can't buy anything.
- **Data-weighted SUM renders symbolically (`SUM(buy * l_extendedprice)`).** A data-VARYING summed
  term has no single literal coefficient to quote, and the raw per-row fan-out
  (`24710*buy + 56688*buy + …`) is unreadable. The coefficient's source-column name is carried
  from the binder (`Term::coefficient->GetName()` at evaluation in `physical_decide.cpp`) through
  `EvaluatedConstraint::coefficient_labels` → `ConstraintProvenance::weight_labels`
  (`ilp_model_builder.cpp`, stamped at the aggregate emission sites) → `FormatSumLhs`, which renders
  `SUM(var * label)` when a fanned variable is data-varying and a label is present (else it still
  falls back to the raw form). Uniform-coefficient sums are unaffected (they still fold to
  `SUM(c*var)`).

### AVG, strict and quadratic clauses in the user's units

- **AVG.** The AVG→SUM rewrite pre-scales row coefficients by `1/N_g`
  (`ScaleAvgRowCoefficients`), so the slack is **already in AVG units** — reported raw, no
  `/N_g`. A pure-AVG row is tagged `provenance.avg_scaled` (set in `physical_decide.cpp` when
  every linear term is AVG-scaled, propagated at the aggregate provenance sites in
  `ilp_model_builder.cpp`); `FormatLhs` then collapses the `1/N`-scaled terms back to `AVG(x)`
  (inner coeff = stored coeff × the variable's term count) instead of rendering `0.5*x + 0.5*x`.
  A **data-varying** AVG coefficient (`AVG(buy * p_retailprice)`, price differs per row) has no
  single literal to re-quote, so `FormatAvgLhs` bails and the row renders through `FormatSumLhs` —
  but `FormatLhs` passes `agg_name = "AVG"` for any `avg_scaled` row, so the wrapper stays `AVG(...)`
  rather than mislabeling the user's AVG constraint (and its suggested edit, quoted in AVG units) as
  `SUM`.
- **SUM.** An aggregate `SUM` over rows emits one solver column per row for the same decide
  variable, so a plain reconstruction reads `x + x + x`. `FormatSumLhs` collapses the per-row
  fan-out back to `SUM(c*var)` for a uniform coefficient, and to `SUM(var * col)` for a
  **data-varying** coefficient (`SUM(buy * l_extendedprice)`) using the symbolic column name carried
  in `provenance.weight_labels` (see the "data-weighted SUM renders symbolically" note above); it
  only falls back to the raw numeric reconstruction when a data-varying term has *no* label.
  **Grouped aggregates fold too**: every group of `per grp: sum(x) by (grp) >= 5` renders
  as that one clause, kept distinguishable by the `group` column (see "Group identity"
  below). A **single-row aggregate group** (one a `when` narrows to a single row) has no
  fan-out for `FormatSumLhs` to fold, so `FormatLhs` wraps the reconstruction in `SUM(...)`
  explicitly when `provenance.is_aggregate` and there is no multi-column fan (`HasVarFan`)
  — `when g = 'a': sum(x) >= 99` never reads as a bare `x >= 99`.
- **Keyed SUM (the fold).** A **keyed** decision (`DECIDE per S: keepS(BOOL)`) maps every
  joined row of a class onto *one* solver column, so the builder **accumulates** instead of
  fanning out: `sum(keepS)` over two alert rows of one sensor reaches the matrix as a single
  index with coefficient `2`. The per-row fan-out every rule above keys on is then
  structurally absent, and reconstructing from the matrix quotes arithmetic the user never
  wrote — `SUM(2*keepS)` when the group holds one class, or `SUM(keepS * 1)` when the group
  mixes join multiplicities (the differing per-class totals read as a data-varying weight,
  so the coefficient's *source text* gets rendered as if it were a column). Both fail the
  pasteable-edit test and leak the keyed/per-row distinction. The accumulating build sites
  therefore stamp `provenance.folded_terms` — the coefficient the user actually wrote per
  summed decide variable, taken from `CoefficientColumn::IsUniform()` or, when the constant
  materializes dense, from a numeric `coefficient_labels` entry (that label is the
  coefficient expression's source text, so a wholly-numeric label *is* the written
  coefficient). `FormatLhs` consults `FormatFoldedSumLhs` **before** the matrix-inference
  paths, since those misread accumulated rows; it declines (returning false) for any row it
  cannot reconstruct whole — an auxiliary column, an unstamped variable, or a data-varying
  term with no symbolic name — leaving the paths below unchanged. `folded_terms` is
  populated only on the accumulating path, so a per-row fan-out never diverts there. AVG
  rows are excluded: their coefficients are additionally 1/N pre-scaled, so the recorded
  unit is not quotable in AVG units. Genuinely data-weighted keyed clauses still render
  symbolically (`SUM(keepS * cost)`).
- **Strict `<` / `>`.** The δ offset (`< K → <= ceil(K)-1`, `> K → >= floor(K)+1`) is baked into
  `rhs` at the δ site (`ApplyComparisonSense`, `ilp_model_builder.cpp`), which also sets
  `provenance.strict` + `provenance.typed_k` (the user's literal: a constant moved off the body,
  as in `SUM(x - 5) < 3`, is added back). `MakeLoosenEdit` re-quotes the
  suggestion against `typed_k` and renders `<` / `>`, e.g. `x < 10` → `x < 16`.
- **Quadratic.** `BuildElasticModel` loops `model.quadratic_constraints` and slacks the **linear
  RHS only** (never the Q matrix): `e(x) + xᵀQx ≤ K` loosens to `… ≤ K + s`. The `BlockSlackRef`
  carries a `quadratic` flag (its `rows` then index `quadratic_constraints`); `FormatQuadraticLhs`
  renders `POWER(x, 2)` / `x*y` terms. Quadratic slacks are always editable-weighted (a quadratic
  is always a LOOSEN edit, never a data conflict). QCQP is Gurobi-only. A constant in the
  quadratic body (`SUM(POWER(x - 5, 2)) <= 3`) is folded into the bound as on the linear path,
  and stamped as `lhs_offset` the same way, so the echo and the suggestion quote the written
  `3`, not the folded `-72`; a strict `<` / `>` keeps the typed literal as `typed_k`.

### Group identity (group label + scope)

A clause over `per` classes or `by` groups (`per grp: sum(x) by (grp) >= 5`) emits one row
per group (how a clause becomes model rows: `../../language/guide.md`
§6.4), so each group can get its own edit. Each edit is **self-identifying**, which is
what lets the SUM fold above apply without two groups colliding in the relation. Three
channels, read off `ConstraintProvenance` and the source registry (no change to the solve):

- **`group_label`** — the group's printable key value (`a`, or `EU, 2024` for the composite
  key of `per r, y:`). `LookupOrBuildPerGroupIds` (`physical_decide.cpp`) requests
  `BuildGroupIds`'s `rep_keys` out-param and stores each group's representative values on
  the cache entry; the per-call renumbering to consecutive `0..K'` (the groups the call's
  filter keeps, in the order their rows first appear) reindexes the labels in lockstep (a
  new `mapped` id is pushed at exactly `out_group_labels.size()`, so labels stay aligned
  with the dense ordinals). `FormatPerGroupKey` joins composite columns with `, `; a NULL
  key value — a class of its own (D5) — renders `NULL`, and an empty-string key renders
  `''` so it is not confused with "ungrouped" (a string `'NULL'` reads like a NULL, see
  `todo.md` §2). Threaded through `EvaluatedConstraint::group_labels` → stamped on
  `provenance.group_label` at **every** emission site in `ilp_model_builder.cpp`: the
  grouped aggregate sites, and the per-row site. The per-row site is the one
  that sees a hard MIN/MAX whose bound no row in a group can reach: the rewrite drops the
  MIN/MAX marking and re-emits that group as ordinary per-row rows
  (`linearization_minmax.cpp`, the `NO_SOLUTION` branch). So a grouped failure names the
  group it sits in (`per g: min(x) by (g) <= -inf`, group `0`) rather than sending the user
  through every group. In the expanded scope the classes a row stands for (`class_set`,
  read by `FanOutByClass`) supply the `group` values; `group_label` names the group of a
  row that carries none. The `clause` stays in the user's spelling, so a finding never
  relies on relation row order to tie `suggested_change`/`amount` to a group and never
  appends implementation-only `[group: ...]` text to the clause.
- **`is_aggregate`** — set from `eval_const.lhs_is_aggregate` at the aggregate emission sites;
  drives the single-row `SUM(...)` wrapper in `FormatLhs` (see the AVG section above).
- **The scope (`qualifier`)** — the `when c per K:` text in front of the clause, built once
  per clause on the bound tree by `InitializeConstraintSourceInfo`
  (`decide_source_provenance.cpp`, `VisitSourceComparisons`) and kept in the source
  registry (`ConstraintSourceInfo::qualifier`). `SourceAwareProvenance` copies it onto the
  provenance the renderer reads, and `AttachDecideQualifier` writes it in front of both
  the clause and the suggestion. A `when` and a `per` key print in that order
  (`when active per grp: sum(x) by (grp) >= 5`); a key prints as written, comma-separated
  (`per r, y:`), `per ()` as `per ():`, and the default `per row:` not at all (D32). The
  reducer prints with its `by (G)` after it. The `when` predicate renders through
  `RenderDecideSource`, the renderer EXPLAIN uses, so a clause reads identically wherever
  it is quoted back: the binder's implicit CASTs are dropped (`grp = 'a'`, not
  `(grp = CAST('a' AS VARCHAR))`), a cast the user typed is replayed from its source
  fragment, and a column that needs quoting keeps its quotes (`when "ok:": ...`).

The two-level objective's `per` key passes a throwaway labels vector (objective groups
are not diagnosed by clause key). A composite key is written as a comma list
(`per region, year:`); parentheses are only for `per ()`, and `per (region, year):` is a
parse error that names the comma spelling. An easy `max(x) <= K` is a column bound, not a
grouped clause — see "An easy MIN/MAX over one decision is a column bound" above.

### Rigid shapes

`<>` indicator rows (`USER_MECHANISM`), McCormick bilinear links (`STRUCTURAL`) and every
MIN/MAX **closing** row (`USER_MECHANISM`, both the hard and the composed path) are not
relaxable (`IsRelaxableForElastic`), so the elastic transform attaches no slack to them: a `<>`
or bilinear conflict is resolved by loosening an editable bound, and a conflict reachable only
through those rigid rows produces an `undiagnosed` finding (the empty-block guard). `<>` is
remove-only; removal is the exact atomic DROP below.

**A Big-M row is never an editable knob.** A row carrying a Big-M states some construct's
*mechanism*, and slack on it is measured in that mechanism's units, not the user's. A composed
MIN/MAX lowers to an envelope, closing rows and an outer row; only the outer row is the user's
clause. Making the closing rows relaxable broke both halves of a diagnosis at once:

- *The name.* `ExtremumLinkSpec` carries no `source_clause_id`, so a blamed closing row had no
  clause to name. `SourceAwareLhs` fell back to `FormatLhs` and rebuilt the text from the row's
  own coefficients, reporting `MIN((x + c)) - x - 110*MIN((x + c)) >= -110` — a string appearing
  nowhere in the query, stamped `edit_source=source_literal`.
- *The number.* Slack on a closing row lets the auxiliary `z` leave its extremum; the user's
  bound moves by that slack times the coefficient `z` carries in the outer row. For
  `3*MIN(x + c) + SUM(x) <= 22` the engine reported 8 where the real repair is 24, and the
  suggested `<= 30` re-solved to infeasible.

Relaxing the outer row is mathematically equivalent — lowering `z` by `s/a` and raising the
bound by `s` admit the same solutions — and is already in the user's own units, so repair goes
there. `EmitComposedMinMaxOuter` stamps it `USER_PARAMETER` + `SHARED_SCALAR` with a
`repair_group_id` and a `source_clause_id`.

The guard that makes this structural is `assert_blamable_row` in the elastic transform: a
relaxable row must carry an explicit `ElasticShape` **and** a `source_clause_id`. Without the
latter the renderer can only fabricate, so a row that cannot name a clause is not one the user
can be told to edit. `D_ASSERT` is debug-only, so the release-build backstop is the Python
invariant helper described in `../foundations/done.md`.

**What the tags are called.** The pipeline aliases that carry this linkage are
`MINMAX_CLAUSE_TAG_PREFIX` (`__minmax_clause_<clause_idx>_<min|max>__`) and
`NE_CLAUSE_TAG_PREFIX` (`__ne_clause_tag_<clause_idx>__`), both in
`common/enums/decide.hpp`. The payload is a **clause** index — into
`LogicalDecide::minmax_clause_labels` / `ne_clause_labels`, which hold the text a diagnosis
renders — and not an indicator index. The distinction matters because an indicator is allocated by the formulation and a
clause is not: the clause is registered where it is *tagged*, before either arm is chosen, so
both parts of the payload are always present. There is one spelling per tag, and the reader
asserts it.

**Tests.** C++ structural (`test_decidb_diagnostic_engines.cpp`): one shared slack spans all N
rows with the correct sign; a grouped `SHARED_SCALAR` clause folds to one block in query and
one per group in expanded; a data RHS folds to one block in query and stays independent per-row
in expanded; `USER_MECHANISM`/`STRUCTURAL` rows are never slackened; a data-offset tier
defers to an editable knob; a query-mode data RHS folds to one virtual offset; strict re-quotes
against `typed_k`; an AVG row renders `AVG(x)` with the raw slack; a quadratic constraint slacks
its linear part only with Q untouched. C++ behavioral: a shared block with rigid floors reports
the max overshoot (7), one edit, not the sum (10). Python differential
(`test_query_diagnostics_relation.py`): an easy `max(x) <= 5` and a multi-instance bound
report total = max overshoot; `per grp: sum(x) by (grp) >= 5` folds to one edit in query mode
and breaks out per class (`group` = `a` / `b`) in expanded mode, an empty-string key keeping
its `''` label; `when grp='a': sum(x) >= 99` keeps its `sum(...)` wrapper and its `when` in
front (`when grp = 'a': sum(x) >= 99`), and `when active per grp:` keeps that order; a
BOOLEAN model loosens its SUM target, never its 0/1 domain (regression guard); a data-RHS clause
reports a named virtual offset (query) / per-row profile (expanded); a data-offset row defers
to an editable edit; reversed bounds are
reported as their canonical folded form; AVG/strict re-quote (single-row SUM keeps its wrapper:
`sum(x) > 10`, including strict quadratic); quadratic and McCormick gated to Gurobi; `<>` and
McCormick rows stay rigid.

## Elastic engine: exact atomic DROP

Exact atomic DROP handles clauses that cannot be meaningfully loosened: `<>`, `IN (...)`, and every
constraint-side comparison containing an L0 `norm`. The optimizer assigns one
written-order `removal_group_id` to the source comparison before any rewrite. Objective-side
L0 is deliberately excluded. Every descendant row or native construct copies that identity,
including IN cardinality/link rows, an folded bound's clause rows, ABS/McCormick descendants,
composed MIN/MAX extraction, and backend lowering.

`source_clause_id`, `repair_group_id`, and `removal_group_id` are separate namespaces:
display identity does not decide elastic sharing, and neither decides atomic omission.
Removable descendants remain rigid and receive no slack. A candidate DROP physically filters
all matching linear, quadratic, indicator, and native-general constraints from a copied
`SolverModel`; column declarations and the ordinary solve model are unchanged.

**Label channel for global-block columns (aggregate `<>`, composed MIN/MAX).** A per-row `<>`
indicator is a decide-var column, so `BuildColumnProvenance` already names it from `var_labels`.
An aggregate `<>` binary lives in the global block, which `BuildColumnProvenance` otherwise
leaves unnamed — so a dropped aggregate `<>` would have an empty subject.
`SolverInput::global_variable_labels` (parallel to `global_variable_types`) carries the user
source text of a labeled global: the clause text `SUM(x) <> K` for an aggregate-`<>` binary
(looked up from `aux_var_expressions` at that site), and the aggregate text `MAX(x)` for a
composed-MIN/MAX z (both allocation sites build it from the term's `agg_name` +
`inner_expr->ToString()`) — so the composed outer pin renders `SUM(x) + MAX(x) <= -1`, never
an internal `colN`. Other global aux (single-term MIN/MAX objective z, McCormick) stay
unlabeled, which is an empty entry rather than a missing one.

The channel is positional — entry `i` names global column `i` — and it is kept aligned at
the point of allocation, not at the point of labelling. `AddGlobalContinuousAux` and
`AddGlobalBinaryAux` (`ilp_linearization.cpp`) are the only two places a global column is
created, and each takes the label as an optional argument and writes the entry itself. So the
channel is dense by construction and a new allocation site cannot land its label on the wrong
column by forgetting to pad first. The `resize(num_global_vars)` still standing before
`SolveModel` is therefore a no-op kept as a belt-and-braces assertion of that invariant.
`BuildColumnProvenance` takes the vector as an optional argument and writes the labels
onto the global-block columns; the infeasible engine forwards it through
`InfeasibleDiagnosisInput::global_variable_labels`. DROP labels come from the source
registry keyed by `source_clause_id`, the clause's `when c per K:` qualifier in front.

Because that registry is the single source of clause text, it also owns the operator
spelling. `DecideComparisonOperator` (`decide_source_provenance.cpp`) renders
`COMPARE_NOTEQUAL` as `<>` rather than DuckDB's default `!=`, so every DECIDE clause
renderer — written and canonical text, DROP and LOOSEN subjects, EXPLAIN — quotes the
operator the syntax reference documents and the user typed. Without it a `<>` clause came
back as `x != 1`, matching neither the query nor the `SUM(x) <> 0` spelling
`ne_clause_labels` had always used.

**Exact search and ranking.** Groups are enumerated lazily by increasing cardinality. Every
candidate at the first feasible cardinality is solved; a timeout, `INF_OR_UNBD`, suboptimal
result, or backend error aborts the layer rather than returning a partial minimum. Candidates
then compare data-offset cost, editable-loosen cost, objective outcome (a proven unbounded
objective outranks a finite one), and source order. Display-rounded values are used for the
user-visible cost/objective comparisons. The original objective is solved only after repair
budgets are frozen.

**Reading + reporting.** Each selected source group produces exactly one
`ClauseEdit{kind=DROP, edit_source='remove_only'}` with `suggested_change='remove this
clause'`. DROP has no amount and never exposes an internal cardinality, indicator, ABS,
McCormick, or extremum row. There is no diagnostic removal Big-M and no removal setting.

**Stage-2 composition.** For each fixed omitted group set, stage 2 freezes `D ≤ D*`
and `E ≤ E*`, restores the user's objective, and computes the achievable result. DROP-set
selection therefore compares objective outcomes only after cardinality and repair costs.

**Stage-2b rank tie-break (best-effort preference, all repair kinds).** When the repair
choice is *indifferent* between two equally-minimal repairs — two DROP sets, or an LP that can
put the same loosening on either of two clauses (or split it fractionally across both where
one edit suffices) — the solver may pick arbitrarily. `BuildTieBreakModel` makes common ties
stable with one extra pass over every repair knob: when a stage-2a objective exists, freeze it
at its optimum with a one-sided row (`obj ≥
Z*` for MAXIMIZE / `≤ Z*` for MINIMIZE — the rhs is **exact**, no tolerance cushion: any eps of
objective room is monetized by the rank objective, which would shave eps of repair onto a
lower-ranked slack and split the edit; the stage-2a point attains `Z*` to machine precision,
and a rounding pathology just makes the pass non-optimal, keeping the stage-2a repair). Then
minimize the rank-weighted slack sum: each slack gets its tier coefficient (editable slacks:
the scale weight) times its 1-based rank, with ranks ascending in emission order. The tier
budget rows carried into the model pin each tier's total, so the pass only
reselects *which* clauses carry the repair and prefers lower-ranked clauses. On the
**no-objective path** (no
user objective, or a data-only repair that skips stage 2) the same pass runs directly on the
budget-frozen stage-1 model with no freeze row; its solution is re-read with `snap=false` like
the stage-1 read. The stage-2a achievable objective `Z*` is what gets reported (the tie-break's
own objective is just the ranking sum). The pass is skipped — keeping the prior result — when
no tier owns `≥ 2` knobs (budgets pin cross-tier trades, so no naming tie is possible) or, on
the freeze path, when the objective is quadratic (freezing it would need a quadratic row; an
exact tie there is rarer still).

**Repair-tie policy.** `DIAGNOSE` promises one repair that is optimal
under the lexicographic tier budgets above and, when stage 2 succeeds, the best achievable user
objective under those budgets. It does **not** promise a canonical repair-set identity when
multiple sets have the same tier budgets and achievable objective. Equal-rank multi-edit sets,
quadratic-objective ties, and ties involving singleton-row bounds may therefore name different but
equally good repairs across solver backends. This is accepted behavior: source order has no
semantic priority, and a total order would add metadata, serialization risk, and diagnostic
re-solves without improving repair quality. Stage 2b remains a useful preference for common
single-edit ties, not a cross-backend API guarantee. Tests for unresolved exact ties should
assert feasibility after applying the repair, tier-budget optimality, and achievable-objective
equality rather than requiring the same clause identity from every backend.

**Tests.** C++ structural (`test_decidb_diagnostic_engines.cpp`) proves removable rows receive
no elastic slack, physical omission reaches linear/quadratic/indicator/native-general
representations, and stale build-proven infeasibility is recomputed. A pinned-`<>` must-drop
reports `edit_kind='drop'`; a loosenable
conflict prefers the LOOSEN and never drops; an **aggregate `<>`** whose disjunction binary is
a global-block column is named via the `global_variable_labels` channel (not `var_labels`) and
reported as a drop. Python differential (`test_query_diagnostics_relation.py`, both backends):
`x <> 0 AND x <> 1` on a BOOLEAN drops exactly one `<>` (min cardinality) and chooses the
objective-best drop (`x <> 0` for `MINIMIZE SUM(x)`, `x <> 1` for `MAXIMIZE SUM(x)`), with the
reported objective differential-checked against an independent re-solve of the fixed query;
`SUM(x) <> 0 AND SUM(x) <> 1` (aggregate) does the same while keeping the dropped aggregate
`<>` named; **two independent BOOLEANs** each with `<> 0 AND <> 1` drop the objective-best `<>`
per variable (a minimum-cardinality-2 set, differential-checked); an **objective-indifferent
tie** (`x <> 0 AND x <> 1 AND y ≤ 5 MAXIMIZE SUM(y)`, where the objective only touches the free
`y`) drops the earliest-declared `x <> 0` on **both** backends — the solver-agnostic tie-break
guarantee; `x <> 5 AND 5 ≤ x ≤ 5` prefers loosening. IN (singleton, multi-value, negative,
and under a `when`) and L0 (explicit/auto, directional/equality, composed MIN/MAX, and prepared
bilinear replay) each report one source-level DROP on both backends; objective L0 is excluded,
and the removed setting is rejected.

## Elastic engine: stage-2 achievable objective (freeze-budget)

Stage 2 reports the **achievable objective** the user gets after the minimal fix — and the
specific edit that achieves it. For each fixed DROP candidate, stage 1 (above) finds the
lexicographically least repair budgets `(D*, E*)`, but when that minimum is non-unique it returns an **arbitrary**
minimizer whose edit need not be the one best for the user's objective. Stage 2 is a final
lexicographic tier: among all fixes within those repair budgets, pick the one that maximizes
the user's original objective, and report **that** edit so the edit and the objective agree.

**Method (freeze the budget, not the amounts).** `BuildStage2Model`
(`decide_diagnostic_engines.cpp`) starts from the stage-1 elastic model (slacks already
wired) and:
- adds rigid **budget rows** for every active stage-1 tier: `D ≤ D*` over data-offset slack
  columns and `E ≤ E*` over editable slack columns using the
  same `ref / rms(Aᵢ)` coefficients as the editable pass. Equality blocks count both
  directional slacks (`s⁺ + s⁻`). The caps are exact stage-1 tier optima; the backend
  feasibility tolerance supplies the only cushion that keeps the re-solve feasible.
- **restores the user's original objective** (linear `obj_coeffs`, the quadratic objective
  `q_*` / `has_quadratic_obj` / `nonconvex_quadratic`, and the `maximize` sense), with the
  slack columns set to zero objective weight so the reported objective is exactly `cᵀx` over
  the user's variables. Stage 1 had zeroed and dropped the objective; stage 2 needs it back.

`DiagnoseInfeasible` runs stage 2 for every feasible candidate carrying a real objective
(`HasObjective`). A constant objective has nothing to report, so the candidate keeps its
stage-1 edit list unchanged. A data-RHS clause is an actionable `LOOSEN` (a query-mode virtual
offset or an expanded per-row edit), so it also exercises stage 2 and reports an achievable
objective.

**Reading the result.** On `OPTIMAL`, the stage-2 slacks are re-read into the edit list
(`ReadElasticEdits`, the shared stage-1/stage-2 reader) and the objective value is reported.
Because the budget is enforced only to the feasibility tolerance, a maximizing re-solve can
ride that tolerance (a clean `10` arriving as `10.000001`), so stage-2 amounts and the
objective are snapped by `SnapDiagnosticValue` — snap to the nearest integer when within that
noise, an absolute `1e-6` plus a relative `1e-9` (recovering integer bounds at **any** magnitude, where significant-figure rounding would turn
`1234567890` into `1234570001`), else trim to a fixed
absolute precision. The relative part is that small on purpose: a relative `1e-6` at `1e6`
would erase a real `0.34` of a repair, and the pasted bound would not repair the query. Separately, both stage reads use one-directional, per-edit precision cleanup for
noise introduced by the widened formulation; only stage 2 receives the additional budget-row
snap described here.

**Objective value plumbing.** `SolverResult` carries `objective_value`, populated on
`OPTIMAL` by both backends in the model's own sense (`gurobi_solver.cpp` via
`GRB_DBL_ATTR_OBJVAL` through the loader's `getdblattr` scalar getter;
`deterministic_naive.cpp` via HiGHS `getInfo().objective_function_value`). The engine still
reads the stage-2 achievable objective directly from this field; stage-1 tier budgets are
computed from the solved repair variables so the same coefficient logic feeds readback and
the `R*`/`D*`/`E*` freezes.

**Reporting.** `BuildInfeasibleDiagnostic` has two optional arguments
(`achievable_objective`, `unbounded_after_fix`). An achievable finite value becomes one
model-level finding with `edit_source='achievable_objective'` and the value in typed
`amount`; an unbounded repaired objective becomes an `unbounded_after_fix` finding. The
repair budgets themselves are **not** surfaced .

**Stage-2 unbounded.** If the relaxed region is unbounded in the objective
direction (`UNBOUNDED` / `INF_OR_UNBD`), there is no finite optimum: the engine keeps the
stage-1 edit (still valid) and emits an `edit_source='unbounded_after_fix'` finding. It does
**not** hand off to the unbounded engine — the edit is the actionable part, and composing two
diagnoses would muddy the message.

This is the **final contract**, not a placeholder. The alternative — hunting for a
different repair that leaves the objective finite — would break the promise that stage 1
returns the *smallest* edit, and no such repair need exist. So the answer is two edits,
and the finding says so plainly:

> this fix satisfies your constraints, but the objective can then grow without limit —
> bound the terms in it too

The remedy names the **terms in** the objective rather than "the objective", because an
objective is an expression, not a knob the user can bound directly; what they can bound
are the decisions inside it. Solver vocabulary ("no finite optimum") is deliberately out.
The finding still names no clause — the ray cannot finger a guilty one, which is the same
limit the unbounded engine records.

**Tests.** C++ (`test_decidb_diagnostic_engines.cpp`): a non-unique-minimizer 2-var case
(caps `x ≤ 0`, `y ≤ 0` feeding rigid `x + y ≥ 10`, `MAXIMIZE x`) reports the x-cap edit and
objective `10` — loosening `y` would give `0`; a stage-2-unbounded case (`MAXIMIZE y` with
`y` free) reports the edit plus the `unbounded_after_fix` finding. Python differential
(`test_query_diagnostics_relation.py`): the same shape end-to-end on both backends, with the
reported objective checked against an independent re-solve of the fixed query (the edit
applied), never a hand-computed value. Every infeasible test that carries an objective exercises stage 2. The **no-objective
loosen tie** (`x <= 0 AND y <= 0 AND x + y >= 10` with no MAXIMIZE, where the two backends
would otherwise name different clauses) reports the same single floor edit on both backends
(`test_infeasible_no_objective_loosen_tie_is_solver_agnostic`), the stage-2b guarantee for
loosen repairs.

## Elastic engine: repairs the model can reach

Stage 2 above picks the best repair **among those the elastic model can express**, so the
model must be able to express every repair the user could make. Every Big-M and every derived
column ceiling in the `linearization_*.cpp` files is sized from the decision box as the query
states it. That is correct for the solve and wrong for the diagnosis, because the repair the
engine looks for is precisely a **widening** of that box. A ceiling sized at the query's width
makes the widened repair unrepresentable, and stage 2 would report whatever else it can see.

**What goes wrong without the rule.**

```
x(REAL): x >= -1 AND x <= 1 AND ABS(x) >= 5          MAXIMIZE 6*x
  gurobi (native ABS)  → loosen `x <= 1`      to `x <= 5`      amount 4, objective 30
  highs  (lowered ABS) → loosen `ABS(x) >= 5` to `ABS(x) >= 1` amount 4, objective 6
```

Both repairs are valid and both re-solve, but one is worth five times the other. Gurobi
states ABS natively and bakes in no constant, so it sees both; a lowered Big-M envelope sized at
1 makes the box repair infeasible inside the elastic model. It is a formulation split, not a
solver one: `DECIDB_NATIVE_CONSTRUCTS=off` gives the lowered answer on Gurobi too.

MIN/MAX has the same blind spot without the disagreement. `per g: max(x + c) by (g) >= 25`
over `x` in `[0, 9]` boxes the extremum column at the family's own reach, so **every** arm
would advise loosening the MAX, worth 36, when `x <= 23` is the same size of edit and worth 92.
A cross-backend check cannot catch a blind spot both arms share.

**The rule.** A clause that demands more of an auxiliary than the box can supply sizes that
auxiliary itself. Note when it fires: only when the demand *exceeds* the box, which is only
when the clause cannot be met as written. A query that solves reaches its own bound by
definition, so its ceilings and Big-Ms are untouched. A looser M is valid in any case; it only slackens the
deactivated arm of a disjunction, whereas a too-small one cuts off legal answers.

**Where it lands.**
- **ABS** — `DemandedAuxReach` (`linearization_bilinear_abs.cpp`) returns the largest value any clause
  requires the auxiliary to hold, and `DeriveAbsAuxiliaryBounds` takes `max(box reach, demand)`
  before setting `link.abs_range` and the auxiliary's ceiling. Both consumers follow: the
  ceiling, and the `2M` envelope `LinearizeAbsMaximize` emits off `abs_range`. The demand is
  usually **not** a row — `ABS(x) >= 5` is a simple `var OP const` comparison, so
  `FoldSingletonRows` folds it into the auxiliary's own box — which is why the helper reads
  `lower_bounds[aux]` first and only then walks the rows. Otherwise the ceiling would sit *under* the floor: an empty column no widening of `x` could repair.
- **MIN/MAX** — `LinearizeMinMaxConstraints` widens `group.family` to cover the clause's bound
  before the extremum column is boxed, so both the column and the closing Big-M (`family.Span()`)
  follow.
- **MIN/MAX, native arm** — one level further down, and it needs the opposite treatment. The
  lowering emits the closing side alone (`z <= max`), so `K` is a fine ceiling for `z`. A
  general constraint states `z = MAX(t..)` as an **equality**, so `z` must hold whatever the
  members actually reach — and a member may have to go past `K` to get the extremum there
  (`MAX(x + c) >= 25` is met at `x = 23` with `c = 4`, an expression of 27). Boxing `z` and the member columns at 25 would cap `x` at 21 and cost three units of
objective (89 instead of 92). There is no derivable number — how far the
  repair travels is not known until it is solved — so on the native arm the demanded end is
  **opened** and the pin row tying each member column to its expression does the constraining.
  The native arm derives no Big-M from the family, so an open end costs it nothing but a looser
  root relaxation on a query that cannot solve anyway.

**The elastic model is a formulation, not a matrix patch.** Patching the *solved*
`SolverModel` (re-opening its column box and appending slack columns) would keep its Big-M and
McCormick constants as derived against the unopened box, so they would cap the repair at the
bound it is supposed to widen: `x <= 5` would repair to `x <= 6` on the Big-M paths and never
under McCormick, and the reported repair would depend on which solver is installed. `<>`,
bilinear and `norm` all have encodings of that shape.

So the elastic model is **formulated**: `PhysicalDecide::FormulateElasticModel`
takes the model input and column space as PHASE 3a left them — retained under `DIAGNOSE`
only, before a single constant was derived — widens the box, and runs the same PHASE 3b pass
(`FormulateModel`) the solved model went through. Every constant is re-derived against the
box the repair actually searches. See
[`../../pipeline/08_execution/done.md`](../../pipeline/08_execution/done.md) §6 for why
that pass can run twice at all.

**The widening is bounded, and bounded per column.** Opening a direction all the way is not
an option: an unbounded column has no finite Big-M, so a formulation against a fully open box
does not lose precision — it *refuses the query* (`Bilinear term requires a finite upper bound
on variable 'x'`). So each loosenable direction is widened by a finite amount, and that amount
comes from the column itself:

- **ten times the column's own magnitude**, as a floor. This covers a column whose repair
  target appears in no row — the bound behind an `ABS(x) >= 5`, where the rows are the
  construct's own links and carry no target of their own.
- **whatever a row demands of it**: `|rhs| / |coefficient|`, the value the column would have
  to reach to satisfy that row single-handed. `SUM(y) >= 100` says a repair may need `y` out
  at 100 however small `y <= 2` looks beside it, and that is the difference between one edit
  the user can apply and two because the first could not reach far enough. Dividing by the
  coefficient is what makes a price-weighted row state its demand in the column's units
  rather than its own.

Per column is the whole point, and it was learned the hard way. An earlier version derived one
widening for the model and applied it everywhere, with a doubling loop to grow it when it
bound the search. Every number in the elastic model was then coupled to the largest number
anywhere in the query: a price-weighted row dragged an unrelated `x <= 5` out to 1e15, the
Big-M followed, and HiGHS stopped being able to load the model for reasons that had nothing to
do with `x`. Three separate defects came out of that one coupling before it was replaced.
Scaling each column from its own bounds and row demands prevents unrelated columns from
inflating its constants. A row can still demand a value far beyond the current box — that
larger widening is semantic and local to the column. The calculation needs no search,
doubling, or model-wide radius.

The repair slacks themselves are uncapped. They need no ceiling: what a repair can actually
reach is bounded by the widened column box, which is a hard constraint the solver cannot
exceed, so a slack has nothing to gain by running past it.

**What the widening costs.** A wider box means looser Big-M constants and a worse-conditioned
elastic model, so the slack a backend returns carries rounding noise: Gurobi's own feasibility
tolerance shows up as a spurious `x <= 1` → `x <= 1.000001` edit, and an exact `4.5` comes back
as `4.499999`, which reported verbatim is a repair one part in a million *short* of working.
Both are cleaned up in the readback.

The floor that separates a real edit from rounding is **absolute** — a few multiples of the
backend tolerance — and deliberately not relative to anything. Two relative floors were tried
and both are wrong, in opposite directions:

- taken from the *model's* scale, it erases every small edit in a model that contains one
  large-coefficient row, and answers "no loosening restores feasibility" about a query one
  `x <= 7` away from solving;
- taken from the *largest edit in the same repair*, it erases the `x <= 0.001` half of a
  repair whose other half is `y <= 20000` — and the surviving half alone does not restore
  feasibility, so the user is handed a repair that does not work.

The asymmetry decides it: an extra hairline edit is cosmetic, a dropped one is a broken
repair, so the floor errs toward reporting. (Both were symptoms of the model-wide widening
that has since been replaced; the absolute floor is what is correct independently of it.)

Rounding a reported amount is separate. It *is* relative — to that amount's own magnitude,
so an edit of 2 and an edit of 30,000 in one model are each trusted to about six figures of
themselves — and it is **one-directional**: it may tidy `4.499999` up to `4.5`, never
`4.333333…` down to `4.33333`. A loosening reported one tick short is not a repair; the user
pastes `x <= 3.33333`, the sum lands on 9.99999 against a `>= 10` floor, and the query is
still infeasible. Over-repairing by one part in a million is invisible.

**A backend that cannot analyse the repair model says so.** The elastic program is
deliberately harder than the query's own — a widened box means looser Big-M constants — and
HiGHS in particular gives up on some MIPs it would otherwise take, or refuses to load a model
whose tier weights produced a sub-tolerance coefficient.
That is a limit on what can be *explained*, not a failure of the query, so the solver callback
catches DuckDB exceptions from the backend call and the terminal reports an `undiagnosed`
finding. The guard is at that callback boundary: exceptions from the diagnosis logic still
surface as bugs. The formulation pass above likewise keeps its narrow catch.

The solved model is untouched by all of this — the golden dumps are byte-identical — because
the second formulation runs on a copy.

**The sweep** (`test_diagnosis_repair_sweep.py`). One invariant, swept across four
magnitudes and twenty clause shapes rather than argued case by case: an infeasible query
under `DIAGNOSE` comes back with either a repair that makes it solve, or an explicit
"could not analyse" — never a crash, never silence, never a repair that leaves the query
infeasible. It exists because hand-picked cases kept missing whole classes; every defect it
caught was in the arithmetic *around* the repair rather than in the repair itself, and every
one of them produced advice that looked reasonable and did not work.

**Tests** (`test_query_diagnostics_relation.py::TestRepairsTheModelCanReach`). The five cases
of the retained-constants table — two controls with no encoding at all, then a lowered `<>`,
an L0 `norm` and a McCormick product — are asserted three ways. `assert_backends_agree` across
Gurobi native, Gurobi lowered and HiGHS catches a constant that only one formulation bakes.
The promised payoff is oracled against a real re-solve of the edited query. And each case
carries a **witness**: a single edit, verified by its own solve, that restores feasibility on
its own; the diagnosis must not promise less than the witness delivers. The witness is the
only one of the three that covers the McCormick row, because both backends lower a product
the same way, so a wrong constant there would be wrong by the same amount on both.

**The ABS and MIN/MAX cases** run across three arms via `assert_backends_agree`; the lowered arm is exercised **on Gurobi** so the comparison is of the two
formulations rather than of two solvers that happen to differ. The reported repair is checked
to be the better one, with its stated payoff oracled against a real re-solve of the edited
query. `test_the_abs_clause_wins_when_it_is_the_smaller_edit` proves the rule is not "always
prefer the bound": weighting `x` inside the ABS breaks the symmetry that made the two repairs
tie, the clause becomes the smallest edit at 4.5 against the box's 9, and it is reported with
no tie-break consulted. The MIN/MAX tie runs `DECIDB_NATIVE_CONSTRUCTS=force` as its own arm,
because MIN/MAX goes native only where the Big-M is underivable and the native path is
otherwise never reached.

## An `IN`-only conflict returns an undiagnosed finding

`x IN (3,5) AND x IN (2,4)` has no elastic edit to report: `IN` over a decision is lowered to
a membership disjunction whose rows are all mechanism, so the empty-block guard finds no
relaxable block. Under `DIAGNOSE`, the operator turns that invalid engine result into one
`edit_source='undiagnosed'` finding explaining that no loosening of the written clauses
restores feasibility. Without the prefix, the same query raises the state-only infeasible
error. The same guard covers `<>` and McCormick mechanism rows.

## Elastic engine: column-bound conflicts (intrinsic reset, inverted box, type-domain errors)

A user limit can live in a variable's **column bounds** instead of a matrix row, where the
row-only elastic engine can't relax it. The built `SolverModel`'s `col_lower`/`col_upper` is a
*fused* product — intrinsic domain (REAL/INT `[0,+∞)`, BOOLEAN `[0,1]`) + folded user bounds +
implied tightenings from `DecidePropagateImpliedBounds` (a presolve pass with no provenance).
Three mechanisms make every column-derived user limit diagnosable while keeping the intrinsic
domain rigid:

- **Box widening (only the intrinsic domain stays rigid).** The repair model's
  `OpenElasticColumnBox` (`physical_decide.cpp`) widens every decide column direction that
  is narrower than the rigid box (`SolverInput::rigid_lower_bounds` / `rigid_upper_bounds`:
  the intrinsic domain, captured before propagation with every folded user direction
  re-opened) — folded user bounds and implied tightenings alike — by the bounded
  per-column amount described above. This is safe: propagation only ever tightens (raises
  lower / lowers upper), every implied tightening has a backing row (`USER_PARAMETER`
  slackable, or `STRUCTURAL` still-rigid) that keeps enforcing it, and every folded bound
  is enforced by its own clause's rows. This is what lets `x <= 2+3` (a
  foldable cap copied into the column box) be loosened by its row instead of pinned by the
  box. A BOOLEAN-domain column (`SolverInput::variable_types[var] == BOOLEAN`) is clamped
  to `[0,1]` and never widens past it, so a pin on one opens only back to its domain and a
  BOOLEAN implied-tightened below 1 (the budget example) opens back to 1.
- **Inverted-box survival (`SolverInput::tolerate_infeasible_bounds`).** Two opposite folded
  bounds (`x <= 4 AND x >= 10`) invert the box (`col_lower > col_upper`). Without help,
  `SolverModel::Build` throws before `retained_model` is populated, so the engine never sees the
  model. Under `DIAGNOSE` the flag (set in `Finalize`, same trigger as the unbounded-ray extraction)
  makes `Build` keep the inverted box, and `SolveModel` short-circuits to INFEASIBLE **without
  handing the box to the backend** — HiGHS hard-rejects `lb>ub` at load and poisons the session;
  Gurobi tolerates it, so the short-circuit is the solver-agnostic path. The box widening then
  un-inverts the box for the elastic re-solve, which loosens one of the two bounds.
- **A conflict with the type's own range is a static error, not an elastic edit.** A user
  bound that contradicts the default floor of a REAL or INTEGER (`x <= -1` with no lower bound
  written), or the `[0,1]` box of a BOOLEAN (`x >= 2`, `x <= -1`), can't be fixed by loosening
  a clause (only by changing the type, writing a lower bound or dropping the bound), so it
  never reaches the engine. A check in the sink constructor (`DecideGlobalSinkState`, over
  the box `DecideOptimizer::FoldSingletonRows` resolved) throws a static
  `InvalidInputException`, under `DIAGNOSE` too:
  `"DECIDE optimization is infeasible: x <= -1 cannot hold because x is non-negative (>= 0). …"`
  and `"DECIDE optimization is infeasible: x >= 2 cannot hold for a BOOL variable."`, each
  quoting the bound as written. The guard is `U < 0 AND L >= 0` (upper below the
  non-negativity floor) or, for a BOOLEAN, a box outside `[0,1]`, so an explicitly-lowered
  floor (`x <= -1 AND x >= -5`, box `[-5,-1]`) stays feasible and a purely user-vs-user
  inverted box (`x >= 5 AND x <= 1`, both ≥ 0) proceeds to the elastic engine. The same bound
  under a `when` is a row, not a box, and is diagnosed like any clause.

### Scalar-subquery bound provenance

A scalar subquery becomes a column ref after `PlanSubqueries`, so correlation provenance is
collected from **both comparison sides** before flattening. The flattened value receives
`QUERY_WIDE_VALUE_TAG` when uncorrelated or `ROW_VARYING_SUBQUERY_TAG` when correlated.
`DecideCanonicalizer` then classifies the complete rebuilt RHS after side swapping and term
migration: an all-query-wide bound receives `QUERY_WIDE_BOUND_TAG`; any row-varying component
keeps it `PER_ROW_DATA`. Physical evaluation consumes that root classification through
`rhs_is_shared_scalar` rather than inferring provenance from the original RHS position.
Diagnostics therefore treat `x <= (SELECT 5)` and `(SELECT 5) >= x` identically, while mixed or
correlated bounds remain data-backed and never expose the internal `SUBQUERY` placeholder.

## Infeasibility reporting: one finding per edit, fixed vocabulary

Every edit the engine finds is one row, and its kind is the `edit_source` column, whose full value set is
in `../../language/guide.md` §8.

There is no summary line. Under `DIAGNOSE` the relation IS the answer: every clause it
names is a row, so there is nothing to summarize and nothing to truncate. A query with
dozens of failing classes returns dozens of rows, which is the right shape for a
relation — the user filters, aggregates, or `LIMIT`s it themselves.

**Why no runnable rewritten query.** The engine has no access to the original SQL text (it
works on `SolverModel` rows, and clause labels are *reconstructed* + canonicalized — the user's
`5 >= x` comes back as `x <= 5`), so a faithful full query is unbuildable and a reconstructed
partial would duplicate what the table already carries. The table is the source of truth; the
flat finding is the repair fragment the engine can state faithfully.

**Fixed flat vocabulary.** Every result row is one finding in the fixed schema
`state | clause | suggested_change | amount | total | scope | edit_source | group | row`.
`total` and `scope` are reserved for counted unbounded escapes and remain NULL for
infeasibility findings. The
`edit_source` column is the stable kind discriminator:

- clause-level loosens: `source_literal` or `virtual_offset`;
- expanded profiles: `expanded_row` or `expanded_group`, with identity in `row` or `group`;
- remove-only clauses and special findings: `remove_only`, `unreachable_bound`,
  `rigid_conflict`, `undiagnosed`;
- model-level facts: `achievable_objective` or `unbounded_after_fix`.

A data conflict is an actionable `virtual_offset` finding in query mode or an `expanded_row`
profile in expanded mode, never a dead end.

**Tests.** C++ (`test_decidb_diagnostic_engines.cpp`) asserts the typed schema, renderer,
statement-scoped handoff, loosen/drop builders, virtual offsets, and expanded identities.
Python differential (`test_query_diagnostics_relation.py`) asserts the flat relation directly
and then applies its reported edits: query-mode data uses `virtual_offset`, expanded data uses
`expanded_row`, and remove-only clauses use `remove_only`.

## Tests

`TestUnreachableBound` in `test/decide/tests/test_query_diagnostics_relation.py` covers the
pre-solve verdict: the bare, aggregate, and hard MIN/MAX spellings each name their clause and
emit `unreachable_bound` with no edit rows; one clause over three relation rows reports once;
two out-of-reach bounds are two findings; a vacuous `x <= +inf` is not a finding and the real
conflict beside it still gets an ordinary loosen edit; and a merely far-out-of-reach finite
bound still goes through the elastic solve. The infinite-bound cases run on both backends;
`test_infinite_bounds.py` (`test_highs_answers_an_unreachable_bound_as_infeasible`,
`test_highs_still_solves_a_vacuous_infinity`) pins that the unprefixed verdict keys on direction
rather than on the infinity. The semantics side is in
`test/decide/tests/test_infinite_bounds.py`, whose unreachable cases assert the named
verdict rather than just the word "infeasible".

`test/common/test_decidb_diagnostic_engines.cpp` — SECTIONs drive `DiagnoseInfeasible`
against the bundled HiGHS backend on one-variable models: a relaxable cap conflicting with
a rigid floor reports the unique minimal loosening (`x <= 5` → `x <= 10`, amount 5); an
equality row loosens via its two-sided slack (`x = 5` → `x = 8`); a rigid-only conflict
with a non-helping relaxable row renders the elastic-infeasible row; and an all-rigid
model returns invalid (→ `undiagnosed` at the operator). End-to-end differential coverage
(both backends, vs `oracle_solver`, including singleton-row-bound and BETWEEN cases) is in
`test/decide/tests/test_query_diagnostics_relation.py`. That file's `_apply_reported_fix`
harness guards the core least-change promise mechanically: for each asserted infeasible
diagnosis it applies every reported edit to the SQL (loosen → replace the clause with its
`suggested_change`; drop → remove the clause) and asserts the edited query actually solves.
BOOLEAN user-pin coverage (`x >= 1`, `x = 1`, `x <= 0` pins, plus a domain-restatement
guard asserting `x <= 1 AND x >= 0` never becomes an editable knob) lives there too. The
keyed fold has three label tests in the same file. Two run on TPC-H so the CLI stays
read-only: one class per group
(`test_infeasible_entity_scoped_label_drops_accumulated_coefficient`:
`DECIDE per r: keepR(BOOL)` with `per r.r_name: sum(keepR) by (r.r_name) >= 6` over
region ⋈ nation, where each region folds 5 rows onto one column) must not render
`sum(5*keepR)`; mixed multiplicities in one group
(`test_infeasible_entity_scoped_label_survives_mixed_multiplicity`:
`DECIDE per n: keepN(BOOL)` with `per r.r_name: sum(keepN) by (r.r_name) >= 1000` over
customer ⋈ nation ⋈ region, where nations carry 50-72 customers each) must not render
`sum(keepN * 1)`. Both
run `_apply_reported_fix`, so the label is proven pasteable, not just pretty. The third
(`test_infeasible_row_scoped_label_unaffected_by_fold_rendering`, over `VALUES`) pins that
a decision per row, `per grp: sum(buy) by (grp) >= 5`, keeps the fan-out path. The
genuinely data-weighted keyed clause (`SUM(keepS * cost)`) is verified manually only — no
automated case yet.
