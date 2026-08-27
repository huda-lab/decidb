# Website — full update

This is the website-owned work queue. Each entry below is meant to be picked up on its
own: it names the file, the exact text to change, the decision if one is still open, and
how to check the result.

**The canonical source is
[`../descriptions/00_project_overview/syntax_reference.md`](../descriptions/00_project_overview/syntax_reference.md)**
(frozen 2026-08-26). Verify every claim against that file and against a real run of
`build/release/decidb`. Do not copy behaviour from the paper or from the site's own
existing copy — both are older than the code.

The site was last updated **2026-08-08** (`f5d2a9cfa9`). About sixty commits have landed
since, several of which changed user-facing syntax. Everything below was checked against
the binary at `57c4926fd1` (reports `v0.2.0`, Gurobi installed) on 2026-08-27.

---

## Settled decisions

These are decided. No item below should reopen them.

1. **Stay inside the existing five pages.** New material becomes new sections in
   `documentation.html`. No new nav entries. (One new *example* page is allowed — see §5.)
2. **More than one right answer is fine.** A displayed result table only has to be *a*
   valid answer for its query, not the only one. Where several answers are equally good,
   add the variability note (§5) rather than rewriting the query to force a unique winner.
3. **NULL coefficients keep today's behaviour.** A NULL in a constraint or objective
   coefficient is an error that suggests `COALESCE()`. Document it as it is.
4. **`DIAGNOSE` works on exactly one DECIDE.** It cannot be attached to nested DECIDE
   subqueries or to a query containing several DECIDE operators. Say so plainly.
5. **The version is 0.2.0 and the Python package is live.** Update both.
6. **`index.html` may argue; the reference pages may not.** The landing page and its
   pitch — including the side-by-side against hand-written solver code — stay as they are.
   `documentation.html`, `getting-started.html`, `examples.html` and every page under
   `examples/` are description only. See the voice rule below. No item in this queue edits
   `index.html`.

---

## Ground rules

Apply to every item.

- **Every displayed query must run verbatim.** Paste it into `build/release/decidb` and
  confirm before publishing.
- **Boolean decisions render as `0` and `1`.** The readback column type is `int32`. Never
  show `true`/`false`.
- **Do not add EXPLAIN output anywhere.** Layered EXPLAIN rendering is queued work
  (`../descriptions/03_expressivity/explain/todo.md`); anything shown now gets rewritten.
- **Do not paste a DIAGNOSE table for a complicated broken query.** Six queued diagnostics
  items change which repair rows come back for non-trivial cases. The two stable shapes
  are the feasible case and a single loosened literal — both are supplied in §3.
- **The paper is a reference, not an authority.** `../DeciDB_Paper.pdf` is submitted and
  frozen. Use it for framing and for the Figure 1 example. Where it and the code disagree,
  the code wins and the difference simply does not go on the site. (It is already behind
  in one visible way: it announces v0.1.0.)

### How the reference pages should read

**Applies to `documentation.html`, `getting-started.html`, `examples.html` and everything
under `examples/`.** `index.html` is the pitch and is exempt — it may argue, compare, and
sell. Everything else is a guide for someone already using the system: it describes what
the system does and how to use it, and nothing more.

Concretely, when writing any item below:

- **State the rule, not the reason.** "Cast the data or the bound, not the decision" — not
  "because decisions already reach the solver as doubles."
- **No internals.** No Big-M, no indicator variables, no linearization, no "this compiles
  to". A reader writing SQL cannot act on any of it. If a limit exists, name the limit.
- **No defending a restriction.** If something is rejected, say what is rejected and show
  the accepted form. Do not explain why the alternative would be ambiguous or unsound.
- **No comparisons and no claims of superiority.** Not against other systems, not against
  writing solver code by hand, not against an earlier version of DeciDB. That argument is
  made once, on `index.html`, and is not repeated here.
- **Errors are quoted, not narrated.** Where the engine already prints a good message,
  paste it. It is written for exactly this reader.
- **No hedging.** Say what happens. Do not add caveats about future versions or about what
  a different setup might do.

The exception is this file. The reasoning in the items below is for whoever picks up the
work; none of it belongs in the published copy.

### Claims we do not make

- **No join pushdown.** `src/` contains only stock DuckDB pushdown passes. There is no
  DECIDE-specific one.
