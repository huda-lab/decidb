# Query Diagnostics — Unbounded (how it works)

> Router terminal: **failed → unbounded** (`find ray` → report). See `router/README.md`.
> Reached only under the `DIAGNOSE` prefix — nothing else starts the engine.

The objective improves without limit — the feasible region is too *open* in the
improving direction. The fix is forced (the user must add a bound or correct a
sign; you cannot relax your way out), but the diagnosis is rich: it **names the
exact variables escaping to infinity** and **prescribes the forced remedy** (add
an upper bound) without inventing the cap value. Opt-in: `DIAGNOSE <query>` starts it,
and a query without the prefix gets the plain state-only error instead.

This doc describes the shipped behavior, topic by topic. Remaining enrichments are
in `todo.md`. Shared plumbing it builds on (the trigger, provenance, the
reporting relation) is in `foundations/done.md`.

## Status: reaching the unbounded engine

A presolve can report the ambiguous "infeasible *or* unbounded." The solve facade
normalizes that to a definitive status (a zero-objective feasibility probe) before
the engine runs — see `foundations/done.md` ("Structured solver result" and "Solver
behavior" for the per-backend specifics). A genuinely-unbounded solve therefore
arrives here as a clean `UNBOUNDED`.

## Ray extraction (portable box-LP)

To name escapers we need a recession ray — a direction the solver can travel forever
improving the objective. Rather than depend on solver-specific ray APIs, DeciDB
extracts one with a portable LP built over the prepared `SolverModel`
(`BuildUnboundedRayFallbackModel`, `diagnostic_solves.cpp`). For variables `d` over
the model's columns:

```
maximize    σ · cᵀd            σ = +1 if the model maximizes, −1 if it minimizes
subject to  Aⱼ · d  {≤,=,≥} 0   for every linear row j (its sense preserved, RHS homogenized to 0)
            0 ≤ dᵢ ≤ 1          where the original upper bound uᵢ ≥ EFFECTIVE_INFINITY
            dᵢ = 0               otherwise (a finite upper bound ⇒ that column cannot escape)
            d continuous         (integrality relaxed)
```

`σ` makes the objective always a *maximize* of the improving direction, so the box-LP
is sense-agnostic. Homogenizing every row to RHS `0` keeps only the recession cone of
the feasible region; the `[0,1]` box makes it bounded so the LP terminates, and the
per-column gate fixes finite-upper-bound columns to `0` so they never appear in `d`.
Quadratic objectives/constraints are declined (the builder returns no model).

A recession ray exists iff this LP is `OPTIMAL` with objective > `DIAGNOSTIC_RAY_EPSILON`;
then `d` is the ray and its nonzero entries are exactly the escaping columns. The ray
is attached to `SolverResult::ray` only in that case. These two thresholds —
`EFFECTIVE_INFINITY` (`1e20`) and `DIAGNOSTIC_RAY_EPSILON` (`1e-8`) — live together
in `decidb/diagnostic_constants.hpp` so the builder, solver, and engine that share
the box-LP "free suspect filter" invariant agree by construction (see
`foundations/done.md`). Ray extraction is gated (`SolveModelOptions::
extract_unbounded_ray`) and armed only by the statement's `DIAGNOSE` prefix. An
unprefixed query skips it, so its failure path pays nothing. Quadratic
objectives/constraints are out of scope (no ray extracted). When the ray names no
variable — a quadratic model, or a ray in which only internal auxiliaries escaped —
the operator returns one `edit_source='undiagnosed'` finding that explains why; the
relation is never empty and never points the user at a second statement.

**Suspect filtering is free:** because the box-LP fixes finite-UB columns to 0, a
non-zero ray entry *is already* the type/sign/bound-filtered set of suspects — no
extra filter needed.

## Naming the escaping variables

The ray gives non-zero entries per solver *column*. Each column is mapped back to
the user-facing variable through the `ColumnProvenance` map (variable provenance,
`foundations/done.md`): user columns resolve to the declared variable name, aux
columns to the source expression they were generated from. The `DiagnoseUnbounded`
engine collects columns with `|ray[i]| > DIAGNOSTIC_RAY_EPSILON`, groups them by `decide_var_idx`,
and collects each variable's escaping **scope-instances** (the
`ColumnProvenance.instance`: the row for row-scoped, the entity id for
entity-scoped). `BuildUnboundedDiagnostic` turns that summary into one typed finding
per escaping variable, or one per reported categorical slice: `clause` names the
variable, `suggested_change` prescribes its cap, `edit_source` carries the direction,
`group` names the slice, `amount` counts escaping instances, `total` retains the
applicable denominator, and `scope` says whether the instances are rows or entities.

