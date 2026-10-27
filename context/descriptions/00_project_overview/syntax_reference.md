# Syntax Reference

> **Code-grounded syntax reference — reviewed 2026-10-18.**
>
> The current implementation is the source of truth. Every claim here should describe
> behavior exercised by the current source and tests. Where this file or
> `context/DeciDB_Paper.pdf` disagrees with the implementation, update the documentation;
> if the implementation itself is suspect, record that separately as an open code issue.
> The paper's Figure 1 uses spellings that are removed (§10.3).
>
> Feature docs under `03_expressivity/` deliberately do not restate all syntax — they
> hold semantics and code pointers — so this remains the single documentation entry
> point for syntax questions.

## 1. The DECIDE clause

Two clause orders are accepted; they parse to the same plan and neither is
preferred by the engine.

**Split order** (the paper's, Figure 1) — the declaration sits between `SELECT`
and `FROM`, the constraints and objective come after the joins:

```sql
SELECT ...
DECIDE [per key:] name(type) [, [per key:] name(type) ...]
FROM ...
[JOIN ...]
[WHERE ...]
SUCH THAT
    [scope:] constraint
    [AND [scope:] constraint ...]
[MAXIMIZE | MINIMIZE [scope:] objective]
```

**Single-block order** — the whole clause sits after `WHERE`:

```sql
SELECT ...
FROM ...
[WHERE ...]
DECIDE [per key:] name(type) [, [per key:] name(type) ...]
SUCH THAT
    [scope:] constraint
    [AND [scope:] constraint ...]
[MAXIMIZE | MINIMIZE [scope:] objective]
```

A `scope` is `[when condition] [per key]` (§1.1); on the objective its `per` can only be
`()`. The declaration may appear in one position or the other, never both. A
declaration with no `SUCH THAT` (*"DECIDE requires a SUCH THAT clause; ..."*), or a
`SUCH THAT` with no declaration (*"SUCH THAT requires a DECIDE clause; ..."*), is a
parser error.

### 1.1 Scopes: per, when, by

Three keywords, three separate jobs:

| Keyword | Position | Job |
|---|---|---|
| `per K` | in front of a declarator, a constraint or a reducer's argument | how many decisions, constraints or reducer inputs are created: one per class of the key `K` |
| `when c` | in front of a constraint, the objective or a reducer's argument | which rows are looked at: those where `c` is true |
| `by (Γ)` | after a reducer | which rows that one reducer reads: those sharing the current value of `Γ` |

`per` never selects the rows a reducer reads, and `by` never changes how many
constraints exist (§5.4).

Terms used below:

- **Reducer**: `SUM`, `AVG`, `MIN` or `MAX` over rows.
- **Tuple term**: any part of a clause that is not inside a reducer — a column, a
  decision, arithmetic over them.
- **Key / class**: a key lists columns and relations (for `by`, also expressions);
  rows with equal key values form one class.
- **Kept rows**: the query's result rows (after joins and `WHERE`) that pass the
  enclosing `when`.
- **Determines**: key `K` determines a value when every row of each `K`-class carries
  the same value (§7.1).

**Grammar**:

```
decide_list ::= declarator (',' declarator)*
declarator  ::= ['per' key ':'] name '(' INT | BOOL | REAL ')'

constraint  ::= [scope ':'] comparison       -- AND separates constraints; a scope covers one
objective   ::= (MAXIMIZE | MINIMIZE) [scope ':'] expression     -- per must be '()' or absent
scope       ::= ['when' condition] ['per' key]                   -- at least one of the two

reducer     ::= agg '(' [scope ':'] expression ')' ['by' '(' [expression, ...] ')']
              | agg '(' relation, ... ':' expression ')' ['by' ...]   -- the colon form, §5.1
agg         ::= SUM | AVG | MIN | MAX         -- norm(e, p) and count(*) take a by, no scope

key         ::= '(' ')'  |  'row'  |  element (',' element)*
element     ::= column | relation             -- a relation stands for all its stored columns
```

| Omitted | Means |
|---|---|
| `per` on a declarator, on a constraint, or inside a reducer | `per row` |
| `per` on the objective | `per ()` |
| `by` | `by ()`: all kept rows |
| `when` | every row |

`per row` names no key; written out, it means exactly the default.

Tests: `test/decidb/test_scope_parser.test` (the grammar).

### 1.2 Words that are keywords inside the clause

Inside a DECIDE clause — the declaration list, and everything from `SUCH THAT` to
the end of the objective, subqueries written there included — `when`, `per`, a `by`
directly after `)` and a `row` directly after `per` are keywords, never names. A
column with such a name is written quoted there: `x <= "per"`. Outside the clause,
including the `SELECT` list and `FROM` of a DECIDE query, nothing changes, and
`GROUP BY` / `ORDER BY` / `PARTITION BY` in a subquery written inside the clause are
untouched. `scalar` is an ordinary name: `DECIDE scalar(INT)` declares a decision
called `scalar`.

A scope inside an ordinary SQL subquery written in the clause is refused
(*"DECIDE: 'WHEN condition:' is not supported yet"*).

## 2. Decision variables

- Must be declared in the `DECIDE` list, and the type is **mandatory**: it is
  written in parentheses after the name, `x(INT)`. There are exactly three type
  names — `INT`, `BOOL`, `REAL`. A name without one is a parser error that names the
  three (§10.1).
- Use: available in `SUCH THAT`, `MAXIMIZE/MINIMIZE`, and the `SELECT` list, always
  by its bare name (§2.3).
- A declaration's **key** decides how many solver columns it yields:

  | Spelling | Scope | Columns |
  |---|---|---|
  | `x(INT)`, `per row: x(INT)` | per row (default) | one per result row (§2.1) |
  | `per K: x(INT)` | keyed | one per class of the key `K` among the result rows (§2.3) |
  | `per (): x(INT)` | query-wide | exactly one, for the whole query (§2.2) |
- **Type Declarations** (in DECIDE clause):
  - `x(INT)`: $x \in \{0, 1, 2, ...\}$ by default
  - `x(BOOL)`: $x \in \{0, 1\}$ (automatically adds bounds constraints)
  - `x(REAL)`: $x \in [0, \infty)$ by default (continuous)
- **Result column types**: `INT` returns `BIGINT`, `BOOL` returns `INTEGER`
  (`0`/`1`), `REAL` returns `DOUBLE`. `INT` is 64-bit because nothing holds a
  decision to a narrower range — the bound that pins a variable may be a column,
  or an aggregate row that bounds no single variable — so a 32-bit column would
  truncate a legitimate optimum. A whole-number answer beyond a double's exact
  range ($2^{53}$) is refused by name rather than rounded.
- **Default lower bound is 0** for `INT` and `REAL`. This is a
  *default*, not a floor: a variable becomes **signed** (may take negative
  values) when the query bounds it alone below zero. The bound is read by value, not
  by spelling: `x >= -K` in any affine spelling whose constants are known before
  the query runs (`x - 1 >= -3`, `2 * x >= -4`, `-x <= 2`, `x / 2 >= -1`,
  `2 * (x - 1) >= -6`),
  `x BETWEEN -K AND K`, an easy `min(x) >= -K` in the same spellings (no `when` and
  no filter of its own, whatever its `per` or `by`, §5), or a negative literal in an
  `IN` domain (`x IN (-5, 0, 5)`). A bound whose other side is data (`x >= w - 4`, a
  subquery) is a row, not a floor. A variable the query never lowers stays
  non-negative, and so does one lowered only under a `when`
  (`when g = 'A': x >= -5` keeps `x >= 0`; an open issue in
  `06_issues/bugs/todo.md`). There is no fully-free ($-\infty$) domain: signed
  variables always have a finite lower bound. See `03_expressivity/decide/done.md` →
  "Signed variables".

**Examples:**

```sql
DECIDE x(BOOL)           -- x is binary (0 or 1)
DECIDE x(INT)           -- x is integer, default domain {0, 1, 2, ...}
DECIDE x(REAL)              -- x is continuous, default domain [0, +inf)
DECIDE x(BOOL), y(INT), z(REAL)  -- multiple typed variables

-- Signed (negative-domain) variables: opt in with an explicit negative bound
DECIDE adj(REAL)            -- ... SUCH THAT adj BETWEEN -10 AND 10  → adj in [-10, 10]
DECIDE d(INT)           -- ... SUCH THAT d >= -5                  → d in [-5, +inf)
```

### 2.1 One decision per row (default)

A declarator without `per` — or with `per row:`, the same thing written out — makes one
decision per **result row**: the rows the joins and `WHERE` produce. When a query
joins several tables, each joined row gets its own independent decision.

**Reducers count result rows**, whatever the decision's key. If a nurse appears in 5
result rows (one per shift), an unscoped `sum` reads her decision 5 times. This
matches SQL over the join result; to count each class once, give the reducer its own
`per` (§5.3) or use the colon form (§5.1).

```sql
-- One decision per nurse, though each nurse appears once per shift.
SELECT n.name, s.shift_date, keepN
FROM nurses n
JOIN shifts s ON n.id = s.nurse_id
DECIDE per n: keepN(BOOL)
SUCH THAT
    sum(keepN * s.hours) <= 100
MAXIMIZE sum(per n: keepN * n.skill_score)
```

`sum(keepN * s.hours)` adds each kept shift's hours, one term per result row;
`sum(per n: keepN * n.skill_score)` adds each nurse's score once, where
`sum(keepN * n.skill_score)` would add it once per shift. Without `per n:` on the
declaration, each of a nurse's rows would get its own `keepN`.

### 2.2 Query-wide — `per (): x(TYPE)`

Some decisions are not attached to a tuple. `DECIDE per (): name(TYPE)` declares
**one** decision shared by the whole query, regardless of input cardinality.

```sql
-- The paper's Example 1: no region falls short of its demand by more than max_shortfall.
SELECT r.routeID, r.regionID, ship, max_shortfall
FROM routes r
DECIDE ship(INT), per (): max_shortfall(INT)
SUCH THAT
    ship <= r.capacity AND
    sum(ship) <= 40 AND
    per r.regionID: r.demand - sum(ship) by (r.regionID) <= max_shortfall
MINIMIZE max_shortfall
```

- **Output.** The assigned value is repeated on every output row, as a keyed
  decision's value repeats on every row of its class.
- **In constraints.** A query-wide decision may be compared against row-varying
  data (`ship <= max_shortfall`); the constraint is generated per row, and every
  generated row references the same solver column. That is what makes the
  decision shared rather than per-row. It is determined by every key (§7.1), so it
  may stand beside a reducer or be its bound under any `per`, and it counts once
  (§5).
- **A reducer over query-wide decisions only** — `sum(cap)`, `avg(cap)`,
  `sum(cap1 + cap2)` — has one column and nothing to reduce over, so either plausible
  reading (coefficient `1` or coefficient `n`) is a different optimization problem.
  As a constraint's whole decision side (`sum(cap) <= 5`, `SUM(D: cap) <= 5`) it is
  refused (*"cap is a query-wide decision, so SUM(cap) has nothing to aggregate over;
  use cap on its own"*); write the bare name. A body that mixes a query-wide decision
  with row-varying data is a standard scalar-times-vector reduction:
  `sum(cost * cap)` means `(sum cost) * cap`, and `sum(x + cap)` means
  `sum(x) + n*cap`, where `n` is the number of values the reducer reads (its `by`
  group's kept rows after its own `when`, or the classes of its own `per`, §5.3).
- **In objectives.** A query-wide decision may appear bare, either alone
  (`MINIMIZE max_shortfall`) or as an additive term beside reducers
  (`MINIMIZE max_shortfall - sum(ship)`) — its coefficient is applied **once**,
  not once per row — or inside a reducer whose body is not row-invariant
  (`MINIMIZE sum(cost * cap)`), where it is weighted like any other term.

Tests: `test/decide/tests/test_scalar_scope.py`.

### 2.3 Keyed — `per K: x(TYPE)`

```sql
DECIDE ship(INT)                                 -- one per result row
DECIDE per D.depotID: reserve(REAL)              -- one per distinct depotID
DECIDE per D: open(BOOL)                         -- one per distinct tuple of D
DECIDE per D.region, S.shipmentID: alloc(INT)    -- one per distinct pair, across two relations
DECIDE per (): cap(INT)                          -- one for the whole query
DECIDE ship(INT), per D: open(BOOL), per (): cap(INT)   -- each declarator its own key
```

- **Key elements** are columns and relations of the `FROM` clause, in any mix,
  spelled as anywhere else in the query: `region`, `D.region`, `main.D.region`;
  `D`, `main.D`. An unqualified `USING` column is the kept side's, as in SQL. A
  relation stands for all its stored columns: `per D` keys on every column of `D`,
  and `per D.depotID` on one. A generated column is left out, since it only repeats
  the columns it is computed from. The key is exactly what is written. Expressions,
  decisions, `rowid` and generated columns are not key elements (§10.1); expressions
  belong in `by` (§5.2).
- **A key is a set**: order and repetition do not matter (`per a, b` and `per b, a`
  are one key), and declarators with equal keys share one grouping.
- **Each declarator has its own key**: a `per` applies only to the name right after
  it. `per D: open(BOOL), ship(INT)` leaves `ship` one per row; to give two names the
  same key, repeat it.
- **Count**: one decision per distinct key value among the result rows; a key value
  on no result row has no decision. Rows with a NULL key share one decision (D5).
- **Output**: every result row shows its class's value, so the value repeats on all
  rows of a class.
- **Reference** by the bare name everywhere, the select list and `ORDER BY` included
  (§10.3).

```sql
-- result rows:  (D1, S1)  (D1, S2)  (D2, S3)
DECIDE per D: open(BOOL), ship(INT)
--   open -> 2 decisions (D1, D2)
--   ship -> 3 decisions (one per row): the key of open does not carry over

DECIDE per D: open(BOOL), per D: closed(BOOL)
--   open, closed -> 2 decisions each
```

Tests: `test/decide/tests/test_scope_declare.py`.

## 3. Constraints

Constraints must evaluate to a boolean. Multiple constraints are separated by `AND`; a comma between them is a parser error (*"comma-separated SUCH THAT constraints are not supported; use AND between constraints"*). An `OR` between constraints (`x <= cap OR x <= 1`) is refused: every constraint must hold, and a choice between alternatives is modelled with a `BOOL` decision. An `OR` inside a `when` condition or a subquery is data and unaffected.

- **Supported Operators**: `=`, `<`, `<=`, `>`, `>=`, `<>`.
  - `<>` (not-equal): Supported on both per-row and aggregate constraints. Rewritten to `LHS <= K-1 OR LHS >= K+1`, which (like strict `<` / `>`) is only valid when the LHS is integer-valued. Rejected at **bind** time with a `BinderException` whenever the compared side is not provably whole-numbered from its declared types — a REAL decision, or a column/literal of a fractional type (`DOUBLE`, `DECIMAL` with scale > 0). The message names the offender and the cast that fixes it. For `AVG(x) <> K` the denominator is hoisted to the RHS (emitted as `SUM(x) <> K*n`, `n` the size of each `by` group), keeping the LHS integer-valued.
    - Encoding depends on where `K` falls in the LHS's reachable range. When `K` is **interior**, the disjunction costs 1 auxiliary binary + 2 Big-M constraints. When the range lies wholly on one side of `K`, one branch is dead and a **single plain inequality** is emitted instead — `SUM(x) <> 0` over non-negative decisions is just `SUM(x) >= 1`, with no binary and no Big-M. When `K` is unreachable the constraint excludes nothing and is dropped. Only a variable's intrinsic domain (BOOL 0/1, default non-negativity) licenses the collapse; a bound you write yourself stays loosenable under `DIAGNOSE` and keeps the disjunction.
  - An L0 count is integer-valued whatever it counts, so `norm(e, 0, M) < K` and `norm(e, 0, M) <> K` are accepted even when `e` references a REAL decision. Other norm orders (`1`, `2`, `'inf'`) are not counts and are not exempt.
  - `<` / `>` (strict): Rewritten internally to the integer-step form (`LHS < K` $\rightarrow$ `LHS <= ceil(K) - 1`), which is only valid when the compared side is provably whole-numbered. Use `<=` / `>=` instead when it is not.
    - **The rule is stated on declared types, and rejects at bind time**, before any data is read. A side is whole-numbered when every decision in it is `INT`/`BOOL` and every column, literal and reducer over them has a whole-numbered type: an integer type, or `DECIMAL` with scale 0. `DOUBLE`, `FLOAT` and `DECIMAL(p, s>0)` are not, *whatever values they happen to hold* — otherwise inserting one fractional row could make a working query illegal. The message names the offender and the cast that fixes it (`SUM(x * l_quantity::BIGINT) < 100`).
    - **Not refused**: the bound `K` itself (a fractional bound is handled by rounding, or for `<>` by dropping a tautology); a decision-free additive offset, which moves the bound rather than the lattice; `POWER(e, n)` with a whole, non-negative constant `n`; and an L0 count, which is integral whatever it counts. A fractional *multiplier* (`SUM(0.5 * x) < 5`) is refused, since it rescales the decision off the lattice, and so is an offset under anything not linear in it — a power, a product of decisions, a norm of order 1, 2 or `'inf'`, the `MIN`/`MAX` of §5.2 — which is part of the value there (`POWER(x + 0.25, 2) < 11`, `(x + 0.5) * y < 5`, `norm(x + 0.5, 'inf') > 3`).
    - **A NULL beside a decision** makes the expression NULL whatever the decisions are (`SUM(x + NULL) >= 3`); it is refused where it is written (*"DECIDE: '(x + NULL)' is NULL whatever the decisions are; replace the NULL with a value, as in COALESCE(column, 0)"*).
- **Between**: `expr BETWEEN a AND b` $\rightarrow$ `expr >= a AND expr <= b`.
- **In**: `x IN (v1, ..., vK)` — works on both table columns and decision variables. On decision variables, rewritten to K binary indicator variables with cardinality + linking constraints. `IN` on aggregates (e.g., `SUM(x) IN (...)`) is not supported.
- **Linearity**: Most sub-expressions involving a decision variable must be linear.
  - `x * 5`: OK.
  - `x + y`: OK.
  - `x * column`: OK (column is constant per row).
  - `x * y`: OK — bilinear (Gurobi only for non-Boolean pairs; McCormick for Boolean×anything).
  - `POWER(x - target, 2)`: OK — quadratic constraint (Gurobi only, via `GRBaddqconstr`).
  - `x * x * x`: **ERROR** (triple+ products not supported).
- **Quadratic constraints**: `POWER(linear_expr, 2)` / `expr ** 2` / `(expr)*(expr)` in constraints enables QCQP. Gurobi only. Composes with `when`, `per` and `by`. See §3.1.
- **Subqueries**: Scalar subqueries (both uncorrelated and correlated) are allowed on the RHS of constraints. Correlated subqueries are decorrelated into joins, producing per-row values. A subquery bound that varies by row follows the bound rule of §5, like a column. Subqueries cannot reference DECIDE variables from the enclosing query.
- **Nested DECIDE**: A scalar RHS subquery may contain its own `DECIDE` clause. The inner decision query solves first and its one-row result becomes the outer bound. After the subquery, `when`, `per` and `by` are keywords again in the enclosing clause. `DIAGNOSE` has a separate exactly-one-DECIDE restriction (§8.3).
- **NULL**: A NULL in any value the solver reads is an error, never a zero — coefficients, bounds, and reducer inputs alike. The message names the offending column and asks for explicit `COALESCE()` imputation or a `WHERE` filter. A data-only imputation may be written inline anywhere the value is used, including inside a reducer (`SUM(x * COALESCE(weight, 0))`). `COUNT(col)` is the exception: it skips a NULL and counts a NaN, as SQL's `COUNT` does. Rationale in [`../03_expressivity/such_that/done.md`](../03_expressivity/such_that/done.md). This is distinct from a `when` condition, where NULL is false and drops the row, and from a `per` or `by` key, where NULL is a class (§7.2).

### 3.1 Quadratic constraints (QCQP)

```sql
-- Per-row quadratic constraint
SUCH THAT POWER(x - target, 2) <= 9

-- Aggregate quadratic constraint (total budget)
SUCH THAT SUM(POWER(x - target, 2)) <= 1000

-- One per department, over that department's rows
SUCH THAT per department: SUM(POWER(x - target, 2)) by (department) <= 50

-- Multiple syntax forms (all equivalent)
SUCH THAT POWER(x - t, 2) <= K
SUCH THAT (x - t) ** 2 <= K
SUCH THAT (x - t) * (x - t) <= K
```

**Gurobi only** — HiGHS does not support quadratic constraints. Negated and scaled forms are supported: `-POWER(expr, 2)`, `K * POWER(expr, 2)`.

### 3.2 Numeric precision and casts

A DECIDE model carries **one numeric domain: `DOUBLE`**, roughly 15 significant
digits. This is not a policy choice — the solver interface is `double` throughout
(`SolverInput` holds `vector<double>` for every bound and coefficient) and both
Gurobi and HiGHS take doubles, so every value in a model ends up there whatever
types the SQL expression carried.

Three consequences are explicit:

**Do not cast decision expressions in solver clauses.** `CAST`, `TRY_CAST`, and `::`
are rejected when their input contains a decision variable in `SUCH THAT` or the
objective. This is independent of the target type:

```sql
SUCH THAT CAST(x AS INTEGER) <= 3     -- rejected
SUCH THAT TRY_CAST(x AS DOUBLE) <= 3  -- rejected
MAXIMIZE SUM(x::DOUBLE)               -- rejected
```

Decision values already enter the solver as DOUBLE, so a cast to DOUBLE is redundant.
Other targets introduce SQL computations such as rounding that are not supported
solver algebra. Compare the decision directly and cast the data or bound instead.

**Data casts retain DuckDB semantics.** `x <= CAST(1.6 AS INTEGER)` has a bound of 2,
and `SUM(x * CAST(weight AS INTEGER))` uses the cast result as its coefficient.
DuckDB evaluates the complete typed data expression first; DeciDB converts only its
result to DOUBLE when building the solver model.

**Internal casts are transparent.** DuckDB may insert casts to reconcile SQL types,
for example when comparing `SUM(x)` with a DECIMAL column. Those binder-generated
wrappers over decision algebra are implementation detail and do not change the model.

This restriction does not affect ordinary SQL. A cast in a standard query or in the
post-solve `SELECT` list, including `SELECT CAST(x AS INTEGER)`, behaves normally.

**Beyond the domain.** Above `2^53` (about 9 quadrillion) a DECIDE model and
row-by-row SQL evaluation can disagree, because SQL rounds each side of a written
comparison independently while DeciDB folds the bound once. Such a query is outside
what the solver could represent in any case. This limit is pinned by
`test_canonicalize_cast.py::test_magnitudes_beyond_double_domain_are_a_documented_limit`.

## 4. Objective

**Syntax**: `MAXIMIZE|MINIMIZE [when c] [per ()]: E`

```sql
MAXIMIZE sum(ship)                         -- produced once: per () implied
MAXIMIZE per (): sum(ship)                 -- the same, written out
MAXIMIZE when S.priority: sum(ship)        -- over the priority rows only
MINIMIZE sum(S.unit_cost * ship) + sum(per D: D.opening_cost * open)
MINIMIZE max(per D.depotID: sum(ship) by (D.depotID))   -- two-level form
```

- **Optional**: Omitting `MAXIMIZE`/`MINIMIZE` creates a feasibility problem — the
  solver finds any assignment satisfying all constraints. Both Gurobi and HiGHS
  support this.
- **Produced once.** No `per` means `per ()`. Any other `per`, `per row` included, is
  the parser error *"an objective is produced once: write per (): or leave per out"*.
- **`when c`** in front filters the rows for the whole objective; one that keeps no
  row is an error (§7.3). Inside reducers, `when` and `per` work as in §5.3.
- **Body**: reducers (`SUM`, `AVG`, `MIN`, `MAX`, `norm`), decisions outside them, and
  sums of those. It must involve at least one decision.
  - A decision outside the reducers is legal when it has one instance over the kept
    rows, and counts once: a `per ()` decision always (`MINIMIZE max_shortfall -
    sum(ship)`), a per-row or keyed one when the `when` keeps one row or class
    (`MAXIMIZE when id = 3: x`); otherwise it is the determination error (*"DECIDE:
    per () does not determine the decision x (declared per row). The kept rows cover
    more than one x."*). Not beside a `MIN` or `MAX` (§10.2). A data value beside it
    must be one value over the kept rows.
  - A product of two query-wide decisions counts once too (`MAXIMIZE SUM(x) + s * u`,
    needs Gurobi); any other product of two decisions outside a reducer is refused
    (*"... multiplies decisions outside a reducer, which is not supported yet"*)
    unless one is `BOOL`: that product is linearized first, and counts once.
  - A factor on a reducer must be one value over the kept rows (§5).
- **`by` on a top-level reducer** must be one group over the kept rows: `by ()`, or a
  `Γ` with a single value there (`MAXIMIZE when g = 'A': sum(x) by (g)`); otherwise
  *"DECIDE: per () does not determine g (in by (g)). ..."*.
- **Two-level form** `outer(per k: inner(e) by (k))`, the whole objective: `inner` is
  evaluated once per class of `k` (NULL a class), then `outer` over those values. All
  nine combinations of `SUM`, `MIN` and `MAX` are accepted, and `k` may list several
  columns. It takes a `when` in front of the objective, not inside either reducer; its
  inner reducer takes no `per`, and its `by` key must be its `per` key (§10.2).
- **Linear objectives**: must be linear in decision variables.
- **Quadratic objectives (QP)**: `MINIMIZE SUM(POWER(linear_expr, 2))` is supported for convex quadratic programming. The inner expression must be linear in decision variables. Three equivalent syntax forms:
  - `POWER(expr, 2)` / `POW(expr, 2)` — function call
  - `expr ** 2` — exponentiation operator
  - `(expr) * (expr)` — identical multiplication (both sides must be the same expression)
  - Negated forms: `-POWER(expr, 2)`, `(-1) * POWER(expr, 2)` for concave QP (both solvers).
  - `MAXIMIZE SUM(POWER(expr, 2))` is non-convex (Gurobi only, via NonConvex=2).
  - Gurobi supports continuous QP, mixed-integer QP (MIQP), and coupled or
    rank-deficient quadratic groups. HiGHS supports continuous QP only when the
    quadratic matrix is non-singular; integer/boolean variables or a square that
    couples several decision variables require Gurobi.
- **Bilinear objectives and constraints (`x * y`)**: Products of two different DECIDE variables are supported in both objectives and constraints. Two categories:
  - **Boolean x anything** (McCormick linearization): When one factor is `BOOL`, the product is exactly linearized. Works with both Gurobi and HiGHS. Requires a finite upper bound on the non-Boolean variable — given explicitly (`x <= K`) or inferred by implied-bound propagation from a non-negative constraint like `SUM(x) <= K`. Bool x Bool uses simpler AND-linearization (no Big-M).
  - **General non-convex** (`Real*Real`, `Int*Int`, `Int*Real`): Produces indefinite Q matrix. Objectives: Gurobi only (NonConvex=2). Constraints: Gurobi only (via quadratic constraints). HiGHS rejects with clear errors.
  - Data coefficients are supported: `SUM(profit * b * x)`.
  - Composes with scopes: `sum(when c: b * x)`.
  - Triple or higher products (`a * b * c`) are rejected.

Tests: `test/decide/tests/test_scope_objective.py`.

## 5. Aggregations

- `SUM()` is the primary aggregate over decision variables.
- `AVG(expr)` is `SUM(expr)` divided by N, the number of values the reducer receives:
  the rows of its `by` group that the clause's `when` and its own `when` keep, or the
  classes of its own `per` (§5.3). For objectives, `AVG` and `SUM` share the same
  argmax/argmin. For constraints, `AVG(expr) op K` becomes `SUM(expr) op K*N`.
- `SUM`/`AVG` bodies may include data-only additive terms alongside decision terms, e.g. `SUM(x + cost) <= K` or `SUM(q * (price + x)) <= K`. DeciDB evaluates the data-only aggregate contribution over the rows the reducer reads and subtracts it from the scalar RHS before building the solver row.
- `MIN(expr)` and `MAX(expr)` are supported. Easy cases (`MAX(expr) <= K`,
  `MIN(expr) >= K`) become a bound on each member, with no auxiliary variable; with no
  `when` and no filter of its own, a member that is a single decision is bounded in its
  column box instead of by a row, in any affine or scaled spelling (so `min(x) >= -5`,
  `min(x) - 1 >= -6` and `-min(x) <= 5` all make `x` signed, §2). Hard cases
  (the opposite direction, equality) add an extremum column per `by` group, pinned by
  Big-M indicator rows. In objectives,
  `MINIMIZE MAX(expr)` and `MAXIMIZE MIN(expr)` use one auxiliary; `MAXIMIZE MAX(expr)`
  and `MINIMIZE MIN(expr)` additionally require Big-M indicators. They compose with
  `when`, `per` and `by`.
- Each reducer can filter its own rows: `sum(when morning: x * hours) +
  sum(when evening: x * hours) <= 40` (§5.3).
- **A factor on a reducer** is supported for every aggregate kind and on either side:
  `2 * SUM(x*p)`, `SUM(x*p) * 2`, `SUM(x*p) / 2`, `2 * MAX(x*v)`, and inside a composed
  side (`SUM(x) + 2 * MAX(x*v)`). A negative factor reverses the relation the
  aggregate faces — `-2 * MAX(e) >= -8` is `MAX(e) <= 4`, the cheap per-member
  direction — and `-2 * max(e)` is `min(-2 * e)` (§5.2); a factor whose sign is not
  known before the query runs takes the composed formulation, which is exact for
  either sign.
  - **In a constraint**, the factor may be any term without a decision: a literal, an
    uncorrelated scalar subquery, or a value that varies by row — a column
    (`D.maxShipmentShare * sum(ship) by (D.depotID)`) or a correlated subquery. It is
    a tuple term: under `per` the key must determine it (§7.1), and a row-varying one
    gives the comparison the helper-decision shape (§6.4).
  - **In the objective**, the factor must be one value over the kept rows. A
    row-varying one is refused: *"DECIDE objective: 'weight' varies per row, so it
    cannot multiply SUM(x). Move it inside the aggregate, e.g. SUM(x * weight)."* A
    correlated subquery is refused with a message that calls it a subquery rather
    than quoting the internal column name flattening gives it: *"DECIDE objective:
    this subquery returns a different value for each row, so it cannot multiply
    SUM(x). Move it inside the aggregate, e.g. SUM(x * (SELECT ...))."*
  - A **decision** factor is refused in a constraint and in the objective, even a
    query-wide one: `s * sum(x)` gives *"DECIDE constraint: 's' is a decision, so it
    cannot multiply SUM(x). Only data can scale SUM/AVG/MIN/MAX."* (in the objective:
    *"... Only constants and query-wide values can scale SUM/AVG/MIN/MAX."*) The product written inside the reducer, `sum(s * x)`, is a
    bilinear term (§4).
  - A **data-only reducer** (`sum(cap)`, `avg(price)`, `count(*)`) may not share a scaled term with a decision: `x * avg(cap)`, `sum(x) / sum(cap)` and `(sum(x) + sum(cap)) / 2` are refused (*"An aggregate over table columns ... cannot multiply / divide / be divided together with a decision variable"*). It belongs on the bound side; as a factor, pre-compute it as an uncorrelated scalar subquery (`sum(x) / (SELECT sum(cap) FROM t)`).
- A query-wide decision may appear as a **bare additive term** of a reduced
  constraint, since it is row-invariant: `sum(ship) - max_shortfall <= cap` (paper
  §3.1). It contributes its coefficient once, not once per row. It may equally be
  written as the **bound** (`sum(ship) <= max_shortfall`); which side it is on carries
  no meaning, because canonicalization moves every decision-bearing term to the left
  before anything downstream looks. A per-row or keyed decision beside a reducer or as
  its bound (`sum(x) <= y`, `ship <= 0.20 * sum(ship)`) has one instance per row or
  class, and the comparison takes the helper-decision shape (§6.4).
- **Either side may carry the decision.** The constraint gate classifies both sides
  and accepts when either is a DECIDE expression, so `5 >= x`, `10 >= SUM(x)` and
  `cap >= SUM(x)` are the same constraints as their mirror images. A reducer may
  appear on **both** sides (`SUM(x*v) <= SUM(y*v)`, `SUM(x*v) <= MAX(x*w) + 30`).
- **The bound rule.** A side carrying no decision must give one value per generated
  constraint, whichever side it is written on. Without `per`, a value that varies by
  row is reduced to its tightest value within each `by` group of the kept rows (MIN
  for `<=`/`<`, MAX for `>=`/`>`) — a bare data column qualifies exactly like a scalar
  subquery (`sum(ship) by (depotID) <= stock`, the shape of the paper's Figure 1).
  Under `per` the key must determine it instead (§7.1). `=` refuses a bound that
  genuinely varies, naming it a contradiction (*"`cap` takes more than one value here
  (3 and 5) — an `=` bound cannot hold two different values at once, so this DECIDE
  query is infeasible."*). `<>` does not collapse at all: it keeps every value the
  bound took as its own exclusion (`SUM(x) <> 500 AND SUM(x) <> 350` within one
  group), not just the tighter of the two.
- `norm(expr, p)` expresses an L_p regularization term over a decision-variable
  expression (lasso/ridge-style). The user supplies the weight, e.g.
  `MINIMIZE SUM(cost*x) + 0.5 * norm(x - base, 1)`. Canonicalization replaces it with
  its definition (L0 is lowered by the optimizer), so it composes with a
  `when` or `per` in front of its clause, takes a `by` (`per g: norm(x, 1) by (g) <=
  4`), and works in both objectives and constraints (including norm-bounded
  constraints like `norm(e, 1) <= K`). It takes no scope inside its parentheses and no
  colon form (§10.1, §10.2). Supported orders:
  - `norm(e, 1)` → `SUM(ABS(e))` — L1 (sparse-leaning; ABS linearization).
  - `norm(e, 2)` → `SUM(POWER(e, 2))` — squared L2 / ridge (convex QP).
  - `norm(e, 'inf')` → `MAX(ABS(e))` — L-infinity (max linearization).
  - `norm(e, 0[, M])` → **L0 / count of nonzeros**. Adds one 0/1 indicator `z` per row, linked so `z = 1` **iff** `|e| >= tolerance` (a forward Big-M link forces `z=1` when `e != 0`, a reverse link `ABS(e) >= tol*z` forces `z=0` when `e` is within tolerance); the term becomes `SUM(z)`, the **exact** count. Upgrades the model to a MILP. Because the count is exact it is sound in **every** context — `MINIMIZE`/penalty and `<= K` cap, **and** `>= K`, `= K`, `MAXIMIZE`. The Big-M bound is **inferred from the data** when omitted — `norm(e, 0)` derives a tight per-problem `M` at execution from variable bounds (implied-bound propagation) + data; pass `norm(e, 0, M)` to supply it explicitly. The reverse link reuses ABS linearization, so `e` must have finite bounds. The nonzero tolerance (default `1e-4`, must exceed the solver feasibility tolerance) is configurable via `SET decide_l0_tolerance = …`; a value forced into `(0, tolerance)` is reported infeasible (tolerance-ambiguous).
  - Over data only (no decision in `e`, as on a bound: `SUM(x) <= norm(cap - 3, 0)`), `norm(e, 0)` is the plain count
    of the rows where `e` is not 0, `SUM(ABS(SIGN(e)))`, with no indicator and no `M`. A NULL or NaN `e` raises, as for
    any other value a bound reads.

### 5.1 The colon form — `SUM(D: e)`

By default a reducer combines one term per **result row**. A join repeats a table's
tuples once per match, so an unscoped reducer over a decision keyed on that table
weights it by how many rows its class joined with. The colon form reduces over the
named relation's tuples instead, contributing one term per tuple.

**Syntax**: `agg(Rel[, Rel, ...]: expr) [by (Γ)]`, where `agg` is `SUM`, `AVG`, `MIN`
or `MAX`, and each `Rel` is a table alias or table name bound in the `FROM` clause. It
means `agg(per Rel[, Rel, ...]: expr)` (§5.3): same result, same checks, and it may
take a `by`. It lists relations only; a column key needs the `per` spelling. EXPLAIN
and DIAGNOSE print it as written (§9).

```sql
SELECT routeID, depotID, open, ship
DECIDE per D: open(BOOL), ship(INT)
FROM Depots D JOIN Routes T USING (depotID)
SUCH THAT ship <= capacity * open AND SUM(ship) >= 300
MINIMIZE SUM(unit_cost * ship) + SUM(D: opening_cost * open);
```

A depot serving three routes appears in three result rows. `SUM(D: opening_cost * open)`
charges its opening cost **once**; the unscoped `SUM(opening_cost * open)` would charge
it three times.

- **Identity is the tuple, not the value.** A class is one tuple of all of `D`'s
  stored columns, so two depots with equal `opening_cost` are two terms. This is not
  `SUM(DISTINCT expr)`.
- **Scope is the surviving rows, not the base table.** The join, `WHERE` and the
  clause's `when` decide which tuples contribute; a depot filtered out has no term.
- **The named relations must determine the body**, checked against the data on every
  run (§7.1): a column or a decision of a relation that is not named is accepted only
  when it holds one value per class. `SUM(D: opening_cost + unit_cost * ship)` names
  `T`'s columns, and a depot with two routes stops the query (*"DECIDE: per D does not
  determine T.unit_cost (in sum(D: D.opening_cost + T.unit_cost * ship)). ... has more
  than one value: 2, 3."*) rather than picking an arbitrary route. Constants and query-wide
  decisions are always determined; a body that is only a query-wide decision is
  refused (`SUM(D: cap)`, §2.2). A relation that is not in the `FROM` clause is a
  Binder Error (*"Relation 'X' is not in the FROM clause, so a reducer cannot be
  qualified by it."*).
- **Construction order** is the rows `by` selects → one value per class of the named
  relations → aggregation; the scoped spelling `sum(when c per D: e)` adds its `when`
  before the classes form.
- **`AVG(D: expr)` divides by the number of classes**, not the number of rows.
- **`MIN`/`MAX` are unaffected by de-duplication** — every row of a tuple identity carries
  the same value, so dropping repeats cannot move an extremum. The qualifier is accepted
  and is a no-op for them.
- **Several relations at once — `SUM(D, T: expr)`.** The tuple identity widens to the
  concatenation of every named relation's own key, so a row is a duplicate only when it
  repeats on *all* of them together. Naming a relation removes the fan-out it would
  otherwise contribute to the reducer; a relation left unnamed still contributes its
  fan-out, uncollapsed — so with a third, unnamed relation `X` in the join,
  `SUM(D, T: expr)` differs from both `SUM(D: expr)` and the unscoped `SUM(expr)`.
  `expr` may draw columns and decisions from any of the named relations, not just one.
  The list is order-independent (`SUM(D, T: ...)` and `SUM(T, D: ...)` are the same
  reducer). One consequence follows directly: if `D` and `T` are the *only* two relations
  the query joins, naming both is a no-op — the composite key already is the join-result
  row, so `SUM(D, T: expr)` and unscoped `SUM(expr)` compute the same thing. The two
  forms only diverge once some relation the query joins is left out of the list.

### 5.2 `by (Γ)` — which rows a reducer reads

```sql
sum(ship)                                                 -- by (): all kept rows
sum(ship) by (D.depotID)                                  -- rows of the current depot
sum(ship) by (D.depotID, S.dispatchDay + S.transitDays)   -- an expression as a key
```

- **Applies to** `SUM`, `AVG`, `MIN`, `MAX`, `norm(e, p)` and `count(*)`, with or
  without a decision inside, on either side of a comparison.
- **`Γ`** lists columns, relations (all their stored columns; a column of the same
  name wins) and expressions over data. It may not mention a decision.
- **Omitted** means `by ()`: every kept row. So does a key that is the same on every
  row (`by (1)`, `by (NULL)`).
- **Rows read**: every kept row whose `Γ` equals the current row's — including rows
  outside the current class (§6.1).
- **NULL** is a value of `Γ`: NULL-keyed rows form one group (D5).
- **`Γ` must be determined** by the enclosing `per` key (§7.1). A key written twice in
  one clause is one key, a subquery in it included.
- **Placement.** `by` directly follows the reducer's `)` and belongs to that reducer
  only. After a call that is not one of the six it is a Binder Error; after anything
  but a function call it is a syntax error (§10.1).
- **`MIN` and `MAX`** are accepted wherever `SUM` is (D3).
- A data-only reducer on the bound side is evaluated over its own groups:
  `count(*) by (g)` counts the kept rows of the current row's `g`.
- **A factor holds the reducer's value**: `-2 * max(e) by (g)` is
  `min(-2 * e) by (g)`, and `-max(e)` is the factor -1.
- **`<` and `>` step a whole-numbered bound by one**: `max(x) by (g) > cap` is
  `max(x) by (g) >= cap + 1`. The step is on the extremum itself, so a strict bound
  refuses a `MIN` or `MAX` whose value can be fractional, a constant added inside
  included (*"Comparison '>' is not supported here: 'max(x + 0.5)' has type
  DECIMAL(21,1) ... Compare with '>=' instead."*).

Tests: `test/decide/tests/test_scope_reducer_by.py`.

### 5.3 `when` and `per` inside a reducer — `agg([when c] [per K]: e)`

```sql
sum(when S.priority: ship)                          -- only priority rows
sum(per D: D.opening_cost * open)                   -- each D tuple counted once
sum(when S.priority per D: D.opening_cost * open)   -- filter, then each remaining D tuple once
```

- **Order**: the rows `by` selects → the inner `when` → one value per class of the
  inner `per` (each class read at its first row the `when` keeps) → the aggregate.
- **Inner `when`** filters this reducer only. It is read on the rows the clause keeps,
  and the reducer's body on the rows both keep; other reducers of the clause, and the
  number of constraints, are unaffected. A `when` in front of the clause applies
  first.
- **Inner `per K'`** hands the reducer one value per class of `K'` among its rows, so a
  tuple repeated by a join is counted once. `K'` must determine the reducer's body
  (§7.1). `per row` is the default; `per ()` hands it a single value.
- **`AVG`** divides by the number of values it receives: kept rows, or classes when an
  inner `per` is present.
- **`MIN` and `MAX`** are unaffected by an inner `per`.
- **A reducer with no row for some generated class is an error** (D8), naming the
  class (§7.3). Only the inner `when` can empty a group. To leave such classes out,
  put the condition in front of the constraint:

```sql
-- error when some depot has no priority shipment
per D.depotID: sum(when S.priority: ship) by (D.depotID) <= D.priorityCapacity

-- only depots with a priority shipment get a constraint
when S.priority per D.depotID: sum(ship) by (D.depotID) <= D.priorityCapacity
```

Tests: `test/decide/tests/test_scope_reducer_inner.py`.

### 5.4 `per` and `by` are independent

| Written | Constraints | Each reducer reads |
|---|---|---|
| `ship <= 0.20 * sum(ship)` | one per row | all rows |
| `per (): sum(ship) <= networkCapacity` | one | all rows |
| `per D.depotID: reserve >= D.networkReserveShare * sum(S.demand)` | one per depot | all rows |
| `per D.depotID: sum(ship) by (D.depotID) <= D.capacity` | one per depot | that depot's rows |
| `ship <= D.maxShipmentShare * sum(ship) by (D.depotID)` | one per row | the rows of that row's depot |

## 6. Scoped constraints — `[when c] [per K]: C`

```sql
SUCH THAT
    when S.priority: ship >= 1 AND                                  -- the priority rows
    per D.depotID: sum(ship) by (D.depotID) <= D.capacity AND       -- one per depot
    per (): sum(when S.priority per D: D.opening_cost * open) <= 50 -- each depot once
MAXIMIZE when S.status = 'active': sum(ship)
```

`when` is written before `per`, and the colon is required whenever either is present.
The same form scopes the objective (§4) and a reducer's argument (§5.3).

Tests: `test/decide/tests/test_scope_generation.py`, `test_scope_combinations.py`,
`test_scope_bare_clauses.py`, judged by the scope oracle (`04_testing/scope/done.md`).

### 6.1 Evaluation

A scoped expression is evaluated on a set of rows in four steps:

1. **Filter.** Keep the rows where `c` is true.
2. **Generate.** Split the kept rows into classes by `K`. The body is produced once
   per class: one constraint, or one input value for the enclosing reducer.
3. **Aggregate.** A reducer `agg(...) by (Γ)` in the body reads all kept rows whose
   `Γ` equals the class's `Γ`, not only the class's own rows. Its argument is itself a
   scoped expression, evaluated on the rows the reducer reads.
4. **Well-defined.** `K` must determine every tuple term of the body and every `Γ`
   (§7.1).

```sql
-- kept rows
--   shipment  depot  region  ship  capacity
--   S1        D1     North   x1    100
--   S2        D1     North   x2    100
--   S3        D2     North   x3     80
--   S4        D3     South   x4     50

per D.depotID: sum(ship) by (D.region) <= D.capacity

--   classes: D1 {S1, S2}   D2 {S3}   D3 {S4}
--   D1: region North -> reads S1, S2, S3 ->  x1 + x2 + x3 <= 100
--   D2: region North -> reads S1, S2, S3 ->  x1 + x2 + x3 <= 80
--   D3: region South -> reads S4         ->  x4 <= 50
--   well-defined: every depot has one region and one capacity
```

### 6.2 `per K` — how many constraints

```sql
SUCH THAT
    ship <= S.demand AND                                      -- no per: one per row
    per D.depotID: sum(ship) by (D.depotID) <= D.capacity AND -- one per depot
    per (): sum(ship) <= 1000                                 -- exactly one
```

- **`per K:`** creates one constraint per class of `K` among the kept rows.
- **No `per`**, or `per row:`, means one constraint per kept row.
- **`per ():`** creates exactly one constraint.
- **One scope, one constraint.** In `per g: A AND B` only `A` is scoped; `B` starts a
  new constraint with the defaults. A `BETWEEN` is one constraint, both halves under
  the scope.
- **Key elements** are columns and relations, as in a declaration's key (§2.3).
  Expressions belong in `by`. A key of several columns is written without
  parentheses (`per a, b:`); parentheses are only for `per ()`.
- **NULL** is a key value: NULL-keyed rows form a class and get a constraint (D5). To
  leave them out, write `when a is not null per a:`.
- **`per` does not group reducers** (D6). `per g: sum(x) <= 10` bounds the total over
  all kept rows, once per `g`, and the identical constraints are one model row. To
  bound each group's own sum, write `per g: sum(x) by (g) <= 10`.
- **An explicit `per` must determine the body** (§7.1); no `per` always does. With no
  `per`, `sum(x) <= cap` is one constraint per kept row, each pairing the same total
  with that row's `cap`: the total against the tightest `cap`, and
  `sum(x) by (g) <= cap` the tightest `cap` in each group (the bound rule, §5). The
  same clause under `per g:` instead requires `g` to determine `cap`.

### 6.3 `when c` — which rows

```sql
SUCH THAT
    when S.priority: ship >= 1 AND
    when S.status = 'active' and S.demand > 0 per D.depotID:
        sum(ship) by (D.depotID) <= D.capacity
```

- **Condition.** Any boolean expression over data, ended by `per` or `:`; parentheses
  are not needed, and an `and` before that end belongs to the condition. It may not
  mention a decision, a reducer (*"DECIDE: an aggregate cannot be used here. ..."*) or
  a window function (§10.1).
- **NULL** counts as false: the row is dropped.
- **Filtering comes first.** Classes form from kept rows only, so a key value with no
  kept row has no constraint, and every reducer in the body reads kept rows only.
- **Nothing is computed on a dropped row**: a NULL there is no error, nor an
  expression that would fail there (`CAST(s AS INTEGER)` of a text, `ln(0)`) — in the
  clause, its keys, its bounds, its checks (§7.3) or any row a rewrite adds for it
  (`IN`, `ABS`, `BETWEEN`, `norm`). The parts of an `and` or an `or`, at any depth
  outside a `not`, are read left to right, each only on the rows the parts before it
  leave undecided: `when s <> '' and CAST(s AS INT) > 3:` casts no empty text.
- **Dropped rows keep their decisions.** The clause simply does not constrain them.
- **A `when` that keeps no row is an error** (D4), whether or not the constraint has a
  reducer (§7.3). An input with no rows at all is not an error: the result is empty.

### 6.4 How a clause becomes model rows

**What a constraint body may contain.** Tuple terms and reducers mix freely, on either
side: `ship <= 0.20 * sum(ship)`, `reserve >= 0.10 * sum(S.demand) by (D.region) +
0.02 * sum(S.demand) by (D.country)`. Each reducer has its own `by`, so several
reducers in one constraint may read different rows. A factor on a reducer may be any
term without a decision (§5); a decision as a factor is refused. Operators, `BETWEEN`,
`IN`, `ABS`, `norm`, quadratic and bilinear terms, subqueries and NULL handling follow
§3–§5.

**The three shapes.** Each comparison takes one of three shapes:

- **Rows** — no reducer on the decision side: one model row per class, built from its
  first kept row (the key determines the clause, §7.1).
- **Grouped** — decision reducers sharing one `by` key, under whole-query factors,
  beside query-wide terms only: one model row per `by` group of the kept rows, under
  the group's tightest bound; `=` refuses a group whose bounds differ, `<>` excludes
  every value. With no `per`, a bound that varies inside a group is that tightest
  bound.
- **Helper decision** — anything else on the decision side: a per-row or keyed
  decision beside a reducer (`ship <= 0.20 * sum(ship) by ()`, the facility link
  `sum(ship) by (D.depotID) <= D.capacity * open`), a factor that varies by row
  (`ship <= D.maxShipmentShare * sum(ship) by (D.depotID)`), reducers with different
  `by` keys, a reducer inside another expression
  (`abs(sum(ship) by (g) - 40) <= 5`). Each reducer becomes a **helper decision**: one
  instance per `by` group of the kept rows, holding the reducer's value over the rows
  it reads, defined once (an equality row per group for `SUM`/`AVG`; for `MIN`/`MAX`
  an extremum link per group, pinned on the side the row would otherwise let it escape
  through, both for `=`, `<>` and a factor of unknown sign) and used by one row per
  class. The helper is whole-numbered when the reducer's value is, every constant
  inside counted, so `<`, `>` and `<>` keep their rules where it is and are refused
  where it is not (`ship + sum(ship + 0.5) by (D.region) > 40`, §10.1); it may be
  negative. Deck examples 1, 5 and 8 take this shape; example 1 costs N rows of two
  entries and one definition row, not N rows of N. A product or square of decisions
  inside such a reducer is refused (§10.2).

`EXPLAIN` names a helper by its reducer (`sum(ship) by (D.depotID)`) and says it became
a helper decision; `DIAGNOSE` names the clause and never offers a definition row.

Tests: `test/decide/tests/test_scope_value_rewrite.py`.

### 6.5 Deck examples 1–8

All run as written, and in their fully explicit form, over:

```sql
FROM Shipment S JOIN Depot D USING (depotID)        -- one result row per shipment
DECIDE ship(INT), per D.depotID: reserve(REAL)
```

`networkCapacity` is any column with one value over all rows.

| # | As written | Fully explicit | Constraints | Reducer reads | Must be determined |
|---|---|---|---|---|---|
| 1 | `ship <= 0.20 * sum(ship)` | `per row: ship <= 0.20 * sum(per row: ship) by ()` | one per shipment | all rows | — |
| 2 | `per (): sum(ship) <= networkCapacity` | `per (): sum(per row: ship) by () <= networkCapacity` | one | all rows | `networkCapacity` constant |
| 3 | `per D.depotID: reserve >= D.networkReserveShare * sum(S.demand)` | `… * sum(per row: S.demand) by ()` | one per depot | all rows | `reserve`, `D.networkReserveShare` |
| 4 | `per D.depotID: sum(ship) by (D.depotID) <= D.capacity` | `… sum(per row: ship) by (D.depotID) …` | one per depot | the depot's rows | `D.capacity` |
| 5 | `ship <= D.maxShipmentShare * sum(ship) by (D.depotID)` | `per row: ship <= D.maxShipmentShare * sum(per row: ship) by (D.depotID)` | one per shipment | the rows of its depot | — |
| 6 | `per D.depotID: reserve >= 0.10 * sum(S.demand) by (D.region) + 0.02 * sum(S.demand) by (D.country)` | each `sum(per row: S.demand)` | one per depot | the depot's region; the depot's country | `reserve`, `D.region`, `D.country` |
| 7 | `per D.depotID: sum(when S.priority: ship) by (D.depotID) <= D.priorityCapacity` | `… sum(when S.priority per row: ship) …` | one per depot | the depot's priority rows | `D.priorityCapacity` |
| 8 | `per S.shipmentID: ship <= 0.25 * sum(ship) by (D.depotID, S.dispatchDay + S.transitDays)` | `… sum(per row: ship) by (…)` | one per shipment | rows with the same depot and arrival day | `ship`, `D.depotID`, the arrival day |

- Example 7 is an error on data where some depot has no priority shipment (§5.3).
- Example 8 needs `S.shipmentID` to determine the per-row `ship`, which holds because
  each shipment is one result row.

Tests: `test/decide/tests/test_scope_deck_examples.py`.

## 7. Determination, NULL and empty sets

### 7.1 The determination rule

A term must have one value per generated constraint. So a `per` key must determine
everything its body reads once per class:

- every **tuple term** — a column or expression outside reducers, including the bound
  and any factor on a reducer;
- every **decision** outside reducers;
- every reducer's **`by` key**.

An inner `per` key, and the relations of a colon form, must likewise determine the body
of their reducer.

- **A decision** declared with key `Kv` is determined by `K` when all rows of each
  `K`-class belong to one `Kv`-class. A per-row decision is determined only when every
  `K`-class is a single row. A `per ()` decision is always determined.
- **Always determined**: literals, uncorrelated scalar subqueries, and anything under
  `per row`.
- **`per ()`** — and the objective, which is produced once — requires each tuple term
  to have one value over all kept rows.
- **Checked against the data**, on every run, before solving, over kept rows only
  (§7.3). The same query can therefore pass on one dataset and fail on another.
- **NULL** counts as one value in this comparison. A NULL in a coefficient or bound is
  still the NULL error of §3.
- **On failure** the query stops with an error that names the key, the expression, and
  one class holding two values (§7.3).

```sql
-- holds: every depot has one region
per D.depotID: sum(ship) by (D.region) <= 500

-- fails when a depot has shipments in two S.region values
per D.depotID: sum(ship) by (S.region) <= 500

-- fails: ship is per row, and a depot has several rows
per D.depotID: ship <= 10

-- holds only if every row carries the same networkCapacity
per (): sum(ship) <= networkCapacity
```

Tests: `test/decide/tests/test_scope_determination.py`.

### 7.2 NULL and empty sets

| Situation | Outcome |
|---|---|
| NULL in a declaration's `per` key | its own decision (D5) |
| NULL in a constraint's `per` key | its own class, with a constraint (D5) |
| NULL in a `by` key | its own group (D5) |
| NULL `when` condition | row dropped |
| NULL in a coefficient or bound | error (§3) |
| a `when` in front of a constraint or the objective keeps no row | error (D4) |
| a key value has no kept row | no class, no constraint, no error |
| a reducer's own `when` empties a group of a generated class | error naming the class (D8) |
| the query's input has no rows at all | empty result |

### 7.3 Data checks

Run before solving, on every run:

| Check | Message |
|---|---|
| A `when` keeps no row of a non-empty input (constraint or objective) | `DECIDE: when S.status = 'actve' matches no rows.` |
| A `per` key determines every tuple term (a bound, a factor on a reducer), every decision outside reducers and every `by` key, over the kept rows; an inner `per` key or a colon form's relations, the body of its reducer | `DECIDE: per D.depotID does not determine D.capacity. D.depotID = 'D1' has more than one value: 25, 40.` · `... does not determine the decision ship (declared per row). D.depotID = 'D1' covers more than one ship.` · `(in by (S.region))`, `(in sum(per D: ...))`, `(in sum(D: ...))` · for `per ()`: `The kept rows have more than one value: 3, 5.` |
| A reducer's own `when` leaves a `by` group of the kept rows empty | `DECIDE: sum(when S.priority: ship) has no rows for D.depotID = 'D2'.` · `... has no rows.` for `by ()` |

The phrases `does not determine`, `matches no rows` and `has no rows for` are fixed;
the names and values around them are quoted from the query and the data. NULL counts
as one value in a check; a NULL coefficient or bound is still the NULL error of §3,
which names a subquery by its text as DuckDB prints it (*"DECIDE: the subquery
(SELECT ...) is NULL. ..."*). An input with no rows at all is not an error. The checks
also run under `DIAGNOSE`. Over a join, a message names a column with its relation
(`D.capacity`).

## 8. `DIAGNOSE` — asking a query to explain itself

`DIAGNOSE` is a prefix on a `SELECT` that carries a `DECIDE` clause:

```sql
DIAGNOSE SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= 5 AND x >= 8 MAXIMIZE SUM(x);
```

It runs the query and reports on the run instead of returning its rows — the same
relationship `EXPLAIN ANALYZE` has to an ordinary query. **It is the only thing that
starts the diagnostics engine.** A query without the prefix never pays for a diagnostic
solve.

### 8.1 What it returns

A relation, not a printed report, so it composes:

```
state | clause | suggested_change | amount | total | scope | edit_source | group | row
```

| column             | type    | what it holds                                                               |
| ------------------ | ------- | --------------------------------------------------------------------------- |
| `state`            | VARCHAR | `infeasible`, `unbounded`, or `feasible`                                     |
| `clause`           | VARCHAR | the clause as you wrote it (in the echo style of §9), or the runaway decision's name |
| `suggested_change` | VARCHAR | the smallest edit that addresses this finding                                |
| `amount`           | DOUBLE  | how far a bound moves, escaping instances, or the achievable objective       |
| `total`            | BIGINT  | denominator for an unbounded escape count; otherwise NULL                    |
| `scope`            | VARCHAR | `row`, or `entity` for a keyed decision, on an unbounded escape count; otherwise NULL |
| `edit_source`      | VARCHAR | what kind of finding this row is (below)                                     |
| `group`            | VARCHAR | the class's `per` key value (expanded scope, one finding per class; a NULL key value prints as the text `NULL`), or the categorical slice a decision escapes on |
| `row`              | BIGINT  | the kept row this finding covers, under the `expanded` slack scope           |

One row per finding. A query that solves returns exactly one row, `state = 'feasible'`,
everything else NULL — there is no separate output path for a query that worked.

For an unbounded finding with a count, `amount / total` is the escape rate and `scope`
identifies the counted unit. When `group` names a categorical slice, `total` is that
slice's size; otherwise it is the variable's total count of row or class instances.

`edit_source` is the finding's kind, and the column to filter on:

| value                  | meaning                                                          |
| ---------------------- | ---------------------------------------------------------------- |
| `source_literal`       | a literal you wrote, loosened in place                            |
| `virtual_offset`       | a synthetic offset over a data-backed RHS (`x <= col + delta`)    |
| `expanded_row`         | one kept row's own overshoot (`expanded` slack scope)             |
| `expanded_group`       | one class's own overshoot (`expanded` slack scope)                |
| `remove_only`          | a clause that cannot be loosened, only deleted (`<>`, `IN`, L0)   |
| `unreachable_bound`    | a bound no assignment can reach (`x >= inf`)                      |
| `rigid_conflict`       | loosening the clauses you wrote cannot restore feasibility        |
| `runaway_+inf` / `-inf`| a decision growing without bound, and which way                   |
| `achievable_objective` | what the objective reaches once the edits are applied             |
| `unbounded_after_fix`  | the fix works, but the objective can then grow without limit      |
| `undiagnosed`          | the state is known but no engine could name a cause               |

Because it is a relation, it can be selected from and filtered:

```sql
SELECT clause, suggested_change
FROM (DIAGNOSE SELECT ... DECIDE ...)
WHERE amount > 1000;
```

### 8.2 What a query WITHOUT the prefix does

It reports its state and stops:

```
DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see which
clause to change.
```

No clause name, no repair, no second statement to run. Naming the clause *is* the
elastic solve, and that only happens when you ask for it.

### 8.3 Restrictions

- The inner query must contain a `DECIDE` clause. `DIAGNOSE` reports on an optimization
  run; a plain SQL query has none, and is rejected with a message saying so.
- The complete bound plan must contain **exactly one** `DECIDE` operator. A DECIDE query
  inside an ordinary subquery is accepted when it is the only one. An outer DECIDE with
  a nested DECIDE, or sibling subqueries that each contain DECIDE, is rejected so one
  diagnosis relation can never be mistaken for the result of another solve.
- `DIAGNOSE` takes no options. There is no `DIAGNOSE (VERBOSE) …`.
- A query that fails before it can be solved — a syntax error, a semantic error, a model
  class the host's solver refuses — still raises under the prefix. `DIAGNOSE` explains
  the *outcome of a solve*; there is nothing to explain when no solve happened.

### 8.4 Tuning the engine

These settings configure *how* the engine works once `DIAGNOSE` has started it. None of
them starts or suppresses it.

| setting                                     | what it does                                                                                    |
| ------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| `diagnose_decide_infeasible_slack_scope`    | `query` (default): one edit per SQL-level knob. `expanded`: one per kept row, or per class for a clause with a `per` key — fills `group` and `row`. |
| `diagnose_decide_escape_rate`               | report a categorical slice when its within-group escape rate is at least this (default 0.8).      |
| `diagnose_decide_categorical_ratio`         | treat a column as categorical when distinct values ≤ ratio × rows (default 0.1).                  |
| `diagnose_decide_min_categories`            | absolute floor on that cap, so small tables still qualify (default 20).                           |

### 8.5 A slow solve is not a diagnosis

Hitting the time limit, or pressing Ctrl-C, is ordinary execution behaviour and happens
with or without the prefix. The query prints a checkpoint report — what was found and
how far it can still improve — and then:

- **at a terminal**, offers to keep solving on the warm solver (Enter continues, `s`
  stops and takes the best so far);
- **anywhere else** (scripts, pipes, benchmarks, the C API), errors — there is nobody to
  answer the offer;
- **after Ctrl-C**, hands back the best solution found so far with the caveat that it is
  not proven best, terminal or not. You already said stop; there is nothing to ask.

## 9. Inspecting a decision plan — `EXPLAIN`

DuckDB's three plan-inspection forms work on DECIDE queries:

```sql
EXPLAIN SELECT id, x FROM t DECIDE x(BOOL)
SUCH THAT SUM(x * weight) <= 50 MAXIMIZE SUM(x * value);

EXPLAIN ANALYZE SELECT ... DECIDE ...;
EXPLAIN (FORMAT JSON) SELECT ... DECIDE ...;
```

The DECIDE node shows the declared variables, objective, and constraints. It leads with
the SQL the user wrote, then adds indented canonical or rewritten rows only when the
model differs materially from that spelling. Under each clause, one line per rewrite says
what it made of the clause (`abs(ship - 20) becomes an auxiliary variable`,
`sum(ship) by (D.region) becomes a helper decision per group`, `max(ship) by (D.region)
becomes an extremum column pinned by indicator rows`, `avg(ship) becomes a sum divided by
its row count`), and an auxiliary column prints as the expression it stands for
(`abs(ship - 20)`, `(open * ship)`, `(ship = 5)`), never by an internal name; a bound the
engine derives from the data prints as `M`. The objective's rewrites, and the rows they
add, print under the objective. Internal auxiliary variables are excluded from the
declaration list.

**Echo.** EXPLAIN, DIAGNOSE and DECIDE messages print a clause in one style: reducers
and the scope keywords in lower case, the `when c per K:` qualifier in front of the
clause, a reducer's `by (Γ)` after it, and the defaults `per row:` and `by ()` left out
(`per row: ship <= 0.2 * SUM(ship) by ()` prints as `ship <= 0.2 * sum(ship)`). The
colon form prints as written (`sum(D: open)`). A key prints as written; EXPLAIN and
DIAGNOSE print another column by its bare name (`when priority:` for
`when S.priority:`), where a message quotes it as written. DIAGNOSE's
labels for a `MIN`/`MAX` extremum column and a `<>` indicator use the same style
(`max(ship)`, `sum(ship) by (D.region) <> 20`).

`EXPLAIN` plans without executing the decision query. `EXPLAIN ANALYZE` executes it and
adds actual row counts and timing. JSON uses the same content in a structured plan.

Tests: `test/decide/tests/test_scope_explain_diagnose.py`.

## 10. Errors, limits and removed spellings

### 10.1 Errors

| Situation | Raised | Message |
|---|---|---|
| A `per` key does not determine a value or a decision; a `when` keeps no row; a reducer's own `when` empties a group | run, before solving | §7.3 |
| A reducer in a `when` | run, before solving | `DECIDE: an aggregate cannot be used here. ...` |
| `per` other than `()` on the objective | parse | `an objective is produced once: write per (): or leave per out` |
| A parenthesized key | parse | `write per a, b: without parentheses; parentheses are only for per ()` |
| An expression in a `per` key | parse | `a per key lists columns or relations; put expressions in by (...)` |
| A declaration with no type | parse | `DECIDE variable "x" needs a type; write x(INT), x(BOOL) or x(REAL)` |
| A comma between constraints | parse | `comma-separated SUCH THAT constraints are not supported; use AND between constraints` |
| `by` after anything but a function call (a column, a literal, `CASE`, `CAST`, `TRY_CAST`, `::`, `COALESCE`, `NULLIF`, `EXTRACT`, parentheses), or a scope inside such a form | parse | syntax error |
| A removed spelling | parse (`D.x`: bind) | names the new spelling; after a clause with a scope in front, a syntax error (§10.3) |
| `OR` between constraints | bind | `SUCH THAT: OR is not supported.` |
| `by` after a call that is not `SUM`, `AVG`, `MIN`, `MAX`, `norm` or `count` | bind | `DECIDE: by (...) must directly follow SUM, AVG, MIN, MAX, norm or count` |
| `when` or `per` inside a call that is not `SUM`, `AVG`, `MIN` or `MAX` (`norm` and `count` included) | bind | `DECIDE: when and per inside a call need SUM, AVG, MIN or MAX; write the scope in front of the constraint or inside a reducer` |
| A decision in a `when`, a key or a `by` | bind | `DECIDE: x is a decision; when, per and by may only use data` |
| A window function in a `when`, a key or a `by` | bind | `DECIDE: a window function such as 'row_number() OVER ()' is not supported in when, per or by` |
| A volatile function (`random()`) anywhere in a constraint, the objective or a `by` key; compute it once per row in a subquery or CTE instead | bind | `DECIDE: random() is not supported in a constraint` (`the objective`, `a by key`) |
| An unknown key element (a key resolves in its own query's `FROM` only) | bind | `DECIDE: depot is neither a column nor a relation of the FROM clause` |
| `rowid` or a generated column in a key | bind | `DECIDE: g.b is not a stored column; a per key lists columns or relations of the FROM clause` |
| An unqualified `FULL OUTER JOIN USING` column in a key | bind | `DECIDE: depotID is merged by a FULL OUTER JOIN USING; write S.depotID or D.depotID in the per key` |
| A reducer or `norm` with no argument | bind | `SUM needs one argument, found 'sum()'` · `NORM needs an expression and an order, as in norm(e, 1), found 'norm()'` |
| A colon form naming a relation not in the `FROM` clause | bind | `Relation 'X' is not in the FROM clause, so a reducer cannot be qualified by it.` |
| A reducer over query-wide decisions only, as a constraint's whole decision side (§2.2) | bind | `cap is a query-wide decision, so SUM(cap) has nothing to aggregate over; use cap on its own` |
| A decision as a factor on a reducer; a row-varying factor on a reducer in the objective; a data-only reducer scaling a decision | bind | §5 |
| `<`, `>` or `<>` on a side that is not whole-numbered | bind | §3 |
| `<`, `>` or `<>` on a reducer a helper decision replaces whose value can be fractional (`ship + sum(ship + 0.5) by (D.region) > 40`) | bind | `Comparison '>' is not supported here: 'sum(ship + 0.5) by (D.region)' can take fractional values in this constraint, and stepping the bound is exact only on whole numbers. Compare with '>=' instead.` |

Messages quote the user's SQL, never an internal name. All of these are still raised
under `DIAGNOSE`: nothing was solved, so there is nothing to diagnose (§8.3).

### 10.2 Not supported yet, and not in the language

Each shape below is refused with a message, never answered wrongly.

| Shape | Message |
|---|---|
| A reducer inside a reducer, but the two-level objective (deck example 9 included) | `DECIDE: an aggregate inside an aggregate is not supported yet` |
| The two-level objective beside another term or under a factor | `DECIDE: an aggregate inside an aggregate is not supported yet` |
| A `per` inside the two-level objective's inner reducer | `DECIDE: per inside the inner reducer of min(per g: sum(...) by (g)) is not supported yet` |
| A `when` inside either reducer of the two-level objective | `DECIDE: when inside the outer reducer of min(per g: ...) is not supported yet; write it in front of the objective` · `... inner reducer ...` |
| A two-level `by` key other than its `per` key | `DECIDE: in min(per g: sum(...) by (id)) the by key must be the per key; any other is not supported yet` |
| An outer `AVG` in the two-level objective | `DECIDE: avg over the classes of avg(per g: sum(...) by (g)) is not supported yet; write sum, min or max over them` |
| `norm(D: e, p)` | `DECIDE: norm(D: ...) is not supported yet; a relation qualifier takes SUM, AVG, MIN or MAX` |
| A product or square of decisions inside a reducer that a helper decision replaces (§6.4): a per-row or keyed decision or a product of decisions beside it, a row-varying factor on it, another `by` key next to it, an enclosing expression | `DECIDE: 'sum(ship * ship)' is not supported yet in this constraint: a product or square of decisions inside a reducer is accepted only where the constraint's reducers share one by key, under a whole-query factor, with nothing beside them but data and query-wide decisions, each on its own` |
| `AVG` under `<>` beside another term, or with its own `when`/`per` | `DECIDE: AVG in a <> constraint is not supported yet ...` |
| `<>` over a square or a product of decisions that is not linearized first (a product with a `BOOL` decision is) | `DECIDE: <> over a product or a power of decisions is not supported yet: power(x, 2) <> 9` |
| A decision beside a `MIN` or `MAX` in the objective | `DECIDE: 's' beside MIN or MAX is not supported yet` |
| A square, or a product of two decisions, outside a reducer in the objective, unless the two are distinct query-wide decisions or one is `BOOL` | `DECIDE objective: 'x * y' multiplies decisions outside a reducer, which is not supported yet` |
| A scope inside an ordinary SQL subquery written in the clause | `DECIDE: 'WHEN condition:' is not supported yet` |

A reducer over data only stays on the bound side of a comparison (§5).

**Not in the language** (shown in the ANR deck, deliberately left out): variant
signatures, frame selectors (`at`, `over`, `within`), `if` guards, bounds in a
declaration, the `SEMIREAL` / `SEMIINT` / `TEXT` types, `then` and `satisfy`.

### 10.3 Removed spellings

Each is a parser error whose message carries the phrase `is removed` and names the
spelling to write instead, built from the query's own text so that it pastes back
(`D.x` is the one exception: a Binder Error with no `is removed`). A condition is quoted
whole as written (`SUM(cap) WHEN cap BETWEEN 2 AND 5` becomes `sum(when cap BETWEEN 2 AND 5:
cap)`), a name that needs quotes is quoted, and `count(*)` is quoted as `count(*)`. A
removed form inside a subquery of a clause is refused the same way. A `WHEN` after a
reducer followed by a bare comparison (`SUM(x) <= SUM(cap) WHEN cap > 2`) was never a
working spelling and stays a plain syntax error. The messages below are for
`FROM ... t(id, cap, g)`.

| Removed | Write instead | Message (key phrase) |
|---|---|---|
| `T.x(TYPE)` | `per T: x(TYPE)` | `the declaration t.x(INT) is removed; write per t: x(INT) (per t keys on every column of t; write per t.k: x(INT) to key on one column k)` |
| `T.x` (no type) | `per T: x(INT)`, `per T: x(BOOL)` or `per T: x(REAL)` | `the declaration t.x is removed; write per t: x(INT), per t: x(BOOL) or per t: x(REAL) (per t keys on every column of t)` |
| `scalar x(TYPE)`, `scalar T.x(TYPE)` | `per (): x(TYPE)` | `scalar s(INT) is removed; write per (): s(INT)` |
| `scalar x` (no type) | `per (): x(INT)`, `x(BOOL)` or `x(REAL)` | `scalar s is removed; write per (): s(INT), per (): s(BOOL) or per (): s(REAL)` |
| `C PER g` · `C PER (g, h)` · `C PER g, h` | `per g, h: C`, with `by (g, h)` after each reducer of `C` | `the postfix PER is removed; write per g: in front of the constraint and by (g) after each of its reducers; a NULL g is now its own group, so write when g is not null per g: to leave those rows out` |
| `C PER g` when `C` holds no reducer | `C` (it already holds on every row) | `the postfix PER is removed; a constraint with no reducer already holds on every row, so drop the PER` |
| `C WHEN c` | `when c: C` | `the postfix WHEN is removed; write when g = 'A': in front of the constraint` |
| `C WHEN c PER g` | `when c per g: C`, with `by (g)` as above | `the postfix PER is removed, as is the WHEN before it; write when g = 'A' per g: in front of the constraint and by (g) after each of its reducers; a NULL g is now its own group, so write when g = 'A' and g is not null per g: to leave those rows out` |
| `SUM(e) WHEN c` | `sum(when c: e)`; after a reducer with a scope or a `by`, the condition joins it (`sum(when d and c: e) by (g)`) | `the WHEN after sum(x) is removed; write sum(when g = 'A': x)` |
| `COUNT(*) WHEN c` · `norm(e, p) WHEN c` | `sum(when c: 1)` · `when c:` in front of the constraint when the condition holds for all of it | `... write sum(when g = 'A': 1)` · `... norm takes no when inside it: write when g = 'A': in front of the constraint when the condition holds for all of it` |
| `SUM(D: e) WHEN c` | `sum(when c per D: e)` | `the WHEN after sum(t: x) is removed; write sum(when g = 'A' per t: x)` |
| `MAXIMIZE E WHEN c` | `MAXIMIZE when c: E` | `the postfix WHEN is removed; write MAXIMIZE when g = 'A': sum(x)` |
| `MINIMIZE OUTER(INNER(e)) PER g` | `MINIMIZE outer(per g: inner(e) by (g))`; an outer `AVG` becomes `sum`; a `WHEN` inside the inner reducer goes in front | `the postfix PER is removed; write MINIMIZE sum(per g: max(x) by (g)); a NULL g is now its own group, so write MINIMIZE when g is not null: sum(per g: max(x) by (g)) to leave those rows out` · `... (an average over the classes is not supported yet; a sum ranks the solutions the same way)` |
| `MINIMIZE OUTER(INNER(D: e)) PER g` · `MINIMIZE c * OUTER(INNER(e)) PER g` | no spelling yet | `the postfix PER is removed; the two-level form max(per g: inner(e) by (g)) takes no per inside its inner reducer yet` · `... the two-level form takes one reducer call inside the outer one ... is not supported yet` |
| `MAXIMIZE SUM(e) PER g` (flat) | `MAXIMIZE sum(e)` | `the postfix PER is removed; an objective is produced once: write MAXIMIZE sum(x)` |
| `MAXIMIZE MAX(e) PER g` (flat `MIN` or `MAX`) | `MAXIMIZE max(e)`, or `MAXIMIZE sum(per g: max(e) by (g))` | `... an objective is produced once: write MAXIMIZE max(x), or MAXIMIZE sum(per g: max(x) by (g)) to combine one value per class` |
| `MAXIMIZE AVG(e) PER g` (flat) | `MAXIMIZE when g is not null: sum(per g: avg(e) by (g))` (what it meant), or `MAXIMIZE avg(e)` | `the postfix PER is removed; write MAXIMIZE when g is not null: sum(per g: avg(x) by (g)) to add up one average per class as before (a NULL g is now its own group), or MAXIMIZE avg(x) for one average over all rows` |
| `D.x`, a decision referenced with its relation | `x` | `DECIDE: write open, not t.open; a decision is referenced by its bare name` · in a `when`, `per` or `by`: `DECIDE: t.open is a decision; when, per and by may only use data` |

- **NULL keys.** A NULL key value is its own class under `per` and its own group under
  `by` (§7.2). `when g is not null per g:` leaves those rows out, as the `PER` message
  says.
- **`per T` keys on every column of `T`.** Two rows of `T` share a decision only when
  every stored column of `T` agrees; `per T.k:` keys on the column `k` alone.
- A removed spelling written after a clause that already has a scope in front is a
  plain syntax error (*syntax error at or near "PER"*).
- The colon form `SUM(D: e)` is current syntax (§5.1).
- A view or macro stored with a removed spelling prints as written and is refused when
  it is bound, with a message naming the new spelling.

Tests: `test/decide/tests/test_scope_removed_syntax.py`; the scanner
`test/decide/tests/test_no_old_spellings_remain.py` (with `_old_spellings.py`) keeps
these spellings out of every test surface; the removed-spelling cases of
`test/decidb/test_scope_parser.test`.

## 11. Direct Solve — simple problems without a solver

Some DECIDE queries have a closed-form answer that DuckDB can compute with ordinary
operators (windows, filters, projections). DeciDB detects those queries and answers them
directly instead of building a model and calling Gurobi or HiGHS. It is on by default and
needs no syntax change. A query it cannot prove takes the normal solver path, unchanged.

Today it covers three kinds of problem:

- **Choose rows by score.** One per-row `BOOL` decision under limits on **how many rows are selected**
  (`sum(x) <= k`, `<`, `>=`, `>`, `=`), over all rows or the rows a `when` keeps, with a linear score
  `sum(coefficient * x)` and optional per-row pins such as `when id = 7: x = 1`. The limits can be constants or
  columns.
- **Independent decisions in a range.** One or more per-row `BOOL`, `INT` or `REAL` decisions where every row has
  bounds of its own (`x BETWEEN 0 AND cap`, `x <= cap`, `2*x <= cap`) and nothing links two rows. The objective is a sum
  with one term per row: linear (`sum(score * x)`) or with one square
  (`sum(power(x - target, 2))`). Each row's value is its best value inside its range.
- **Fill one budget by value per unit.** Per-row `REAL` decisions with bounds of their own and one budget
  `sum(w * x) <= B` (or `>=`, `=`, `BETWEEN`), maximizing or minimizing a linear `sum(v * x)`.

A count limit or a budget with a `by` key, or with a `per K:` in front, takes the solver path for now.

```sql
-- Answered directly: pick the best three rows.
SELECT id, x FROM items DECIDE x(BOOL)
SUCH THAT sum(x) <= 3
MAXIMIZE sum(score * x);

-- Answered directly: each row gets the value closest to its target that its capacity allows.
SELECT id, x FROM items DECIDE x(REAL)
SUCH THAT x BETWEEN 0 AND capacity
MINIMIZE sum(power(x - target, 2));

-- Answered directly: fill the budget with the best value per unit of weight first.
SELECT id, x FROM items DECIDE x(REAL)
SUCH THAT x BETWEEN 0 AND capacity AND sum(w * x) <= 4
MAXIMIZE sum(score * x);
```

What you may notice:

- When several assignments are equally optimal, you can get a different one than the solver
  would have chosen. The objective value is the same.
- A strict bound against a fraction on an `INT` or `BOOL` (`x > 0.5` with no `when`) is read as
  `x >= 1` on both paths.
- `EXPLAIN` of a query it answers shows a window plan with `Direct solve` rows naming the
  rule. `EXPLAIN` of every other DECIDE query is exactly as before.
- Errors (infeasible, NULL bound, a `when` that keeps no row) are raised in the same situations and worded
  the same way where possible; the text is not guaranteed identical.

Controls:

| setting | effect |
| --- | --- |
| `SET decide_direct_solve = 'auto'` | the default: answer directly when proved, otherwise use the solver |
| `SET decide_direct_solve = 'off'` | always use the solver |
| `SET decide_direct_solve = 'require'` | raise an error when no rule applies, instead of solving |

`DIAGNOSE` always uses the solver. A prepared statement keeps the choice it was planned
with until it is re-planned.