- **No Progressive Shading or SketchRefine selection.** The paper's own footnote 6 says
  this "is not yet fully implemented in the current DeciDB prototype."
- **No timeout diagnosis.** The paper's footnote 7 calls it future work, and
  `../descriptions/01_pipeline/08_execution/slow_solves_todo.md` still has it open.

---

## 1. Syntax the site teaches that no longer parses

**Do this batch first.** A visitor who copies the documentation page today gets a parser
error on their first query. Nine locations, all in `documentation.html`.

The type in a `DECIDE` declaration is **mandatory** and is written **in parentheses**.
There are exactly three type names: `INT`, `BOOL`, `REAL`. The old `IS` form was removed.

What the binary says today:

```
DECIDE x                 → Parser Error: DECIDE variable "x" needs a type;
                            write x(INT), x(BOOL) or x(REAL)
DECIDE x IS INTEGER      → Parser Error: write the DECIDE type in parentheses,
                            e.g. x(INT); the IS form is no longer accepted
SUCH THAT x IS BOOLEAN   → Parser Error: syntax error at or near "BOOLEAN"
```

### 1.1 Complete Syntax block — `documentation.html:203`

`DECIDE [Table.]variable [IS BOOLEAN | IS INTEGER | IS REAL] [, ...]`
→ `DECIDE [scalar] [Table.]variable(INT | BOOL | REAL) [, ...]`

While here, also show **both clause orders** (§4.1).

### 1.2 DECIDE clause skeleton — `documentation.html:267`

`DECIDE variable_name [IS type] [, variable_name2 [IS type] ...]`
→ `DECIDE variable_name(TYPE) [, variable_name2(TYPE) ...]`

### 1.3 The "default is INTEGER" sentence — `documentation.html:273`

> Specify the variable type with IS BOOLEAN, IS INTEGER, or IS REAL. If you omit the type,
> the default is INTEGER.

Both halves are wrong. Replace with: the type is required and goes in parentheses —
`x(BOOL)`, `x(INT)`, `x(REAL)`. Omitting it is a parser error.

### 1.4 Aggregate Constraints example — `documentation.html:436`

The example opens with `SUCH THAT x IS BOOLEAN AND ...`. Types cannot be declared in
`SUCH THAT` at all. Move it to the DECIDE line:

```sql
DECIDE x(BOOL)
SUCH THAT
    SUM(x * weight) <= 100
    AND SUM(x * volume) <= 50
    AND SUM(x) >= 5
```

### 1.5 Build-incrementally example — `documentation.html:1039`

Same problem, same fix — drop the `x IS BOOLEAN` line and put `DECIDE x(BOOL)` above it.

### 1.6 Prose that names the old spelling — lines 481, 651, 790, 1026

Four sentences say "IS INTEGER / IS BOOLEAN variables", "one factor is IS BOOLEAN", "Use
IS BOOLEAN whenever…". Reword to `INT` / `BOOL` declarations. These are prose, not code,
so nothing breaks — but leaving them keeps teaching the dead spelling.

**Check when done**: `grep -rn "IS BOOLEAN\|IS INTEGER\|IS REAL\|IS type" *.html examples/*.html`
returns nothing. It returns 9 hits today.

---

## 2. Syntax that shipped and the site has never mentioned

All of `documentation.html`. Each of these is verified working. One item each.

### 2.1 The three variable scopes

Today the page covers row-scoped and table-scoped. Query-wide is missing entirely. Add the
table from syntax_reference §2:

| Spelling | Scope | How many solver variables |
|---|---|---|
| `x(INT)` | row-scoped (default) | one per result row |
| `T.x(INT)` | table-scoped | one per distinct row of `T` |
| `scalar x(INT)` | query-wide | exactly one, for the whole query |

A query-wide decision is one number shared by the entire query — useful when the thing you
are deciding is not attached to any row. Verified example (a minimax: spread the shipments
so the busiest route is as light as possible):

```sql
SELECT r.regionID, ship, max_load
FROM routes r
DECIDE ship(INT), scalar max_load(INT)
SUCH THAT SUM(ship) >= 12 AND ship <= max_load
MINIMIZE max_load;
```

Rules worth stating: `scalar` may never be table-qualified; its value repeats on every
output row; and it may stand as the bound of an aggregate (`SUM(ship) <= max_load`).

### 2.2 Signed variables

