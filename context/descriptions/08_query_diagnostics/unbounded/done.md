# Query Diagnostics — Unbounded (how it works)

The objective improves without limit — the feasible region is too *open* in the
improving direction. The fix is forced (the user must add a bound or correct a
sign; you cannot relax your way out), but the diagnosis is rich: it **names the
exact variables escaping to infinity** and **prescribes the forced remedy** (add
an upper bound) without inventing the cap value. Opt-in via `PRAGMA
diagnose_decide` = `unbounded` or `auto`; with no pragma the solve throws the
static error unchanged.

This doc describes the shipped behavior, topic by topic. Remaining enrichments are
in `todo.md`. Shared plumbing it builds on (the pragma gate, provenance, the
reporting relation) is in `foundations/done.md`.

## Status disambiguation (`INFEASIBLE` vs `UNBOUNDED`)

A presolve can report the ambiguous "infeasible *or* unbounded" without deciding
which. Both backends are normalized to a definitive status before diagnosis runs:

- **Gurobi** — on `GRB_INF_OR_UNBD`, re-solve once with `DualReductions=0`, which
  forces a definitive `INFEASIBLE` / `UNBOUNDED` (`gurobi_solver.cpp`).
- **HiGHS** — has no MIP disambiguation equivalent. The portable classifier
  re-solves the *same* `SolverModel` on the same backend with the objective
  zeroed out (`diagnostic_solves.cpp`): a feasible probe ⇒ the original was
  `UNBOUNDED`; an infeasible probe ⇒ `INFEASIBLE`; anything else preserves the
  ambiguous status. The zero-objective probe is sense-agnostic (works for
  MAXIMIZE and MINIMIZE).

The solve facade (`ilp_solver.cpp`) builds the model and selects the backend once,
and performs this classification before the operator sees the result.

## Ray extraction (portable box-LP)

To name escapers we need a recession ray (a direction the solver can travel
forever improving the objective). Rather than depend on solver-specific ray APIs,
DeciDB extracts one with a portable LP built over the prepared `SolverModel`
(`BuildUnboundedRayFallbackModel`, `diagnostic_solves.cpp`):

- maximize `signed(c)ᵀ d` (the objective direction),
- preserve each linear row's sense with RHS `0` (homogenized),
- relax all variables to continuous,
- box each direction component to `0 ≤ dᵢ ≤ 1` **only** where the original upper
  bound is effectively infinite (`≥ 1e20`); finite-upper-bound columns are fixed
  to `dᵢ = 0`.

The ray is attached (`SolverResult::ray`) only when this LP is `OPTIMAL` with
signed objective improvement > `1e-8`. It is opt-in (`SolveModelOptions::
extract_unbounded_ray`), pre-armed by the pragma gate only for unbounded/auto, so
the default failure path pays nothing. Quadratic objectives/constraints are out of
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
columns to the source expression they were generated from. The diagnosis site
(`PhysicalDecide` Finalize) collects columns with `|ray[i]| > 1e-8`, groups them by
`decide_var_idx`, and collects each variable's escaping **scope-instances** (the
`ColumnProvenance.instance`: the row for row-scoped, the entity id for
entity-scoped). It emits **one row per escaping variable** carrying its name,
direction, and a characterization of *which* instances escape (next section).
`BuildUnboundedDiagnostic` is the pure formatter over that per-variable summary.

**Only user variables escape in practice (verified, both backends).** Auxiliary
variables are structurally bounded — ABS Big-M and bilinear McCormick require
finite bounds (they error before the solver), and MIN/MAX/`<>` indicators are
BOOLEAN `[0,1]`. The aux→expression naming is therefore *defensive* (correct if an
aux ever escapes); the practical escapers are user INTEGER/REAL variables.

## The output relation

The diagnosis surfaces through `decide_diagnostics()` (schema in
`foundations/done.md`) as a **variable-centric** relation — one row per escaping
variable:

    query_id | state | variable | direction | escaping_instances

