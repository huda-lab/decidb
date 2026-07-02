# Query Diagnostics — Unbounded (how it works)

> Router terminal: **failed → unbounded** (`find ray` → report). See `router/README.md`.

The objective improves without limit — the feasible region is too *open* in the
improving direction. The fix is forced (the user must add a bound or correct a
sign; you cannot relax your way out), but the diagnosis is rich: it **names the
exact variables escaping to infinity** and **prescribes the forced remedy** (add
an upper bound) without inventing the cap value. On by default (`PRAGMA
diagnose_decide` is `auto`); set `diagnose_decide='off'` to suppress diagnosis and
get the plain static error instead.

This doc describes the shipped behavior, topic by topic. Remaining enrichments are
in `todo.md`. Shared plumbing it builds on (the pragma gate, provenance, the
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
extract_unbounded_ray`), pre-armed by the pragma gate under `auto` (the default)
and skipped under `off`, so a suppressed failure path pays nothing. Quadratic objectives/constraints are out of
scope (no ray extracted). When the ray names no variable — a quadratic model, or a
ray in which only internal auxiliaries escaped — the diagnosis has no per-variable
content, so it is **not** stashed: the operator falls through to the rich static
`ThrowDecideSolveError` instead of advertising a content-free all-NULL relation.

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
entity-scoped). It emits EAV attributes for each escaping variable: always
`grows_toward`, and `affected_rows` / `affected_entities` when there is scope multiplicity to
characterize (next section). `BuildUnboundedDiagnostic` is the pure formatter over
that per-variable summary.

**Only user variables escape in practice (verified, both backends).** Auxiliary
variables are structurally bounded — ABS Big-M and bilinear McCormick require
finite bounds (they error before the solver), and MIN/MAX/`<>` indicators are
BOOLEAN `[0,1]`. The aux→expression naming is therefore *defensive* (correct if an
aux ever escapes); the practical escapers are user INTEGER/REAL variables.

## The output relation

The diagnosis surfaces through `decide_diagnostics()` (schema in
`foundations/done.md`) as long-form EAV rows:

    diagnosis_id | state | subject_kind | subject | attribute | value

- `subject_kind` — `variable` for every unbounded row.
- `subject` — the escaping variable's name.
- `attribute = grows_toward` — the sign of its ray entry, as ASCII `+inf` / `-inf`
  (not the Unicode `±∞` glyph: ASCII is robust in CSV exports, EAV filters such as
  `WHERE attribute = 'grows_toward' AND value = '+inf'`, and non-UTF-8 terminals).
  Always `+inf` today: user variables are non-negative
  (`[0, 1e30]`), so escape is always upward. The sign is computed from the ray, so a
  future signed/free variable would report `-inf` — but that path is unreachable and
  untested until signed variables exist (see `todo.md` and
  `03_expressivity/decide/todo.md`).
- `diagnosis_id` — ties together the rows of one failed solve.
- `attribute = affected_rows` (row-scoped) / `affected_entities` (entity-scoped) —
  which rows/entities of the variable escape (next section). This row is omitted for
  name-only cases where the value would be NULL (aux/name-only or a single-instance
  variable).

The forced remedy (add a bound) is a single statement that applies to every
escaping variable, so it is carried in the **summary** (stderr), not as a per-row
attribute. (An always-NULL `suggested_bound` column previously shipped here; it was
dropped — DeciDB never picks the cap value, so there was nothing per-variable to
report.) The `affected_rows` / `affected_entities` value is **self-describing**
(`a of b rows where c = 'v'`), so the summary carries **no** legend — a deliberate
declutter for the SQL-user audience (no `c=v (a/b)` shorthand to decode).

The error thrown points the user at the relation: `Details: SELECT * FROM
decide_diagnostics();` — see "How the failure is surfaced" below.

## How the failure is surfaced (error messaging)

All failure text follows one rule (the **"user-facing output is for SQL users"**
principle): one line naming the state + the smallest fix, no solver/LP jargon, no
bullet-list lectures, no meta-commentary on how the diagnosis was derived. Detail
lives in the opt-in relation, not the error.

- **Point back to diagnosis when it was turned off.** With `diagnose_decide='off'`
  an unbounded solve throws the static error (`ThrowDecideSolveError`, UNBOUNDED
  branch, `ilp_solver.cpp`): *"DECIDE optimization is unbounded: a decision variable
  can grow without bound. Add an upper bound, e.g. SUCH THAT x <= <cap>. For the
  variable, set `PRAGMA diagnose_decide='auto'` and re-run."* — so a user who
  suppressed diagnosis is reminded how to get the per-variable detail back. (Under
  the `auto` default this branch is not reached for UNBOUNDED: the solve is diagnosed
  and throws the `Details:` pointer instead.) The unbounded static branch is the
  only one that advertises how to re-enable diagnosis; infeasible now has its own
  elastic engine under `auto`, while slow stays silent until its engine exists.
- **The relation pointer.** When a diagnosis is stashed, the thrown error ends with
  *"Details: SELECT * FROM decide_diagnostics();"* (`ThrowDecideDiagnosisReady`,
  `decide_diagnostic.cpp`). The stash is per-connection, so a fresh connection gets
  an empty relation; the earlier *"(this session)"* qualifier was dropped for
  concision (the per-connection lifecycle still holds — it is just no longer spelled
  out in the error).
- **No clause-blame caveat in the message.** Earlier the summary appended *"names the
  runaway variable, not a single guilty clause."* That sentence was **removed** from
  stderr: it answers a question a SQL user has not asked and spends the most prominent
  line on a limitation rather than the fix. The limit itself still holds (a missing
  bound and a flipped-sign constraint share a recession direction) — it lives in this
  doc (see "Load-bearing limit"), not in the user's error.
- **"Unavailable" reason, not the re-run advert, when diagnosis ran but came up
  empty.** A content-free unbounded solve (a quadratic model, or a ray that escaped
  only via internal auxiliaries) falls through even though diagnosis was active. The
  error must not point the user back at `diagnose_decide='auto'` — it is already on.
  `ThrowUnboundedDiagnosisUnavailable` (`decide_diagnostic.cpp`) instead names the
  reason and keeps the fix: *"DECIDE optimization is unbounded: a non-linear term
  prevents naming the variable. Add an upper bound, e.g. SUCH THAT x <= <cap>."*
  (empty ray ⇒ quadratic; `BuildUnboundedRayFallbackModel` declines quadratics) or
  *"…the runaway is an internal helper variable…"* (present ray, only internal
  auxiliaries escaped). The re-run advert is reached only when diagnosis was turned
  `off`; `physical_decide.cpp` picks the reason at that fall-through.

## `affected_rows` / `affected_entities` — characterizing which rows escape

A variable name fans out into many scope-instances (row-scoped: one column per
result row → `affected_rows`; entity-scoped: one per entity → `affected_entities`).
When only *some* escape, the cell says which, so a partial escape is distinguishable
from a total one and a localized modeling error (a sign flip on one category, a
`WHEN`/`PER` cap that skipped a slice) is visible. Every cell is **self-describing**
(no legend); it is one of:

- **Total escape** (`escaping == total`): `all N rows` (or `all N entities`).
- **Categorical rules**: for every categorical column `c` and value `v`, the
  within-group escape rate `a/b` (a = escaping rows with `c=v`, b = group size) is
  computed; every group with rate ≥ the escape-rate threshold is reported as
  `a of b rows where c = 'v'`, `; `-joined, strongest first. This is the *sufficient
  direction* ("when `c=v`, the variable escapes in a of b rows"); the `a/b` count is
  always shown so a threshold < 1 stays honest. Rules are an independent **union**
  across columns/values (conjunctions are tracked in `todo.md`).
- **Fallback** (single-instance variable, or a scattered escape no categorical
  group characterizes): the bare count `a of b rows` (single-instance variables read
  NULL — nothing to disambiguate). Aux columns are name-only. Entity-scoped variables
  read `entities` in place of `rows` throughout.

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
tested). Column names are harvested at physical-plan time from the DECIDE clause's
own `BoundReferenceExpression`s (`plan_decide.cpp`), whose chunk indices survive
column pruning. Columns referenced only in the outer SELECT carry no harvested
name (we never saw the identifier the user wrote); a categorical candidate over
such an unnamed column is **suppressed** rather than labeled with a positional
`colN` the user never typed (`build_row_grouping` returns false on an empty name) —
the variable then falls back to the bare count. Such columns are usually
high-cardinality and excluded by the categorical cap anyway; suppression covers the
low-cardinality case where the cap would otherwise let an unnamed column through.

**Pragma knobs** (extension options, `decide_diagnostic.cpp`):
`diagnose_decide_escape_rate` (default 0.8), `diagnose_decide_categorical_ratio`
(default 0.1), `diagnose_decide_min_categories` (default 20 — an absolute floor so
small tables still qualify).

`PER` is covered only indirectly: we never map escaping instances onto PER groups,
but if escape aligns with a column a `PER`/`WHEN` clause uses, the categorical scan
surfaces it on its own.

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
multi-var dedup, `auto` routing, `off` suppression) ·
`test/decide/tests/test_query_diagnostics_escaping_instances.py` (row-scoped
partial → categorical rule, total escape, scattered → count fallback,
entity-scoped → entity-key and constant join-column rules, all three
characterization pragmas `escape_rate` / `categorical_ratio` / `min_categories`
changing what is reported plus validating their bounds;
both backends, with oracle-confirmed unbounded constructed cases for the
cardinality-knob tests) · `test/decide/tests/test_query_diagnostics_relation.py`
(the renamed EAV `decide_diagnostics()` schema and `diagnosis_id`) ·
`test/common/test_decidb_diagnostic_engines.cpp` (`DiagnoseUnbounded` with injected
grouping and a clause-shaped EAV stub row) · `test/common/test_decidb_escape_characterization.cpp`
(the pure `CharacterizeEscape` core: threshold gating, union/sort, all-escape,
excluded instances, empty-rules fallback) · `test/common/test_decidb_diagnostic_solves.cpp`
(ray fallback: full-support ray, signed objective, finite-UB zeroing, row-sense
preservation, integrality relaxation, opt-in attachment) ·
`test/common/test_decidb_variable_provenance.cpp` (USER/AUX/GLOBAL_AUX resolution).
The characterization string is asserted against constructed cases (oracle/pinned
confirm only the UNBOUNDED status). Demoed end-to-end on the TPC-H DB via `run.sh`
(a 1-variable `part` model where `buy` is uncapped for `Manufacturer#1` rows, so
the diagnosis reports EAV rows for `buy` / `grows_toward` / `+inf` and
`buy` / `affected_rows` / `29 of 29 rows where p_mfgr = 'Manufacturer#1'`).