The page implies decisions are always non-negative. That is a *default*, not a floor. A
variable becomes signed when the query gives it an explicit negative lower bound —
`x >= -K`, `x BETWEEN -K AND K`, or a negative literal in an `IN` domain. There is no
fully-unbounded domain: a signed variable always has a finite lower bound.

### 2.3 Relation-qualified reducers — `SUM(D: expr)`

The biggest missing feature. A join repeats a table's rows once per match, so an ordinary
`SUM` over a table-scoped decision counts that decision once per joined row. A qualified
reducer counts it **once per original row** instead.

Syntax: `agg(Rel[, Rel, ...]: expr)` where `agg` is `SUM`, `AVG`, `MIN` or `MAX`.

Verified example — a depot serving three routes is charged its opening cost once, not
three times:

```sql
SELECT routeID, D.depotID, open, ship
FROM Depots D JOIN Routes T USING (depotID)
DECIDE D.open(BOOL), T.ship(INT)
SUCH THAT ship <= capacity * open AND SUM(ship) >= 100
MINIMIZE SUM(unit_cost * ship) + SUM(D: opening_cost * open)
ORDER BY routeID;
```

Also cover: identity is the row, not the value (two depots with the same cost are still
two terms — this is not `SUM(DISTINCT …)`); only surviving rows count, so a filtered-out
depot contributes nothing; everything inside must come from a named relation (a query-wide
decision is the one exception); `AVG(D: …)` divides by distinct rows, not result rows;
`MIN`/`MAX` are unaffected; and naming several relations widens the identity to their
combination.

### 2.4 `norm(expr, p)`

Regularization terms, for lasso/ridge-style objectives. Verified working in both
objectives and constraints. Four orders:

| Form | Means |
|---|---|
| `norm(e, 1)` | `SUM(ABS(e))` — L1, leans toward sparse answers |
| `norm(e, 2)` | `SUM(POWER(e, 2))` — squared L2 / ridge |
| `norm(e, 'inf')` | `MAX(ABS(e))` — worst single deviation |
| `norm(e, 0[, M])` | exact count of nonzeros |

Verified: `MINIMIZE SUM(cost*x) + 0.5 * norm(x - base, 1)` and
`SUCH THAT norm(x, 0) <= 2`. Mention `SET decide_l0_tolerance` for the L0 nonzero
threshold (default `1e-4`).

### 2.5 Either side may carry the decision, and reducers may be bounds

The page only ever shows `decisions <= bound`. Today `5 >= x`, `10 >= SUM(x)` and
`cap >= SUM(x)` are all accepted, a reducer may appear on both sides
(`SUM(x*v) <= SUM(y*v)`), and a plain data column may be the bound
(`SUM(ship) <= stock PER depotID`). Data-only aggregate bounds work too
(`SUM(x * val) <= SUM(val)`).

### 2.6 A multiplier on a reducer must be one value for the whole query

`2 * SUM(x*p)`, `SUM(x*p) / 2` and `2 * MAX(x*v)` all work. A per-row column does not, and
the error explains why — quote it:

```
'w' varies per row, so it cannot multiply SUM(x).
Move it inside the aggregate, e.g. SUM(x * w).
```

### 2.7 Aggregate-local WHEN

A `WHEN` can attach to a single aggregate inside a larger expression, filtering only that
aggregate: `SUM(x*h) WHEN (shift='morning') + SUM(x*h) WHEN (shift='evening') <= 40`.

State the parenthesisation rule: in a **constraint** the condition must be parenthesised
so the trailing comparison stays the bound. In an **objective** there is no trailing bound,
so a single simple comparison may omit them.

### 2.8 Casts over a decision are rejected

`CAST(x AS INTEGER) <= 3`, `TRY_CAST(...)`, and `SUM(x::DOUBLE)` are all refused inside
`SUCH THAT` and the objective. Cast the *data* or the *bound* instead. Ordinary SQL casts,
including `SELECT CAST(x AS INTEGER)` after the solve, work normally.

State the rule and show the working form. Do not explain what a cast would do to the
model.

---

## 3. A DIAGNOSE section

New top-level section in `documentation.html`. **Write this one last** (see batches) so
the queued diagnostics work has the most time to land.

### 3.1 What it is

`DIAGNOSE` is a prefix on a `SELECT` that carries a `DECIDE` clause. It runs the query and
reports on the run instead of returning rows — the same relationship `EXPLAIN ANALYZE` has
to an ordinary query.