**Only user variables escape in practice (verified, both backends).** Auxiliary
variables are structurally bounded — ABS Big-M and bilinear McCormick require
finite bounds (they error before the solver), and MIN/MAX/`<>` indicators are
BOOLEAN `[0,1]`. The aux→expression naming is therefore *defensive* (correct if an
aux ever escapes); the practical escapers are user INTEGER/REAL variables.

## The output relation

The diagnosis surfaces through the flat relation `DIAGNOSE` returns (full schema in
`foundations/done.md`):

    state | clause | suggested_change | amount | total | scope | edit_source | group | row

For an unbounded finding, `state='unbounded'`; `clause` is the escaping variable;
`suggested_change` is the forced remedy (`ship <= <cap>`) without an invented cap;
`edit_source` is `runaway_+inf` or `runaway_-inf`; `amount` is the number of escaping
scope-instances; `total` is the applicable whole-variable or categorical-slice count;
`scope` is `row` or `entity`; and `group` is the categorical slice when one is reported.
Thus `amount / total` reconstructs the escape rate that admitted a slice. `row` is NULL
for unbounded findings. A single-instance or auxiliary/name-only finding omits the
count, denominator, and scope when they would add no information.

Before Batch H the escape was one prose EAV cell (`a of b rows where c = 'v'`) and
the remedy lived in an stderr summary. The flat relation carries each component in its
own typed field: the slice in `group`, escaping and total instance counts in `amount`
and `total`, the instance kind in `scope`, and the remedy in `suggested_change`.

## How the failure is surfaced (error messaging)

All failure text follows one rule (the **"user-facing output is for SQL users"**
principle): one line naming the state + the smallest fix, no solver/LP jargon, no
bullet-list lectures, no meta-commentary on how the diagnosis was derived. Detail
lives in the opt-in relation, not the error.

- **Without the prefix, the state and nothing more.** An unprefixed unbounded solve
  throws the static error (`ThrowDecideSolveError`, UNBOUNDED branch,
  `ilp_solver.cpp`): *"DECIDE optimization is unbounded. Prefix the query with DIAGNOSE
  to see which decision needs a bound."* No variable is named, because naming it means
  extracting and walking the ray — work the user did not ask for. The one sentence that
  survives is the one that unblocks them.
- **Under the prefix, no error at all.** The findings are the statement's answer, so the
  query returns rows rather than raising.
- **No clause-blame caveat in the message.** Earlier the summary appended *"names the
  runaway variable, not a single guilty clause."* That sentence was **removed** from
  stderr: it answers a question a SQL user has not asked and spends the most prominent
  line on a limitation rather than the fix. The limit itself still holds (a missing
  bound and a flipped-sign constraint share a recession direction) — it lives in this
  doc (see "Load-bearing limit"), not in the user's error.
- **"Unavailable" reason, not the re-run advert, when diagnosis ran but came up
  empty.** A content-free unbounded solve (a quadratic model, or a ray that escaped
  only via internal auxiliaries) still needs a result even though no variable was
  named. It must not come back empty and it must not point at the prefix the user already used, so the
  engine reports one `edit_source = 'undiagnosed'` finding carrying the reason in
  `suggested_change` (`BuildUndiagnosedDiagnostic`, `decide_diagnostic.cpp`).
  `physical_decide.cpp` picks the reason from the retained
  model and helper status: a helper timeout reports that diagnosis ran out of time; an
  empty ray plus a quadratic objective/constraint reports that a non-linear term prevents
  naming the variable; an empty ray on a purely linear model uses the neutral *"the runaway
  variable could not be identified"* fallback; and a present ray that only names internal
  auxiliaries reports *"the runaway is an internal helper variable."*

## Escape characterization — which scope-instances escape

A variable name fans out into many scope-instances: one solver column per result row
for a row-scoped variable, or one per entity for an entity-scoped variable. The
characterizer distinguishes three cases:

- **Total escape** (`escaping == total`): one finding with `amount = total` and
  `group = NULL`.
- **Categorical rules**: for every categorical column `c` and value `v`, compute the
  within-group escape rate `a/b` (a = escaping instances with `c=v`, b = group size).
  Every group meeting the threshold becomes its own finding with
  `group = "c = 'v'"` and `amount = a`, strongest first. Rules are an independent
  **union** across columns/values (conjunctions are tracked in `todo.md`).
- **Fallback**: a scattered escape that no categorical group characterizes reports
  its escaping count in `amount` with `group = NULL`; single-instance and auxiliary
  name-only cases omit the count.

