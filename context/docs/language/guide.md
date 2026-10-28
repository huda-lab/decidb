# The DECIDE language

What a user can write, and what it means. This is the single source for syntax; the other files in this folder
explain how each part is built. Every claim here describes current behavior; where code and this file disagree, the
code wins and this file is fixed. Messages are in [errors.md](errors.md); shapes the language refuses or does not have
yet are in [errors.md §2](errors.md#2-not-supported-yet) and [roadmap.md](roadmap.md).

Section numbers are stable: code and tests cite them (`guide.md §6.4`).

## 1. The DECIDE clause

Two clause orders parse to the same plan:

```sql
-- Split order (the paper's): declaration between SELECT and FROM
SELECT ...
DECIDE [per key:] name(type) [, ...]
FROM ... [JOIN ...] [WHERE ...]
SUCH THAT [scope:] constraint [AND [scope:] constraint ...]
[MAXIMIZE | MINIMIZE [scope:] objective]

-- Single-block order: everything after WHERE
SELECT ... FROM ... [WHERE ...]
DECIDE [per key:] name(type) [, ...]
SUCH THAT ...
[MAXIMIZE | MINIMIZE ...]
```

A declaration sits in one position, never both. A `DECIDE` with no `SUCH THAT`, or a `SUCH THAT` with no `DECIDE`,
is a parse error. Omitting the objective makes a feasibility problem (both solvers).

### 1.1 Scopes: `per`, `when`, `by`

Three keywords, three separate jobs:

| Keyword | Position | Job |
|---|---|---|
| `per K` | in front of a declarator, a constraint, the objective or a reducer's argument | how many decisions, constraints or reducer inputs exist: one per class of `K` |
| `when c` | in front of a constraint, the objective or a reducer's argument | which rows are looked at: those where `c` is true |
| `by (Γ)` | after a reducer | which rows that one reducer reads: the kept rows sharing the current value of `Γ` |

`per` never selects the rows a reducer reads, and `by` never changes how many constraints exist (§5.4). They are
separate because one constraint often needs both: "one constraint per depot, comparing that depot's reserve with its
region's demand" generates per depot but reads per region (deck example 6, §6.5).

Terms:

- **Reducer**: `SUM`, `AVG`, `MIN` or `MAX` over rows (`norm` and `count(*)` take a `by` too).
- **Tuple term**: any part of a clause outside a reducer — a column, a decision, arithmetic over them.
- **Key / class**: a key lists columns and relations (for `by`, also expressions); rows with equal key values form one
  class.
- **Kept rows**: the result rows (after joins and `WHERE`) that pass the enclosing `when`.
- **Determines**: `K` determines a value when every row of each `K`-class carries the same value (§7.1).

```
decide_list ::= declarator (',' declarator)*
declarator  ::= ['per' key ':'] name '(' INT | BOOL | REAL ')'
constraint  ::= [scope ':'] comparison             -- AND separates constraints; a scope covers one
objective   ::= (MAXIMIZE | MINIMIZE) [scope ':'] expression     -- per must be '()' or absent
scope       ::= ['when' condition] ['per' key]                   -- at least one of the two
reducer     ::= agg '(' [scope ':'] expression ')' ['by' '(' [expression, ...] ')']
              | agg '(' relation, ... ':' expression ')' ['by' ...]   -- the colon form, §5.1
agg         ::= SUM | AVG | MIN | MAX              -- norm(e, p) and count(*) take a by, no scope
key         ::= '(' ')'  |  'row'  |  element (',' element)*
element     ::= column | relation                  -- a relation stands for all its stored columns
```

| Omitted | Means |
|---|---|
| `per` on a declarator, a constraint, or inside a reducer | `per row` |
| `per` on the objective | `per ()` |
| `by` | `by ()`: all kept rows |
| `when` | every row |

`per row` written out means exactly the default. Tests: `test/decidb/test_scope_parser.test`.

### 1.2 Words that are keywords inside the clause

Inside a DECIDE clause — the declaration list and everything from `SUCH THAT` to the end of the objective, subqueries
written there included — `when`, `per`, a `by` directly after `)` and a `row` directly after `per` are keywords. A
column with such a name is quoted there: `x <= "per"`. Outside the clause nothing changes, and `GROUP BY`,
`ORDER BY` and `PARTITION BY` in a subquery inside the clause are untouched. `scalar` is an ordinary name. A scope
inside an ordinary SQL subquery written in the clause is refused.

## 2. Decision variables

- Declared in the `DECIDE` list with a **mandatory** type: `x(INT)`, `x(BOOL)` or `x(REAL)`.
- Referenced by the **bare name** in `SUCH THAT`, the objective, the `SELECT` list and `ORDER BY`; `D.x` is refused. The deck allows a qualifier
  ([roadmap](roadmap.md#a-qualified-decision-name-p-7)).
- The declaration's **key** sets how many solver columns it yields:

  | Spelling | Scope | Columns |
  |---|---|---|
  | `x(INT)`, `per row: x(INT)` | per row (default) | one per result row (§2.1) |
  | `per K: x(INT)` | keyed | one per class of `K` among the result rows (§2.3) |
  | `per (): x(INT)` | query-wide | exactly one (§2.2) |

- **Domains.** `BOOL` is `{0, 1}`; `INT` is `{0, 1, 2, …}` and `REAL` is `[0, ∞)` by default.
- **Result types.** `INT` returns `BIGINT`, `BOOL` returns `INTEGER` (0/1), `REAL` returns `DOUBLE`. `INT` is 64-bit
  because nothing holds a decision to a narrower range: the bound that pins it may be a column or an aggregate row, so
  a 32-bit column could truncate a legitimate optimum. A whole-number answer beyond 2^53 is refused rather than
  rounded.
- **Signed variables.** 0 is a *default* lower bound for `INT` and `REAL`, not a floor. A variable becomes signed when
  the query bounds it alone below zero, read by value: `x >= -K` in any affine spelling with constants known before
  the query runs (`x - 1 >= -3`, `2 * x >= -4`, `-x <= 2`), `x BETWEEN -K AND K`, an easy `min(x) >= -K` with no `when`
  and no filter of its own (§5), or a negative literal in an `IN` domain. A bound whose other side is data
  (`x >= w - 4`) is a row, not a floor. A bound under a `when` does not lower the floor (an open bug,
  `../issues/bugs.md`). There is no free (−∞) domain: a two-sided-unbounded variable is the case most likely to
  make a model unbounded, so a signed variable always has a finite lower bound.

```sql
DECIDE x(BOOL), y(INT), z(REAL)
DECIDE adj(REAL)   -- ... SUCH THAT adj BETWEEN -10 AND 10   -> adj in [-10, 10]
```

### 2.1 One decision per row (default)

A declarator without `per` makes one decision per **result row**. Over a join, each joined row gets its own decision.

**Reducers count result rows**, whatever the decision's key, as SQL does over a join. If a nurse appears in 5 rows
(one per shift), an unscoped `sum` reads her decision 5 times. To count each class once, give the reducer its own
`per` (§5.3) or use the colon form (§5.1):

```sql
SELECT n.name, s.shift_date, keepN
FROM nurses n JOIN shifts s ON n.id = s.nurse_id
DECIDE per n: keepN(BOOL)
SUCH THAT sum(keepN * s.hours) <= 100             -- one term per shift row
MAXIMIZE sum(per n: keepN * n.skill_score)        -- each nurse's score once
```

### 2.2 Query-wide — `per (): x(TYPE)`

One decision for the whole query, whatever the input size.

```sql
-- The paper's Example 1: no region falls short of demand by more than max_shortfall.
SELECT r.routeID, r.regionID, ship, max_shortfall
FROM routes r
DECIDE ship(INT), per (): max_shortfall(INT)
SUCH THAT ship <= r.capacity AND sum(ship) <= 40
      AND per r.regionID: r.demand - sum(ship) by (r.regionID) <= max_shortfall
MINIMIZE max_shortfall
```

- **Output**: the value repeats on every row.
- **In constraints**: compared against row data (`ship <= max_shortfall`) it makes one row per kept row, all
  referencing the same column. It is determined by every key (§7.1), so it may stand beside a reducer or be its bound
  under any `per`, and it counts once.
- **Inside a reducer**: a body of query-wide decisions only (`sum(cap)`, `sum(cap1 + cap2)`) is refused as a
  constraint's whole decision side. There is one column and nothing to reduce, and the two plausible readings
  (coefficient 1 or `n`) are different problems, so neither is chosen silently; write `cap`. A body mixing it with row
  data is ordinary scalar-times-vector: `sum(cost * cap)` is `(Σ cost) · cap`, and `sum(x + cap)` is
  `sum(x) + n · cap`, where `n` is the number of values the reducer reads.
- **In the objective**: bare (`MINIMIZE max_shortfall`), as an additive term counted once
  (`MINIMIZE max_shortfall - sum(ship)`), or inside a reducer with row data (`sum(cost * cap)`).

Tests: `test/decide/tests/test_scalar_scope.py`.

### 2.3 Keyed — `per K: x(TYPE)`

```sql
DECIDE per D.depotID: reserve(REAL)              -- one per distinct depotID
DECIDE per D: open(BOOL)                         -- one per distinct tuple of D
DECIDE per D.region, S.shipmentID: alloc(INT)    -- one per distinct pair, across relations
DECIDE ship(INT), per D: open(BOOL), per (): cap(INT)   -- each declarator its own key
```

- **Key elements** are columns and relations of the `FROM` clause, spelled as anywhere in the query (`region`,
  `D.region`, `D`). An unqualified `USING` column is the kept side's. A relation stands for all its stored columns;
  generated columns are left out, since they only repeat the columns they are computed from. Expressions, decisions,
  `rowid` and generated columns are not key elements; expressions belong in `by`.
- **A key is a set**: `per a, b` and `per b, a` are one key, and declarators with equal keys share one grouping.
- **Each declarator has its own key**: `per D: open(BOOL), ship(INT)` leaves `ship` per row.
- **Count**: one decision per distinct key value among the result rows. A row whose written key column is NULL is in
  no class and has no decision (D5): it shows NULL, a reducer skips it, and an unreduced use skips that instance. A
  relation element compares NULL as a value: `per D` gives a NULL in D's columns a class.
- **Output**: every row shows its class's value; a row with no decision shows NULL.

Tests: `test/decide/tests/test_scope_declare.py`.

## 3. Constraints

Constraints are joined by `AND`. A comma between them is a parse error, and `OR` between them is refused: every
constraint must hold, and a choice between alternatives is a `BOOL` decision. `OR` inside a `when` or a subquery is
data and unaffected.

- **Operators**: `=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN` (two constraints under one scope), `IN`.
- **Strict `<`, `>` and `<>` need a whole-numbered side.** `LHS < K` becomes `LHS <= ceil(K) - 1`, and `<>` becomes
  `LHS <= K - 1 OR LHS >= K + 1`; both are exact only on integers. The rule is checked on **declared types** at bind
  time: every decision is `INT`/`BOOL` and every column, literal and reducer has an integer type or `DECIMAL(p, 0)`.
  `DOUBLE` and `DECIMAL(p, s>0)` fail whatever values they hold, so inserting one fractional row can never make a
  working query illegal. Not refused: the bound itself (rounded; for `<>` a fractional bound excludes nothing and is
  dropped), a decision-free offset, `POWER(e, n)` with whole `n`, and an L0 count. A fractional multiplier
  (`SUM(0.5 * x) < 5`) is refused, and so is an offset under anything non-linear (`POWER(x + 0.25, 2) < 11`). The
  message names the offender and the cast that fixes it.
- **`<>`** works per row and on reducers. Its cost depends on where `K` falls in the left side's reachable range:
  interior costs one binary and two Big-M rows; a range wholly on one side collapses to one plain inequality
  (`SUM(x) <> 0` over non-negative decisions is `SUM(x) >= 1`); an unreachable `K` drops the constraint. Only a
  variable's intrinsic domain licenses the collapse, so a bound you wrote stays loosenable under `DIAGNOSE`. On Gurobi
  `<>` is two indicator constraints, with no Big-M. `AVG(x) <> K` becomes `SUM(x) <> K·n`.
- **`IN`** on a column is a filter; on a decision it restricts the domain (K binary indicators). `IN` on a reducer is
  not supported.
- **Degree**: linear terms, products of two different decisions (§4) and `POWER(linear, 2)` are allowed; a product of
    three or more decisions is refused.
- **Data operands**: any operand that reads no decision is data, evaluated per row as SQL evaluates it: a comparison
  as 0/1 (`x <= 4 * CAST(cap > 2 AS INT)`), a lambda (`SUM(x * list_reduce([cap, b], (a, z) -> a + z))`), a prepared
  statement's `$1`, a table macro's parameter. `CASE` is the exception and is refused everywhere. A comparison that
  reads a decision, used as a number (`x <= (x > 1)`), is refused: *"SUCH THAT does not support '(x > 1)'"*.
- **Subqueries**: scalar subqueries, uncorrelated or correlated, may be bounds or factors. A correlated one becomes a
  per-row value and follows the bound rule of §5. A subquery cannot reference the enclosing query's decisions.
- **Nested DECIDE**: a scalar bound subquery may hold its own `DECIDE`; it solves first and its one-row result is the
  outer bound. A refusal of the inner query reaches the user as its own message.
- **Outer queries**: a DECIDE solves one problem, so it cannot read a column of an outer query (a `LATERAL` subquery,
  or a scalar subquery in another query's select list), in any clause or its `WHERE`. The message names the column
  and points to joining that table into the FROM and writing the clause with `per`; when the column sits inside a
  reducer, it quotes the reducer.
- **NULL**: a NULL data value is skipped where it is read, never read as a zero (deck pp. 50, 56). A reducer skips a
  row whose summand is NULL, as SQL does: `SUM(w * x)` leaves out the rows where `w` is NULL, `SUM(a*x + b*y)` drops
  the row's whole summand when `a` is NULL, and `AVG(w * x)` divides by the rows it kept. A NULL that reaches a
  constraint unreduced (a bound, a coefficient outside a reducer, a factor on an aggregate) skips that instance, so
  `SUM(x) <= cap` with a NULL `cap` has no constraint there. In the objective, a NULL term is dropped. `COALESCE`
  imputes a value instead: `SUM(x * COALESCE(w, 0))`. A NULL written in the query beside a decision
  (`SUM(x + NULL)`) is refused where it is written, and NaN is an error. `COUNT(col)` also counts a NaN, as SQL does.
  In a `when`, NULL is false; in a written key column, NULL is no class (§7.2).

### 3.1 Quadratic constraints

```sql
SUCH THAT POWER(x - target, 2) <= 9                                     -- per row
SUCH THAT SUM(POWER(x - target, 2)) <= 1000                             -- total
SUCH THAT per department: SUM(POWER(x - target, 2)) by (department) <= 50
-- POWER(e, 2), e ** 2 and (e) * (e) are the same; -POWER(e, 2) and K * POWER(e, 2) work
```

Gurobi only; HiGHS has no quadratic constraints, and the query is refused at plan time.

### 3.2 Numeric precision and casts

A DECIDE model carries **one numeric domain, `DOUBLE`** (about 15 significant digits): the solver interface and both
backends take doubles, so every value ends up there.

- **No casts on decisions.** `CAST`, `TRY_CAST` and `::` over an expression holding a decision are refused in
  `SUCH THAT` and the objective, whatever the target. Decisions already enter as `DOUBLE`, and any other target means
  rounding the solver cannot model. Cast the data instead.
- **Data casts keep DuckDB semantics.** `x <= CAST(1.6 AS INTEGER)` has bound 2. DuckDB evaluates the typed data
  expression first; only its result becomes `DOUBLE`.
- **Binder-inserted casts are transparent**; they never change the model. Ordinary SQL, and the `SELECT` list after
  the solve, are unaffected.
- **Beyond 2^53**, a DECIDE model and row-by-row SQL can disagree (SQL rounds each side, DeciDB folds the bound once).
  Pinned by `test_canonicalize_cast.py::test_magnitudes_beyond_double_domain_are_a_documented_limit`.

## 4. Objective

`MAXIMIZE | MINIMIZE [when c] [per ()]: E`

```sql
MAXIMIZE sum(ship)                                         -- produced once: per () implied
MAXIMIZE when S.priority: sum(ship)                        -- over the priority rows
MINIMIZE sum(S.unit_cost * ship) + sum(per D: D.opening_cost * open)
MINIMIZE max(per D.depotID: sum(ship) by (D.depotID))      -- two-level form
```

- **Produced once.** No `per` means `per ()`; any other `per` is a parse error.
- **`when c`** filters the rows for the whole objective; one that keeps no row is an error (§7.3).
- **Body**: reducers (`SUM`, `AVG`, `MIN`, `MAX`, `norm`), decisions outside them, and sums of those, with at least one
  decision.
  - A decision outside the reducers is legal when it has one instance over the kept rows, and counts once: any
    `per ()` decision, or a per-row or keyed one when the `when` keeps one row or class (`MAXIMIZE when id = 3: x`).
    Otherwise it is the determination error. Not beside a `MIN` or `MAX`.
  - A product of two query-wide decisions counts once (Gurobi). Any other product of two decisions outside a reducer
    is refused unless one is `BOOL`, which is linearized first.
  - A factor on a reducer must be one value over the kept rows (§5).
- **`by` on a top-level reducer** must be one group over the kept rows: `by ()`, or a `Γ` with one value there
  (`MAXIMIZE when g = 'A': sum(x) by (g)`).
- **Two-level form** `outer(per k: inner(e) by (k))`, the whole objective: `inner` once per class of `k` (NULL a
  class), then `outer` over those values. All nine `SUM`/`MIN`/`MAX` pairs; `k` may list several columns. It takes a
  `when` in front, not inside; the inner reducer takes no `per`; the `by` key must be the `per` key; an outer `AVG` is
  refused, since `sum` over the classes ranks solutions the same way.
- **Quadratic objectives**: `MINIMIZE SUM(POWER(linear, 2))` is convex QP on both solvers (HiGHS only when continuous
  and the Q matrix is non-singular). `-POWER(...)` under `MAXIMIZE` is concave QP on both. `MAXIMIZE SUM(POWER(...))`
  is non-convex, Gurobi only. Integer variables with a quadratic objective (MIQP) need Gurobi.
- **A square or product under `MIN`/`MAX`**, in the objective or a constraint (`MINIMIZE MAX(POWER(x - t, 2))`,
  `SUM(x) + MAX(POWER(x, 2)) <= 20`): Gurobi only ([problem_types.md](problem_types.md)).
- **Bilinear (`x * y`)**, in objectives and constraints: with one `BOOL` factor it is linearized exactly (McCormick;
  both solvers; the other factor needs a finite upper bound, written or inferred, §5). Otherwise it is non-convex,
  Gurobi only. Data coefficients work (`SUM(profit * b * x)`); three-factor products are refused.

Tests: `test/decide/tests/test_scope_objective.py`. Problem classes and solver support: [problem_types.md](problem_types.md).

## 5. Reducers

- **`SUM`** is the primary reducer. Data-only additive terms inside it (`SUM(x + cost)`, `SUM(q * (price + x))`) are
  evaluated over the same rows and moved to the bound.
- **`AVG(e)`** is `SUM(e)` divided by `n`, the number of values the reducer receives: its `by` group's kept rows that
  pass its own `when`, or the classes of its own `per`. In a constraint `AVG(e) op K` is `SUM(e) op K·n`.
- **`MIN` and `MAX`**. The *easy* direction (`MAX(e) <= K`, `MIN(e) >= K`) bounds each member, with no auxiliary; with
  no `when` and no filter, a member that is a lone decision is bounded in its column box, so `min(x) >= -5` makes `x`
  signed (§2). The *hard* direction (the opposite, or `=`) adds an extremum column per `by` group, pinned by Big-M
  indicator rows (or a native general constraint on Gurobi). In the objective, `MINIMIZE MAX` and `MAXIMIZE MIN` take
  one auxiliary; `MAXIMIZE MAX` and `MINIMIZE MIN` add indicators. A lowered hard case needs every member bounded,
  written or inferred, because no finite Big-M dominates an unbounded range.
- **A reducer's own filter**: `sum(when morning: x * hours) + sum(when evening: x * hours) <= 40` (§5.3).
- **A factor on a reducer** works for every reducer, on either side: `2 * SUM(x*p)`, `SUM(x*p) / 2`, `2 * MAX(x*v)`,
  `SUM(x) + 2 * MAX(x*v)`. A negative factor reverses the relation the reducer faces (`-2 * MAX(e) >= -8` is
  `MAX(e) <= 4`), and `-2 * max(e)` is `min(-2 * e)`.
  - In a **constraint** the factor may be any decision-free term: a literal, an uncorrelated subquery, a column or a
    correlated subquery. It is a tuple term: under `per` the key must determine it, and a row-varying one gives the
    helper-decision shape (§6.4).
  - In the **objective** it must be one value over the kept rows; a row-varying one is refused with a pointer to
    `SUM(x * weight)`.
  - A **decision** as the factor is refused, even a query-wide one: `s * sum(x)` is written `sum(s * x)`, a bilinear
    term.
  - A **data-only reducer** may not share a scaled term with a decision (`x * avg(cap)`, `sum(x) / sum(cap)`); compute
    it as an uncorrelated subquery instead.
- **Either side may carry the decision.** `5 >= x` and `x <= 5` are one constraint, and a reducer may sit on both
  sides (`SUM(x*v) <= SUM(y*v)`). A query-wide decision may be an additive term (`sum(ship) - max_shortfall <= cap`)
  or the bound; a per-row or keyed decision beside a reducer gives the helper-decision shape (§6.4).
- **The bound rule.** A side with no decision must give one value per generated constraint. With no `per`, a value
  that varies by row reduces to its tightest value within each `by` group of the kept rows (MIN for `<=`/`<`, MAX for
  `>=`/`>`) — a bare column as well as a subquery (`sum(ship) by (depotID) <= stock`, the paper's Figure 1). Under
  `per` the key must determine it instead (§7.1). `=` refuses a bound that genuinely varies, since two different
  numbers cannot both be *the* sum. `<>` keeps every value as its own exclusion.
- **`norm(e, p)`** is an L_p term with a user-supplied weight (`MINIMIZE SUM(cost*x) + 0.5 * norm(x - base, 1)`). It
  takes a `when`/`per` in front of its clause and a `by`, but no scope inside and no colon form:

  | `p` | Means | Class |
  |---|---|---|
  | `1` | `SUM(ABS(e))` | LP |
  | `2` | `SUM(POWER(e, 2))` — squared L2 | convex QP |
  | `'inf'` | `MAX(ABS(e))` | LP |
  | `0[, M]` | exact count of rows where `|e| >= tolerance` | MILP |

  The L0 count is exact in every direction (cap, floor, `=`, `MAXIMIZE`), because each row's indicator is pinned both
  ways. `M` is inferred from bounds when omitted; `e` must have finite bounds. The tolerance defaults to `1e-4`
  (`SET decide_l0_tolerance`); a value forced strictly between 0 and the tolerance is infeasible. Over data only,
  `norm(e, 0)` is a plain count of nonzero rows.
- **Other scalar functions over a decision** (`sqrt`, `exp`, `ln`, `floor`, …) are refused: they would make the model
  non-linear. Only `ABS` and `POWER(…, 2)` wrap a decision. Any function over data only is a per-row coefficient.

### 5.1 The colon form — `SUM(D: e)`

`agg(Rel[, Rel, ...]: e) [by (Γ)]` means `agg(per Rel[, ...]: e)` (§5.3): the reducer reads one term per tuple of the
named relations instead of one per result row.

```sql
SELECT routeID, depotID, open, ship
DECIDE per D: open(BOOL), ship(INT)
FROM Depots D JOIN Routes T USING (depotID)
SUCH THAT ship <= capacity * open AND SUM(ship) >= 300
MINIMIZE SUM(unit_cost * ship) + SUM(D: opening_cost * open);   -- each depot's cost once
```

- **Identity is the tuple, not the value**: two depots with equal cost are two terms (this is not `SUM(DISTINCT …)`).
- **Scope is the surviving rows**: join, `WHERE` and `when` decide which tuples contribute.
- **The named relations must determine the body**, checked on the data every run (§7.1). A column of an unnamed
  relation stops the query rather than picking an arbitrary row.
- **`AVG(D: e)`** divides by the number of classes. **`MIN`/`MAX`** are unaffected.
- **Several relations** (`SUM(D, T: e)`): the identity is the concatenation of their keys. Naming a relation removes
  its fan-out; an unnamed one keeps its fan-out. Naming every joined relation is a no-op. Order does not matter.

### 5.2 `by (Γ)` — which rows a reducer reads

```sql
sum(ship)                                                 -- by (): all kept rows
sum(ship) by (D.depotID)                                  -- rows of the current depot
sum(ship) by (D.depotID, S.dispatchDay + S.transitDays)   -- an expression in the key
```

- Applies to `SUM`, `AVG`, `MIN`, `MAX`, `norm` and `count(*)`, on either side, with or without a decision inside.
- `Γ` lists columns, relations and data expressions; never a decision. A key with one value everywhere (`by (1)`) is
  `by ()`.
- Reads every kept row whose `Γ` equals the current row's, including rows outside the current class (§6.1).
- A row whose `Γ` is NULL is in no group of it (D5): that reducer has no value there, and the NULL skips the instance;
  the clause's other reducers still read the row. `Γ` must be determined by the enclosing `per` key (§7.1).
- `by` belongs to the call right before it. A data-only reducer on the bound side reads its own groups:
  `count(*) by (g)` counts the current row's group.
- A factor holds the reducer's value: `-2 * max(e) by (g)` is `min(-2 * e) by (g)`.
- `max(x) by (g) > cap` is `>= cap + 1`; so a strict bound refuses a `MIN`/`MAX` whose value can be fractional.

Tests: `test/decide/tests/test_scope_reducer_by.py`.

### 5.3 `when` and `per` inside a reducer — `agg([when c] [per K]: e)`

```sql
sum(when S.priority: ship)                          -- only priority rows
sum(per D: D.opening_cost * open)                   -- each D tuple once
sum(when S.priority per D: D.opening_cost * open)   -- filter, then each remaining tuple once
```

- **Order**: the rows `by` selects → the inner `when` → one value per class of the inner `per` (read at its first
  kept row) → the aggregate.
- **Inner `when`** filters this reducer only. A `when` in front of the clause applies first.
- **Inner `per K'`** hands the reducer one value per class, so a tuple repeated by a join counts once. `K'` must
  determine the body.
- **A reducer with no row in a group is NULL** (D68), so that group's constraint is skipped; the other reducers of
  the clause do not save it. Written in front, the condition leaves the same depots out:

```sql
per D.depotID: sum(when S.priority: ship) by (D.depotID) <= D.priorityCapacity    -- a depot with none: no constraint
when S.priority per D.depotID: sum(ship) by (D.depotID) <= D.priorityCapacity     -- the same depots get none
```

Tests: `test/decide/tests/test_scope_reducer_inner.py`.

### 5.4 `per` and `by` are independent

| Written | Constraints | Each reducer reads |
|---|---|---|
| `ship <= 0.20 * sum(ship)` | one per row | all rows |
| `per (): sum(ship) <= networkCapacity` | one | all rows |
| `per D.depotID: reserve >= D.share * sum(S.demand)` | one per depot | all rows |
| `per D.depotID: sum(ship) by (D.depotID) <= D.capacity` | one per depot | that depot's rows |
| `ship <= D.maxShare * sum(ship) by (D.depotID)` | one per row | that row's depot |

## 6. Scoped constraints — `[when c] [per K]: C`

```sql
SUCH THAT
    when S.priority: ship >= 1 AND
    per D.depotID: sum(ship) by (D.depotID) <= D.capacity AND
    per (): sum(when S.priority per D: D.opening_cost * open) <= 50
```

`when` comes before `per`, and the colon is required when either is present. The same form scopes the objective and a
reducer's argument. Tests: `test_scope_generation.py`, `test_scope_combinations.py`, `test_scope_bare_clauses.py`.

### 6.1 Evaluation

1. **Filter.** Keep the rows where `c` is true.
2. **Generate.** Split the kept rows into classes by `K`; the body is produced once per class.
3. **Aggregate.** A reducer `agg(...) by (Γ)` reads all kept rows whose `Γ` equals the class's, not only the class's
   own rows.
4. **Well-defined.** `K` must determine every tuple term and every `Γ` (§7.1).

```sql
--   shipment  depot  region  ship  capacity
--   S1        D1     North   x1    100
--   S2        D1     North   x2    100
--   S3        D2     North   x3     80
--   S4        D3     South   x4     50
per D.depotID: sum(ship) by (D.region) <= D.capacity
--   D1: x1 + x2 + x3 <= 100    D2: x1 + x2 + x3 <= 80    D3: x4 <= 50
```

### 6.2 `per K` — how many constraints

- `per K:` makes one constraint per class among the kept rows; no `per` (or `per row:`) one per kept row; `per ():`
  exactly one.
- **One scope, one constraint**: in `per g: A AND B` only `A` is scoped.
- Key elements are columns and relations, as in §2.3. Several columns are written without parentheses (`per a, b:`).
- **A NULL key is no class** (D5): a row whose written `per` column is NULL gets no constraint from the clause. A
  relation's columns compare NULL as a value.
- **A key groups under its collation** (D69), as `GROUP BY` does: over a `COLLATE NOCASE` column, 'a' and 'A' are one
  class, one `by` group and one keyed decision, and a message names the group by its least spelling ('A'). Plain text
  compares case-sensitively.
- **`per` does not group reducers** (D6): `per g: sum(x) <= 10` bounds the total over all kept rows, once per `g`
  (identical rows collapse to one). Write `per g: sum(x) by (g) <= 10` for each group's own total.
- **An explicit `per` must determine the body**; no `per` always does. So `sum(x) <= cap` takes the tightest `cap`
  (the bound rule, §5), while under `per g:` the same clause requires `g` to determine `cap`.

### 6.3 `when c` — which rows

- The condition is any boolean over data, ended by `per` or `:`; an `and` before that end belongs to it, so no
  parentheses are needed. It may not mention a decision, a reducer or a window function.
- NULL counts as false.
- **Filtering comes first**: classes form from kept rows only, and every reducer in the body reads kept rows only.
- **Nothing is computed on a dropped row**: a NULL there is no error, and an expression that would fail there
  (`ln(0)`, a bad cast) is never evaluated — in the clause, its keys, bounds, checks, or rows a rewrite adds. The
  parts of an `and`/`or` are read left to right, each only on rows the earlier parts leave undecided, so
  `when s <> '' and CAST(s AS INT) > 3:` casts no empty text.
- Dropped rows keep their decisions; the clause just does not constrain them.
- **A `when` that keeps no row yields no constraint** (D68). An input with no rows at all gives an empty result.

### 6.4 How a clause becomes model rows

Tuple terms and reducers mix freely on either side. Each comparison takes one of three shapes:

- **Rows** — no reducer on the decision side: one model row per class, from its first kept row.
- **Grouped** — decision reducers sharing one `by` key, under whole-query factors, beside query-wide terms only: one
  model row per `by` group, under the group's tightest bound (`=` refuses differing bounds; `<>` excludes each).
- **Helper decision** — anything else: a per-row or keyed decision beside a reducer (`ship <= 0.20 * sum(ship)`, the
  facility link `sum(ship) by (D.depotID) <= D.capacity * open`), a row-varying factor, reducers with different `by`
  keys, a reducer inside another expression (`abs(sum(ship) by (g) - 40) <= 5`). Each reducer becomes a **helper
  decision** with one instance per `by` group, defined once (an equality row for `SUM`/`AVG`, an extremum link for
  `MIN`/`MAX`), and the clause is one row per class. Example 1 costs N rows of two entries plus one definition row,
  not N rows of N entries. Clauses holding the same reducer over the same rows share one helper and one definition,
  which belongs to none of them: a `DIAGNOSE` that drops one leaves the definition the others read. The helper is whole-numbered when the reducer's value is, so `<`, `>`, `<>` follow §3. A
  product or square of decisions inside such a reducer is refused.

`EXPLAIN` names a helper by its reducer; `DIAGNOSE` names the clause and never a definition row. Tests:
`test/decide/tests/test_scope_value_rewrite.py`.

### 6.5 Deck examples 1–8

From the language proposal (`context/New_syntax.pdf`, pp. 17–24). All run as written and fully explicit, over
`FROM Shipment S JOIN Depot D USING (depotID)` with `DECIDE ship(INT), per D.depotID: reserve(REAL)`:

| # | As written | Constraints | Reducer reads | Must be determined |
|---|---|---|---|---|
| 1 | `ship <= 0.20 * sum(ship)` | one per shipment | all rows | — |
| 2 | `per (): sum(ship) <= networkCapacity` | one | all rows | `networkCapacity` |
| 3 | `per D.depotID: reserve >= D.networkReserveShare * sum(S.demand)` | one per depot | all rows | `reserve`, the share |
| 4 | `per D.depotID: sum(ship) by (D.depotID) <= D.capacity` | one per depot | the depot's rows | `D.capacity` |
| 5 | `ship <= D.maxShipmentShare * sum(ship) by (D.depotID)` | one per shipment | its depot's rows | — |
| 6 | `per D.depotID: reserve >= 0.10 * sum(S.demand) by (D.region) + 0.02 * sum(S.demand) by (D.country)` | one per depot | region; country | `reserve`, region, country |
| 7 | `per D.depotID: sum(when S.priority: ship) by (D.depotID) <= D.priorityCapacity` | one per depot | its priority rows | `D.priorityCapacity` |
| 8 | `per S.shipmentID: ship <= 0.25 * sum(ship) by (D.depotID, S.dispatchDay + S.transitDays)` | one per shipment | same depot and arrival day | `ship`, the key |

Example 7 is an error on data where a depot has no priority shipment (§5.3). Example 9 (nested reducers) is not in the
language yet ([roadmap.md](roadmap.md)). Tests: `test/decide/tests/test_scope_deck_examples.py`.

## 7. Determination, NULL and empty sets

### 7.1 The determination rule

A term must have one value per generated constraint, so a `per` key must determine everything its body reads once per
class: every tuple term (the bound and any factor included), every decision outside reducers, and every reducer's `by`
key. An inner `per` key and a colon form's relations must likewise determine their reducer's body.

- A **keyed** decision is determined when each `K`-class lies in one class of its own key; a **per-row** one only when
  each `K`-class is one row; a **query-wide** one always.
- Literals, uncorrelated subqueries and anything under `per row` are always determined.
- `per ()`, and the objective, need each tuple term to have one value over all kept rows.
- **Checked on the data**, every run, before solving, over kept rows only — so one query can pass on one dataset and
  fail on another. A static check would have to refuse queries that are fine on the data they run on.
- NULL counts as one value here.

```sql
per D.depotID: sum(ship) by (D.region) <= 500    -- holds: each depot has one region
per D.depotID: sum(ship) by (S.region) <= 500    -- fails if a depot ships to two regions
per D.depotID: ship <= 10                        -- fails: ship is per row, a depot has several rows
```

Tests: `test/decide/tests/test_scope_determination.py`.

### 7.2 NULL and empty sets

| Situation | Outcome |
|---|---|
| NULL in a declaration's, constraint's or `by` key column | no class or group of that key (D5); a relation's columns compare NULL as a value |
| a keyed decision on a NULL-key row | no decision: the row shows NULL, a reducer skips it, an unreduced use skips the instance |
| NULL `when` condition | row dropped |
| NULL in a reducer's summand | the row is skipped, as in SQL; AVG divides by the rows kept (§3) |
| NULL bound, unreduced coefficient or factor on an aggregate | the instance is skipped (§3) |
| NULL in an objective term | the term is dropped (§3) |
| NULL or NaN written in the query beside a decision; NaN in data | error (§3) |
| a `when` keeps no row | no constraint (D68) |
| a key value has no kept row | no class, no constraint |
| a reducer reads no row of a group (its own `when`, NULL data, an absent decision) | the reducer is NULL: that group's instance is skipped (D68) |
| an objective reducer reads no row | the term is dropped (D68) |
| `count(*)` over no rows | 0, as in SQL |
| the input has no rows | empty result |

### 7.3 Data checks

Run before solving, on every run, also under `DIAGNOSE`:

| Check | Message |
|---|---|
| A `per` key determines what it must (§7.1) | `DECIDE: per D.depotID does not determine D.capacity. D.depotID = 'D1' has more than one value: 25, 40.` · for a decision: `... covers more than one ship.` · context: `(in by (S.region))`, `(in sum(D: ...))` · for `per ()`: `The kept rows have more than one value: 3, 5.` |

The phrase `does not determine` is fixed; names and values come from the query and the data. Over a join a column is named with its relation.

## 8. `DIAGNOSE`

`DIAGNOSE` prefixes a `SELECT` that carries a `DECIDE` clause. It runs the query and reports on the run instead of
returning rows, as `EXPLAIN ANALYZE` does for an ordinary query. **It is the only thing that starts the diagnostics
engine**, so a query without it never pays for a diagnostic solve. Without the prefix a failed query states its state
and stops: *"DECIDE optimization is infeasible. Prefix the query with DIAGNOSE to see which clause to change."*

```sql
DIAGNOSE SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= 5 AND x >= 8 MAXIMIZE SUM(x);
SELECT clause, suggested_change FROM (DIAGNOSE SELECT ...) WHERE amount > 1000;
```

### 8.1 What it returns

A relation, one row per finding; a query that solves returns one row, `state = 'feasible'`, everything else NULL.

| column | type | holds |
|---|---|---|
| `state` | VARCHAR | `infeasible`, `unbounded` or `feasible` |
| `clause` | VARCHAR | the clause as written (echo style, §9), or the runaway decision's name |
| `suggested_change` | VARCHAR | the smallest edit that addresses this finding |
| `amount` | DOUBLE | how far a bound moves, escaping instances, or the achievable objective |
| `total` | BIGINT | denominator of an unbounded escape count |
| `scope` | VARCHAR | `row`, or `entity` for a keyed decision, on an escape count |
| `edit_source` | VARCHAR | the finding's kind (below) |
| `group` | VARCHAR | the class's `per` key value (expanded scope; a NULL key prints as `NULL`), or the slice a decision escapes on |
| `row` | BIGINT | the kept row this finding covers (expanded scope) |

| `edit_source` | meaning |
|---|---|
| `source_literal` | a literal you wrote, loosened in place |
| `virtual_offset` | an offset over a data-backed bound (`x <= col + delta`) |
| `expanded_row` / `expanded_group` | one kept row's / one class's own overshoot |
| `remove_only` | a clause that can only be deleted (`<>`, `IN`, L0) |
| `unreachable_bound` | a bound no assignment can reach |
| `rigid_conflict` | loosening the written clauses cannot restore feasibility |
| `runaway_+inf` / `runaway_-inf` | a decision growing without bound |
| `achievable_objective` | the objective once the edits are applied |
| `unbounded_after_fix` | the fix works, but the objective then grows without limit |
| `undiagnosed` | the state is known but no engine named a cause |

### 8.2 Restrictions and settings

- The inner query must hold exactly one `DECIDE` in its bound plan; one relation must describe one solve.
- No options (`DIAGNOSE (VERBOSE)` does not exist).
- A query that fails before solving (syntax, binding, a model class the solver refuses, a data check) still raises.

| setting | effect |
|---|---|
| `diagnose_decide_infeasible_slack_scope` | `query` (default): one edit per SQL-level knob; `expanded`: one per kept row or per `per` class |
| `diagnose_decide_escape_rate` | report a categorical slice at this escape rate or above (0.8) |
| `diagnose_decide_categorical_ratio` | a column is categorical when distinct values ≤ ratio × rows (0.1) |
| `diagnose_decide_min_categories` | floor on that cap (20) |

### 8.3 A slow solve is not a diagnosis

Hitting the time limit or pressing Ctrl-C happens with or without the prefix. The query prints a checkpoint report,
then: at a terminal it offers to keep solving (Enter continues, `s` stops with the best so far); elsewhere it errors;
after Ctrl-C it returns the best solution found, marked not proven best.

A query answers with the proven optimum by default. `SET decide_mip_gap = 1e-4` accepts an answer within 0.01% of
it, which can finish a hard integer model sooner; the value must be 0 or more.

How the engine works: [diagnose.md](diagnose.md) and `../diagnostics/`.

## 9. `EXPLAIN`

`EXPLAIN`, `EXPLAIN ANALYZE` and `EXPLAIN (FORMAT JSON)` work on a DECIDE query. The DECIDE node lists the declared
variables, the objective and the constraints. Each clause leads with the SQL as written; indented lines follow only
when the model differs: `≡` for the canonical form, `↳` for what each rewrite made of it
(`abs(ship - 20) becomes an auxiliary variable`, `sum(ship) by (D.region) becomes a helper decision per group`) and
the rows the solver receives. An auxiliary prints as the expression it stands for, never an internal name; a bound
derived from data prints as `M`. `EXPLAIN` plans without solving; `EXPLAIN ANALYZE` solves and adds actual rows and
timing.

**Echo.** EXPLAIN, DIAGNOSE and messages print a clause in one style: lower-case reducers and keywords, the
`when c per K:` qualifier in front, a reducer's `by (Γ)` after it, the defaults `per row:` and `by ()` left out, the
colon form as written. A key prints as written; EXPLAIN and DIAGNOSE print other columns by bare name, where messages
qualify them over a join.

Tests: `test/decide/tests/test_explain.py`, `test_scope_explain_diagnose.py`. Internals: [explain.md](explain.md).

## 10. Direct solve

Some queries have a closed-form answer DuckDB computes with ordinary operators. DeciDB answers those directly instead
of calling a solver; a query it cannot prove takes the solver path unchanged. No syntax change is involved.

- **Choose rows by score**: one per-row `BOOL` under count limits (`sum(x) <= k`, `<`, `>=`, `>`, `=`, constants or
  columns), over all rows or a `when`, a linear score, optional pins (`when id = 7: x = 1`).
- **Independent decisions in a range**: per-row `BOOL`, `INT` or `REAL` decisions with per-row bounds and nothing
  linking rows; a linear objective or one square (`sum(power(x - target, 2))`).
- **Fill one budget by value per unit**: per-row `REAL` decisions with their own bounds and one budget
  `sum(w * x) <= B` (or `>=`, `=`, `BETWEEN`), with a linear objective.

A count limit or budget with a `by` key or a `per K:` takes the solver path. When several assignments are optimal you
may get a different one than the solver; the objective is the same. `EXPLAIN` shows `Direct solve` rows naming the
rule.

| setting | effect |
|---|---|
| `SET decide_direct_solve = 'auto'` | default: answer directly when proved |
| `SET decide_direct_solve = 'off'` | always use the solver |
| `SET decide_direct_solve = 'require'` | error when no rule applies |

`DIAGNOSE` always uses the solver. A prepared statement keeps its choice until re-planned. Internals:
`../direct_solve/README.md`.