Say that diagnostics run only under the prefix. Do not explain the cost of running them or
why the prefix is opt-in.

### 3.2 The result is a relation

Nine columns: `state`, `clause`, `suggested_change`, `amount`, `total`, `scope`,
`edit_source`, `group`, `row`. Copy the column table and the `edit_source` value table
from syntax_reference §8.1 verbatim — they are the contract.

Because it is a relation it composes:

```sql
SELECT clause, suggested_change
FROM (DIAGNOSE SELECT ... DECIDE ...)
WHERE amount > 1000;
```

### 3.3 The two examples to show

Both verified; do not substitute a more complicated query.

**A query that worked** — one row, everything but `state` is NULL. There is no separate
output path for success:

```
┌──────────┬─────────┬──────────────────┬────────┬───┬─────────────┬─────────┬───────┐
│  state   │ clause  │ suggested_change │ amount │ … │ edit_source │  group  │  row  │
├──────────┼─────────┼──────────────────┼────────┼───┼─────────────┼─────────┼───────┤
│ feasible │ NULL    │ NULL             │   NULL │ … │ NULL        │ NULL    │  NULL │
└──────────┴─────────┴──────────────────┴────────┴───┴─────────────┴─────────┴───────┘
```

**A query that could not be satisfied** — `SUCH THAT SUM(x) <= 3 AND SUM(x) >= 10`:

```
│ infeasible │ SUM(x) <= 3 │ SUM(x) <= 10 │  7.0 │ source_literal       │
│ infeasible │ NULL        │ NULL         │ 10.0 │ achievable_objective │
```

Read as: change `SUM(x) <= 3` to `SUM(x) <= 10` — a move of 7 — and the objective will
reach 10.

### 3.4 What happens without the prefix

The query reports its state and stops. No clause name, no repair. Quote both messages:

```
DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see which clause to change.
DECIDE optimization is unbounded. Prefix the query with DIAGNOSE to see which decision needs a bound.
```

### 3.5 Restrictions

- The inner query must contain a `DECIDE` clause.
- **Exactly one.** `DIAGNOSE` cannot be attached to nested DECIDE subqueries, and a query
  containing several DECIDE operators is not supported.