- `variable` — the escaping variable's name.
- `direction` — the sign of its ray entry, as ASCII `+inf` / `-inf` (not the Unicode
  `±∞` glyph: ASCII is robust in CSV exports, `WHERE direction = '+inf'` filters, and
  non-UTF-8 terminals). Always `+inf` today: user variables are non-negative
  (`[0, 1e30]`), so escape is always upward. The sign is computed from the ray, so a
  future signed/free variable would report `-inf` — but that path is unreachable and
  untested until signed variables exist (see `todo.md` and
  `03_expressivity/decide/todo.md`).
- `query_id` — ties together the rows of one failed solve.
- `escaping_instances` — which instances of the variable escape (next section).

The forced remedy (add a bound) is a single statement that applies to every
escaping variable, so it is carried in the **summary** (stderr), not as a per-row
column. (An always-NULL `suggested_bound` column previously shipped here; it was
dropped — DeciDB never picks the cap value, so there was nothing per-variable to
report.) When any row carries categorical rules, the summary also appends a
one-line legend for the `escaping_instances` cell format (`c=v (a/b)` = a of b
instances where `c=v` escape; `; `-separated rules are alternatives).

The error thrown points the user at the relation: `SELECT * FROM
decide_diagnostics() (this session)` — see "How the failure is surfaced" below.

## How the failure is surfaced (error messaging)

Two error-text behaviors close the loop between a failed solve and its diagnosis:

- **Advertise the opt-in (manual-first).** With no pragma set, an unbounded solve
  throws the legacy static error (`ThrowDecideSolveError`, UNBOUNDED branch,
  `ilp_solver.cpp`). That error now ends with one pointer line — *"For a diagnosis
  of which variable is unbounded, set `PRAGMA diagnose_decide='auto'` and
  re-run."* — so the user learns the diagnosis exists without DeciDB spending a
  second solve on their behalf. Only the unbounded branch advertises; the
  infeasible/slow branches stay silent because their engines don't exist yet
  (advertising them would over-promise).
- **Same-session caveat.** When the pragma *is* set and a diagnosis is stashed, the
  thrown error reads *"Diagnosis ready (this session): SELECT * FROM
  decide_diagnostics();"* (`ThrowDecideDiagnosisReady`, `decide_diagnostic.cpp`).
  The stash is per-connection, so a fresh connection (a second `decidb -c …`) gets
  an empty relation; the "(this session)" qualifier sets that expectation. A
  richer empty-stash explanatory row was considered and deferred.

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
column pruning; columns referenced only in the outer SELECT fall back to a
positional `colN` name (almost always high-cardinality, hence excluded anyway).

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
blame. This shapes every clause-aware enrichment in `todo.md`.

## Tests

Differential vs `oracle_solver` / pinned scenarios:
`test/decide/tests/test_query_diagnostics_f6.py` (REAL var, INTEGER/MILP via the
HiGHS `INF_OR_UNBD` path, multi-var dedup, `auto` routing, no-pragma silence) ·
`test/decide/tests/test_query_diagnostics_escaping_instances.py` (row-scoped
partial → categorical rule, total escape, scattered → count fallback,
entity-scoped → entity-key rule, the escape-rate pragma changing what is reported;
both backends) · `test/decide/tests/test_query_diagnostics_f5.py` (the renamed
`escaping_instances` schema) · `test/common/test_decidb_escape_characterization.cpp`
(the pure `CharacterizeEscape` core: threshold gating, union/sort, all-escape,
excluded instances, empty-rules fallback) · `test/common/test_decidb_diagnostic_solves.cpp`
(ray fallback: full-support ray, signed objective, finite-UB zeroing, row-sense
preservation, integrality relaxation, opt-in attachment) ·
`test/common/test_decidb_variable_provenance.cpp` (USER/AUX/GLOBAL_AUX resolution).
The characterization string is asserted against constructed cases (oracle/pinned
confirm only the UNBOUNDED status). Demoed end-to-end on the TPC-H DB via `run.sh`
(a 1-variable `part` model where `buy` is uncapped for `Manufacturer#1` rows, so
the diagnosis reports `buy`, `+inf`, `p_mfgr=Manufacturer#1 (29/29)`).