**Mechanism.** Categorical grouping reuses `BuildGroupIds` (one scan per candidate
column; it returns per-group representative values). A column is a candidate when
`2 ≤ distinct ≤ max(min_categories, ratio × N)` — relative cardinality excludes
continuous / id-like columns (out of scope by design). **Row-scoped** variables
scan all named input columns. **Entity-scoped** variables also scan named input
columns, then lift the row grouping to entity granularity via the live entity
mapping on the `VarIndexer`; the lift is accepted only when every joined row for
an entity has the same candidate value. This covers dimension-table labels such
as supplier→nation while skipping genuinely one-to-many joined columns. The pure
rule computation is `CharacterizeEscape` (no DuckDB execution types — unit
tested). Column names are resolved at physical-plan time (`plan_decide.cpp`) in two
passes, both keyed to chunk indices that survive column pruning. First, names are
harvested from the DECIDE clause's own `BoundReferenceExpression`s (WHEN / PER /
objective / constraint / entity-key columns). Second, any still-unnamed slot is
**back-filled from the child projection's user-written names** — a column referenced
only in the outer SELECT (never in the clause) is named from the child projection
expression when the name is a user-written identifier: an explicit `AS name` alias, or
a bare source-column reference. A computed projection expression with no alias carries
only a generated name (its `ToString`), which is deliberately dropped — a categorical
candidate over such a still-unnamed column is **suppressed** rather than labeled with a
machine name the user never typed (`build_row_grouping` returns false on an empty name),
and the variable falls back to the bare count. The projection back-fill is captured
before `CreatePlan` (like `child_bindings`) because `CreatePlan` moves the projection's
`expressions` vector out.

**Pragma knobs** (extension options, `decide_diagnostic.cpp`):
`diagnose_decide_escape_rate` (default 0.8), `diagnose_decide_categorical_ratio`
(default 0.1), `diagnose_decide_min_categories` (default 20 — an absolute floor so
small tables still qualify).

`PER` is covered only indirectly: we never map escaping instances onto PER groups,
but if escape aligns with a column a `PER`/`WHEN` clause uses, the categorical scan
surfaces it on its own. This is the whole of the group story by design — escape
localized to a PER/WHEN group is reported by the generic categorical scan when the
group key is a low-cardinality column; the engine does not otherwise map escapes onto
group structure, and it never phrases the result as blame on the group's clause (the
ray cannot finger a guilty clause — see the load-bearing limit above).

## Load-bearing limit — names a variable, not a guilty clause

The ray identifies a *missing* bound. It can name the runaway variable but
**cannot** finger a single guilty clause: a flipped-sign constraint is
mathematically indistinguishable from an absent one. So any clause-level output
can only ever be *context* ("`x` appears in clauses 2, 5; none cap it"), never
blame. This limit is **not** surfaced in the user error (it was removed for
concision — a SQL user needs the fix, not the caveat); it is recorded here because
it shapes every clause-aware enrichment in `todo.md`.

## Tests

Differential vs `oracle_solver` / pinned scenarios:
`test/decide/tests/test_query_diagnostics_unbounded_variables.py` (REAL var,
INTEGER/MILP via the HiGHS `INF_OR_UNBD` path, MINIMIZE improving direction,
multi-var dedup, prefix-triggered naming, and state-only unprefixed failures) ·
`test/decide/tests/test_query_diagnostics_escaping_instances.py` (row-scoped
partial → categorical rule, total escape, scattered → count fallback,
entity-scoped → entity-key and constant join-column rules, all three
characterization pragmas `escape_rate` / `categorical_ratio` / `min_categories`
changing what is reported plus validating their bounds; a SELECT-only column named
from its user-written projection alias vs. an unaliased computed column that stays
suppressed to the bare count;
both backends, with oracle-confirmed unbounded constructed cases for the
cardinality-knob tests) · `test/decide/tests/test_query_diagnostics_relation.py`
(the flat relation's schema and types, and that the diagnosis does not outlive its
statement) · `test/common/test_decidb_diagnostic_engines.cpp` (`DiagnoseUnbounded` with
injected grouping, plus the renderer and the statement-scoped handoff) · `test/common/test_decidb_escape_characterization.cpp`
(the pure `CharacterizeEscape` core: threshold gating, union/sort, all-escape,
excluded instances, empty-rules fallback) · `test/common/test_decidb_diagnostic_solves.cpp`
(ray fallback: full-support ray, signed objective, finite-UB zeroing, row-sense
preservation, integrality relaxation, opt-in attachment) ·
`test/common/test_decidb_variable_provenance.cpp` (USER/AUX/GLOBAL_AUX resolution).
The characterization string is asserted against constructed cases (oracle/pinned
confirm only the UNBOUNDED status). Demoed end-to-end on the TPC-H DB via `run.sh`
(a 1-variable `part` model where `buy` is uncapped for `Manufacturer#1` rows, so
the diagnosis reports one `runaway_+inf` finding for `buy` with
`group = "p_mfgr = 'Manufacturer#1'"`, `amount = 29`, and
`suggested_change = 'buy <= <cap>'`).