- No options — there is no `DIAGNOSE (VERBOSE) …`.
- A query that fails *before* it can be solved (syntax error, semantic error, a model the
  host's solver refuses) still raises. `DIAGNOSE` explains the outcome of a solve.

### 3.6 The settings

Five, all verified present via `duckdb_settings()`. These tune the engine once `DIAGNOSE`
has started it; none of them starts or suppresses it.

| Setting | Default | What it does |
|---|---|---|
| `diagnose_decide_infeasible_slack_scope` | `query` | one edit per SQL-level knob; `expanded` gives one per generated row/group |
| `diagnose_decide_escape_rate` | `0.8` | report a category when this share of its rows run away |
| `diagnose_decide_categorical_ratio` | `0.1` | treat a column as a category when distinct values ≤ ratio × rows |
| `diagnose_decide_min_categories` | `20` | floor on that cap, so small tables still qualify |
| `diagnose_decide_removal_bigm` | `0.0` | advanced override when diagnosing a dropped `<>` (0 = automatic) |

Describe each setting by what it changes in the output, in one line. The last one has no
plain-SQL meaning — either give it the one line above or leave it out of the page
entirely.

---

## 4. Corrections to documentation content that already exists

### 4.1 Show both clause orders

`documentation.html` shows only the single-block order (DECIDE after `WHERE`). The split
order — DECIDE between `SELECT` and `FROM`, `SUCH THAT` after the joins — is equally valid
and is the one the paper uses. Both parse to the same plan. Show both; note the
declaration may appear in one position or the other, never both.

### 4.2 Solver Outcomes is missing a case

The section lists three outcomes (Optimal, Infeasible, Unbounded). A fourth exists: the
solve can hit its time limit or be interrupted with Ctrl-C. Describe what the user sees —
a report of what was found so far and how much better it could still get, an offer to keep
going at a terminal, an error in scripts and pipes, and after Ctrl-C the best answer found
so far, marked as not proven best.

Also point the Infeasible and Unbounded entries at `DIAGNOSE` (§3), since that is now the
actual next step.

### 4.3 Table-scoped variables: state the entity key

Add the rule from syntax_reference §2.1: the entity key is **all columns** of the source
table, and there is no syntax to pick a subset.

### 4.4 PER columns may be table-qualified

`advanced-scheduling.html`'s breakdown says "PER takes unqualified column names". That
reads as a restriction and it is not one. `PER e.empID` and `PER employees.empID` are both
accepted and mean the same thing as `PER empID`; the qualifier is only there to
disambiguate in joins. Fix the sentence there and state the rule in `documentation.html`.

### 4.5 Check the COUNT rejection wording

The Objective Requirements list says "COUNT over decision variables is rejected at bind
time." Run it and quote the current message, or drop the claim.

---

## 5. Example pages

I re-ran all eleven pages against the binary on 2026-08-27. Values match on ten. The table
below is the verified status — do not redo this work, just fix the ❌ cells.

| Page | Values | Row order | Prose |
|---|---|---|---|
| basic-knapsack | ✅ | ✅ | ✅ |
| basic-production | ✅ | ✅ | ✅ |
| basic-filtering | ✅ | ✅ but unsorted | ✅ |
| basic-feasibility | valid, but not today's answer | ✅ | ❌ |
| intermediate-diet | ✅ | ✅ | ✅ |
| intermediate-assignment | ✅ | ✅ | ✅ |
| intermediate-dynamic | ✅ | ✅ | ✅ |
| intermediate-per | ✅ | ✅ | ❌ |
| advanced-portfolio | ✅ | ✅ | ✅ |
| advanced-scheduling | ✅ | ❌ | ✅ |
| advanced-table-scoped | ✅ | ✅ | ✅ |

### 5.1 The variability note

Write one short sentence and reuse it verbatim on every page where more than one answer is
equally good:

> This problem has more than one equally good answer. DeciDB returns one of them.

That is the whole note. Do not add caveats about solvers or future versions.

Apply to `basic-feasibility` and `intermediate-dynamic` at minimum.

### 5.2 `advanced-scheduling.html` — the row order is wrong

The page shows Alice1, Alice2, Alice3, Bob1, Bob2, Bob3, Carol4. The join actually returns
Alice3, Bob3, Carol4, Alice2, Bob2, Alice1, Bob1. The assignment values are all correct.

Add `ORDER BY r.name, s.slot_id` to the query and re-capture the table.

### 5.3 `basic-filtering.html` — pin the order

The displayed order (1, 2, 3, 6, 5) matches today but only by accident — it is the filter's
output order, not anything guaranteed. Add `ORDER BY id` and re-capture.

### 5.4 `basic-feasibility.html` — the prose claims too much

> Every shift has exactly one nurse (≥1 PER shift)

`SUM(assigned) >= 1 PER shift_id` says *at least* one, not exactly one. Today's run proves
it: Gurobi puts both Alice and Bob on Night. Under HiGHS the answer differs again (Bob
takes Morning, Carol is unassigned) — both are valid.

Fix: reword to "at least one nurse per shift", describe the displayed table as one valid
schedule rather than *the* schedule, and add the §5.1 note. The table itself is a legal
answer and can stay.

Add one sentence showing `= 1 PER shift_id` as the way to require exactly one. State it as
an option, not as a correction of the example.

### 5.5 `intermediate-per.html` — the result table has a column the query doesn't return

The query selects `name, department, performance_score, promoted`. The result table has an
`id` column in front (`intermediate-per.html:155`). Either drop that column from the table
or add `id` to the SELECT list — and if you add it, re-run to confirm.

### 5.6 New page: the paper's Figure 1

Add one example page for relation-qualified reducers, using a disaster-relief allocation
scenario. The query and data below come from the paper's Figure 1 and are verified to run
as written.

The page presents this as an ordinary example. It does not mention the paper, and it does
not note that the paper's own listing differs — write the working query and its real
output, nothing more. Two differences to be aware of while transcribing:

- `using depotID` → `USING (depotID)`.
- Show `open` as `1`/`0`, per the ground rules.

```sql
SELECT routeID, depotID, regionID, open, ship
FROM Depots D JOIN Routes T USING (depotID) JOIN Regions R USING (regionID)
DECIDE D.open(BOOL), T.ship(INT)
SUCH THAT ship BETWEEN 0 AND capacity * open
  AND SUM(ship) <= stock PER depotID
  AND SUM(ship) >= demand WHEN priority = 'critical' PER regionID
MINIMIZE SUM(unit_cost * ship) + SUM(D: opening_cost * open)
ORDER BY routeID;
```

Sample data (Depots D1/D2, Regions R1/R2, Routes T1/T2/T3) is in the paper's Figure 1.
Verified output: T1 ships 450 from an open D1; T2 ships 0; T3 ships 0 and D2 stays shut.

This single query shows table-scoped decisions, a decision inside a `BETWEEN` bound,
`PER`, `WHEN … PER`, and the qualified reducer. Add it to `examples.html` and wire it into
the prev/next chain.

### 5.7 Sweep

Add `ORDER BY` to any remaining page whose displayed order is incidental rather than
requested, and re-check each table after.

---

## 6. Version, packaging, and the about page

### 6.1 Version number — `about.html:377`

`0.1.0 (Prototype)` → `0.2.0`. The binary reports `v0.2.0`.

### 6.2 The Python package is live — `getting-started.html:245` and `467–478`

Two places say "coming soon". Replace the Coming Soon block with real instructions:

```bash
pip install decidb
```

```python
import decidb
decidb.connect().sql("SELECT 42").fetchall()
```

Requires Python 3.11+. The API mirrors `duckdb`'s.

**Verify first**: install the published package into a clean virtualenv and run a real
DECIDE query through it before this goes live. Do not publish install instructions that
have not been run.

### 6.3 Future Development list — `about.html:396+`

- Remove "Python Bindings" — it ships now (§6.2).
- Keep SOCP as future. It is unimplemented and low priority
  (`../descriptions/03_expressivity/problem_types/todo.md`), and HiGHS has no path to it.
- Keep Sketch-Refine as future, consistent with the guardrail above.

### 6.4 Verify or soften the Current Status figures

"Test Coverage: automated test suite" and "Target Scale: ~1M rows" are both unsourced.
Either back them with something in the repo or soften the wording.

### 6.5 Research Foundation citation

Confirm the DeciDB paper entry cites it as CIDR'27 with the full author list.

### 6.6 Decision needed — may we use the conciseness numbers?

The old version of this file banned "the paper's unverified conciseness ratios". That was
too blunt. The paper does measure them: mean DeciQL-to-baseline size ratio in words of
**0.71** (95% CI [0.62, 0.80]) against DeQL over 12 examples, and **0.60** ([0.49, 0.72])
against SolveDB over 9 — shorter in all 21 cases.

These are a language-design result, not a system-performance claim, so no amount of code
change can contradict them.

**Recommendation**: allow them on `about.html` only, and only attributed — "in the paper's
study of 21 example queries, DeciQL was shorter in every case" — never as a bare number on
`index.html`. **Ask before writing it in.**

---

## 7. Completion gate

Before publishing:

- [ ] Every displayed query runs verbatim on a clean `make release` build.
- [ ] Every displayed table is a valid answer for its query, and pages with several equally
      good answers carry the §5.1 note.
- [ ] `grep -rn "IS BOOLEAN\|IS INTEGER\|IS REAL" *.html examples/*.html` returns nothing.
- [ ] No EXPLAIN output anywhere on the site.
- [ ] Read `documentation.html`, `getting-started.html`, `examples.html` and every example
      page for voice: no justifying a restriction, no solver internals, no comparisons.
      These pages describe; they do not argue. (`index.html` is exempt — settled
      decision 6.)
- [ ] All site syntax agrees with the frozen syntax reference.
- [ ] The release-candidate gate in [`../descriptions/todo.md`](../descriptions/todo.md) §4
      is green.
- [ ] Publication follows
      [`../descriptions/02_operations/release_workflow.md`](../descriptions/02_operations/release_workflow.md).

---

## Suggested batches

| Batch | Contents | Why grouped |
|---|---|---|
| **W1 — Unbreak** | §1 (all nine locations) + §6.1 | Small, no open decisions, and it stops the site teaching queries that error. Do first. |
| **W2 — Examples** | §5 | Mechanical and independently verifiable. Does not depend on W1. |
| **W3 — New syntax** | §2 | The bulk of the writing. |
| **W4 — DIAGNOSE** | §3 | Last, so the queued diagnostics work has the most time to land. |
| **W5 — Gate** | §4 + §6.2–6.6 + §7 | Corrections and the pre-publication sweep. |
