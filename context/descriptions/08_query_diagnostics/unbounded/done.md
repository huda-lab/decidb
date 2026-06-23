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
`direction`, and `escaping_instances` when there is scope multiplicity to
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
- `attribute = direction` — the sign of its ray entry, as ASCII `+inf` / `-inf`
  (not the Unicode `±∞` glyph: ASCII is robust in CSV exports, EAV filters such as
  `WHERE attribute = 'direction' AND value = '+inf'`, and non-UTF-8 terminals).
  Always `+inf` today: user variables are non-negative
  (`[0, 1e30]`), so escape is always upward. The sign is computed from the ray, so a
  future signed/free variable would report `-inf` — but that path is unreachable and
  untested until signed variables exist (see `todo.md` and
  `03_expressivity/decide/todo.md`).
- `diagnosis_id` — ties together the rows of one failed solve.
- `attribute = escaping_instances` — which instances of the variable escape (next
  section). This row is omitted for name-only cases where the value would be NULL
  (aux/name-only or a single-instance variable).

The forced remedy (add a bound) is a single statement that applies to every
escaping variable, so it is carried in the **summary** (stderr), not as a per-row
attribute. (An always-NULL `suggested_bound` column previously shipped here; it was
dropped — DeciDB never picks the cap value, so there was nothing per-variable to
report.) When any row carries categorical rules, the summary also appends a
one-line legend for the `escaping_instances` value format (`c=v (a/b)` = a of b
instances where `c=v` escape; `; `-separated rules are alternatives).

The error thrown points the user at the relation: `SELECT * FROM
decide_diagnostics() (this session)` — see "How the failure is surfaced" below.

## How the failure is surfaced (error messaging)

Two error-text behaviors close the loop between a failed solve and its diagnosis:

- **Point back to diagnosis when it was turned off.** With `diagnose_decide='off'`
  an unbounded solve throws the legacy static error (`ThrowDecideSolveError`,
  UNBOUNDED branch, `ilp_solver.cpp`). That error ends with one pointer line — *"For
  a diagnosis of which variable is unbounded, set `PRAGMA diagnose_decide='auto'`
  and re-run."* — so a user who suppressed diagnosis is reminded how to get it back.
  (Under the `auto` default this branch is not reached for UNBOUNDED: the solve is
  diagnosed and throws the "diagnosis ready" pointer instead.) Only the unbounded
  branch advertises; the infeasible/slow branches stay silent because their engines
  don't exist yet (advertising them would over-promise).
- **Same-session caveat.** When diagnosis runs and a diagnosis is stashed, the
  thrown error reads *"Diagnosis ready (this session): SELECT * FROM
  decide_diagnostics();"* (`ThrowDecideDiagnosisReady`, `decide_diagnostic.cpp`).
  The stash is per-connection, so a fresh connection (a second `decidb -c …`) gets
  an empty relation; the "(this session)" qualifier sets that expectation. A
  richer empty-stash explanatory row was considered and deferred.
- **Variable, not clause blame.** The stashed-diagnosis summary also says the
  diagnosis names the runaway variable, not a single guilty clause. That mirrors
  the ray's information limit: a missing bound and a flipped-sign constraint can
  produce the same recession direction.
- **"Unavailable", not the re-run advert, when diagnosis ran but came up empty.**
  A content-free unbounded solve (a quadratic model, or a ray that escaped only via
  internal auxiliaries) falls through even though diagnosis was active. In
  that case the error must not point the user back at `diagnose_decide='auto'` — it
  is already on (or default) and re-running cannot help. Instead
  `ThrowUnboundedDiagnosisUnavailable` (`decide_diagnostic.cpp`) keeps the
  bound-the-variables guidance and ends with *"Unbounded diagnosis unavailable: …"*
  naming the reason: an **empty ray** ⇒ the model is quadratic
  (`BuildUnboundedRayFallbackModel` declines quadratics), a **present ray with no
  named variable** ⇒ only internal auxiliaries escaped. The re-run advert is
  therefore reached only when diagnosis was turned `off`; `physical_decide.cpp`
  picks between the two at that fall-through.

## `escaping_instances` — characterizing which instances escape

A variable name fans out into many scope-instances (row-scoped: one column per
result row; entity-scoped: one per entity). When only *some* escape, the cell says
which, so a partial escape is distinguishable from a total one and a localized
modeling error (a sign flip on one category, a `WHEN`/`PER` cap that skipped a
slice) is visible. The cell is one of:

- **Total escape** (`escaping == total`): `all N instances escape`.
- **Categorical rules**: for every categorical column `c` and value `v`, the
  within-group escape rate `a/b` (a = escaping instances with `c=v`, b = group
  size) is computed; every group with rate ≥ the escape-rate threshold is reported
  as `c=v (a/b)`, `; `-joined, strongest first. This is the *sufficient direction*
  ("when `c=v`, the variable escapes in a of b instances"); the `a/b` count is
  always shown so a threshold < 1 stays honest. Rules are an independent **union**
  across columns/values (conjunctions are future work).
- **Fallback** (single-instance variable, or a scattered escape no categorical
  group characterizes): the bare count `a of b instances escape` (single-instance
  variables read NULL — nothing to disambiguate). Aux columns are name-only.

**Mechanism.** Categorical grouping reuses `BuildGroupIds` (one scan per candidate
column; it returns per-group representative values). A column is a candidate when
`2 ≤ distinct ≤ max(min_categories, ratio × N)` — relative cardinality excludes
continuous / id-like columns (out of scope by design). **Row-scoped** variables
scan all input columns; **entity-scoped** variables scan only the scope's
entity-key columns (the columns constant within an entity), lifting the row
grouping to entity granularity via the live entity mapping on the `VarIndexer`. The
pure rule computation is `CharacterizeEscape` (no DuckDB execution types — unit
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
blame. The current summary states this caveat directly so users do not hunt for
clause blame in `decide_diagnostics()`. This shapes every clause-aware enrichment
in `todo.md`.

## Tests

Differential vs `oracle_solver` / pinned scenarios:
`test/decide/tests/test_query_diagnostics_f6.py` (REAL var, INTEGER/MILP via the
HiGHS `INF_OR_UNBD` path, multi-var dedup, `auto` routing, `off` suppression) ·
`test/decide/tests/test_query_diagnostics_escaping_instances.py` (row-scoped
partial → categorical rule, total escape, scattered → count fallback,
entity-scoped → entity-key rule, all three characterization pragmas
`escape_rate` / `categorical_ratio` / `min_categories` changing what is reported;
both backends, with oracle-confirmed unbounded constructed cases for the
cardinality-knob tests) · `test/decide/tests/test_query_diagnostics_f5.py` (the renamed
EAV `decide_diagnostics()` schema and `diagnosis_id`) ·
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
the diagnosis reports EAV rows for `buy` / `direction` / `+inf` and
`buy` / `escaping_instances` / `p_mfgr=Manufacturer#1 (29/29)`).
