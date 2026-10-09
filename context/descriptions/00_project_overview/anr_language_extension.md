# ANR Language Extension — `per`, `when`, `by`

> **Target behaviour — settled 2026-10-05. Items 1 and 2 are implemented
> (`syntax_reference.md` §1.1, §2.3); the rest is not.**
>
> This file specifies what the planned scoping language does. `syntax_reference.md`
> describes what the engine accepts today: as an item lands, what it delivers is written
> there, and this file stays whole until item 7 folds it in. The work plan — layers,
> tests, order — is [`../09_anr_language/todo.md`](../09_anr_language/todo.md) and is not
> restated.
>
> Sources: the user's checklist and decisions of 2026-10-05 (cited as D1–D8, as numbered
> in the plan), and the reference notes in `~/Desktop/Capstone` — `ANR Implementation.pdf`
> and `ANR.pdf` pp. 6–7, 14–32, 62. Anything not mentioned here is unchanged from
> `syntax_reference.md`.

## 1. Overview

Three keywords, three separate jobs:

| Keyword | Position | Job |
|---|---|---|
| `per K` | in front | how many decisions, constraints or reducer inputs are created: one per distinct value of the key `K` |
| `when c` | in front | which rows are looked at: those where `c` is true |
| `by (Γ)` | after a reducer | which rows that one reducer reads: those sharing the current value of `Γ` |

`per` never selects the rows a reducer reads, and `by` never changes how many
constraints exist.

Terms used below:

- **Reducer**: `SUM`, `AVG`, `MIN` or `MAX` over rows.
- **Tuple term**: any part of a clause that is not inside a reducer — a column, a
  decision, arithmetic over them.
- **Key / class**: a key is a list of columns (for `by`, also expressions); rows with
  equal key values form one class.
- **Kept rows**: the query's result rows (after joins and `WHERE`) that pass the
  enclosing `when`.
- **Determines**: key `K` determines a value when every row of each `K`-class carries
  the same value.

**Syntax**:

```
decide_list ::= declarator (',' declarator)*
declarator  ::= ['per' key ':'] name '(' INT | BOOL | REAL ')'

constraint  ::= [scope ':'] comparison       -- AND separates constraints; a scope covers exactly one
objective   ::= (MAXIMIZE | MINIMIZE) [scope ':'] expression      -- per must be '()' or absent
scope       ::= ['when' condition] ['per' key]                    -- at least one of the two

reducer     ::= agg '(' [scope ':'] expression ')' ['by' '(' [expression, ...] ')']
              | agg '(' relation, ... ':' expression ')' ['by' ...]   -- same as per relation, ...

key         ::= '(' ')'  |  'row'  |  element (',' element)*
element     ::= column | relation            -- a relation stands for all of its stored columns
```

| Omitted | Means |
|---|---|
| `per` on a declaration, on a constraint, or inside a reducer | `per row` |
| `per` on an objective | `per ()` |
| `by` | `by ()` — all kept rows |
| `when` | every row |

- The statement skeleton and both clause orders of `syntax_reference.md` §1 are unchanged.
- Inside a DECIDE clause `when`, `per`, and a `by` that directly follows `)` are
  keywords; a column with such a name is written quoted there (`"per"`). Ordinary SQL
  outside the clause is unaffected.
- `EXPLAIN` and `DIAGNOSE` echo clauses in these spellings; `DIAGNOSE`'s `group` column
  holds the `per` key value.

## 2. Decision Variables — `decide per K: x(TYPE)`

```sql
DECIDE ship(INT)                                 -- one per result row
DECIDE per D.depotID: reserve(REAL)              -- one per distinct depotID
DECIDE per D: open(BOOL)                         -- one per distinct tuple of D
DECIDE per D.region, P.productID: stock(REAL)    -- one per distinct pair, across two relations
DECIDE per (): cap(INT)                          -- one for the whole query
DECIDE ship(INT), per D: open(BOOL), per (): cap(INT)   -- three keys in one clause
```

- **Key elements** are columns and relations of the `FROM` clause, in any mix. A
  relation stands for all of its stored columns: `per D` is `per D.depotID, D.stock,
  D.opening_cost`. Expressions and decisions are not allowed in a key, nor `rowid` or a
  generated column, which only repeat stored ones.
- **A key is a set.** Order and repetition do not matter; `per a, b` and `per b, a`
  are the same key.
- **Each declarator has its own key** (D7). A `per` applies only to the name right
  after it; it does not carry over. To give two names the same key, repeat it.
- **No `per`** means one decision per result row. `per row` may be written out.
- **`per ()`** means one decision for the whole query.
- **Count.** One decision per distinct key value among the result rows. A key value
  that survives on no result row has no decision.
- **NULL** in a key column is a key value: rows with a NULL key share one decision (D5).
- **Output.** Every result row shows the value of its class's decision, so the value
  repeats on all rows of a class.
- **Reference.** A decision is referenced by its bare name everywhere; `D.open` is not
  accepted.
- **Types** are unchanged: `INT`, `BOOL`, `REAL`, mandatory; domains, default bounds and
  result column types as in `syntax_reference.md` §2.

**Example:**

```sql
-- result rows:  (D1, R1)  (D1, R2)  (D2, R3)
DECIDE per D: open(BOOL), ship(INT)
--   open -> 2 decisions (D1, D2)
--   ship -> 3 decisions (one per row): the key of open does not carry over

DECIDE per D: open(BOOL), per D: closed(BOOL)
--   open, closed -> 2 decisions each
```

## 3. Scoped Expressions

One form appears in three places — a constraint, the objective, and the argument of a
reducer:

**Syntax**: `[when c] [per K] : body`

`when` is written before `per`, and the colon is required whenever either is present.
It is evaluated on a set of rows in four steps:

1. **Filter.** Keep the rows where `c` is true.
2. **Generate.** Split the kept rows into classes by `K`. The body is produced once
   per class: one constraint, or one input value for the enclosing reducer.
3. **Aggregate.** A reducer `agg(...) by (Γ)` in the body reads all kept rows whose
   `Γ` equals the class's `Γ`, not only the class's own rows. Its argument is itself a
   scoped expression, evaluated on the rows the reducer reads.
4. **Well-defined.** `K` must determine every tuple term of the body and every `Γ` (§7).

**Example:**

```sql
-- kept rows
--   shipment  depot  region  ship  limit
--   S1        D1     North   x1    100
--   S2        D1     North   x2    100
--   S3        D2     North   x3     80
--   S4        D3     South   x4     50

per D.depotID: sum(ship) by (D.region) <= D.limit

--   classes: D1 {S1, S2}   D2 {S3}   D3 {S4}
--   D1: region North -> reads S1, S2, S3 ->  x1 + x2 + x3 <= 100
--   D2: region North -> reads S1, S2, S3 ->  x1 + x2 + x3 <= 80
--   D3: region South -> reads S4         ->  x4 <= 50
--   well-defined: every depot has one region and one limit
```

## 4. Constraints

### 4.1 `per` — how many constraints

```sql
SUCH THAT
    ship <= S.demand AND                                      -- no per: one per row
    per D.depotID: sum(ship) by (D.depotID) <= D.capacity AND -- one per depot
    per (): sum(ship) <= 1000                                 -- exactly one
```

- **`per K:`** creates one constraint per distinct value of `K` among the kept rows.
- **No `per`** means one constraint per kept row; `per row:` may be written out.
- **`per ():`** creates exactly one constraint.
- **One scope, one constraint.** In `per g: A AND B` only `A` is scoped; `B` starts a
  new constraint with the defaults.
- **Key elements** are columns and relations, as in §2. Expressions belong in `by`.
  Multi-column keys are written without parentheses: `per a, b:`.
- **NULL** is a key value: NULL-keyed rows form a class and get a constraint (D5). To
  leave them out, write `when a is not null per a:`.
- **`per` does not group reducers** (D6). `per g: sum(x) <= 10` bounds the total over
  all kept rows, once per `g`; the identical constraints are one constraint. To bound
  each group's own sum, write `per g: sum(x) by (g) <= 10`.
- **An explicit `per` must determine the body** (§7). No `per` always does.
- **No `per` over reducers** keeps today's meaning. `sum(x) <= cap` is one constraint
  per row, each pairing the same total with that row's `cap`, which is the total
  against the tightest `cap`. `sum(x) by (g) <= cap` likewise takes the tightest `cap`
  in each group. The bound rules of `syntax_reference.md` §5 apply unchanged (tightest
  for `<=` and `>=`; `=` refuses bounds that differ; `<>` keeps every value). The same
  clause under `per g:` instead requires `g` to determine `cap`.

### 4.2 `when` — which rows

```sql
SUCH THAT
    when S.priority: ship >= 1 AND
    when S.status = 'active' and S.demand > 0 per D.depotID:
        sum(ship) by (D.depotID) <= D.capacity
```

- **Condition.** Any boolean expression over data, ended by `per` or `:`; parentheses
  are not needed, and an `AND` before that end belongs to the condition. It may not
  mention a decision.
- **NULL** counts as false: the row is dropped.
- **Filtering comes first.** Classes are formed from kept rows only, so a key value
  with no kept row has no constraint. Every reducer in the body reads kept rows only.
- **Dropped rows keep their decisions.** The clause simply does not constrain them.
- **A `when` that keeps no row is an error** (D4), whether or not the constraint has a
  reducer. An input with no rows at all is not an error: the result is empty, as today.

### 4.3 What a constraint body may contain

- **Tuple terms and reducers mix freely**, on either side:
  `ship <= 0.20 * sum(ship)`,
  `reserve >= 0.10 * sum(S.demand) by (D.region) + 0.02 * sum(S.demand) by (D.country)`.
- **Each reducer has its own `by`**; several reducers in one constraint may read
  different rows.
- **A factor on a reducer** may be any term without a decision, not only a value for
  the whole query: `D.maxShipmentShare * sum(ship) by (D.depotID)`. It is a tuple
  term, so §7 applies to it. A decision as a factor is a product of two decisions and
  follows the bilinear rules.
- **Everything else is unchanged**: operators, `BETWEEN`, `IN`, `ABS`, `norm`,
  quadratic and bilinear terms, subqueries and NULL handling follow
  `syntax_reference.md` §3–§5. Limits of the first version are in §12.

## 5. Reducers

### 5.1 `by (Γ)` — which rows a reducer reads

```sql
sum(ship)                                             -- by (): all kept rows
sum(ship) by (D.depotID)                              -- rows of the current depot
sum(ship) by (D.depotID, S.dispatchDay + S.transitDays)   -- an expression as a key
```

- **Applies to** `SUM`, `AVG`, `MIN` and `MAX`, with or without a decision inside.
- **Γ** is a list of columns or expressions over data. It may not mention a decision.
- **Omitted** means `by ()`: all kept rows.
- **Rows read**: every kept row whose `Γ` equals the current class's `Γ` — including
  rows outside the class (§3 example).
- **NULL** is a value of `Γ`: NULL-keyed rows form one group (D5).
- **`Γ` must be determined** by the enclosing `per` key (§7).
- **Placement.** `by` directly follows the reducer's `)` and belongs to that reducer
  only. Anywhere else it is an error.
- **`MIN` and `MAX`** are accepted wherever `SUM` is (D3).

### 5.2 `when` and `per` inside a reducer

```sql
sum(when S.priority: ship)                          -- only priority rows
sum(per D: D.opening_cost * open)                   -- each D tuple counted once
sum(when S.priority per D: D.opening_cost * open)   -- filter, then each remaining D tuple once
```

- **Order**: rows selected by `by` → inner `when` → inner `per` → aggregate.
- **Inner `when`** filters this reducer only. Other reducers of the constraint, and the
  number of constraints, are unaffected. It may be combined with a `when` in front of
  the constraint; the one in front applies first.
- **Inner `per K'`** hands the reducer one value per distinct `K'` among its rows, so a
  tuple repeated by a join is counted once. `K'` must determine the reducer's body
  (§7). `per row` is the default; `per ()` hands it a single value.
- **`AVG`** divides by the number of values it receives: kept rows, or classes when an
  inner `per` is present.
- **`MIN` and `MAX`** are unaffected by an inner `per`.
- **A reducer with no row for some generated class is an error** (D8), naming the
  class. This can only come from the inner `when`. To leave such classes out, put the
  condition in front of the constraint:

```sql
-- error when some depot has no priority shipment
per D.depotID: sum(when S.priority: ship) by (D.depotID) <= D.priorityCapacity

-- only depots with a priority shipment get a constraint
when S.priority per D.depotID: sum(ship) by (D.depotID) <= D.priorityCapacity
```

### 5.3 The colon form — `SUM(D: e)`

`SUM(D: e)` and `SUM(D, T: e)` are kept (D1). They mean `sum(per D: e)` and
`sum(per D, T: e)`: same result, same checks, and they may take a `by`. The colon form
lists relations only; a column key needs the `per` spelling. The postfix `WHEN` that
could follow it is removed (§10).

### 5.4 `per` and `by` are independent

| Written | Constraints | Each reducer reads |
|---|---|---|
| `ship <= 0.20 * sum(ship)` | one per row | all rows |
| `per (): sum(ship) <= cap` | one | all rows |
| `per D.depotID: reserve >= w * sum(S.demand)` | one per depot | all rows |
| `per D.depotID: sum(ship) by (D.depotID) <= D.capacity` | one per depot | that depot's rows |
| `ship <= w * sum(ship) by (D.depotID)` | one per row | the rows of that row's depot |

### 5.5 Reducers inside reducers

Not supported, with one exception: the two-level objective of §6. Every other nesting
is refused with "not supported yet" (§12).

## 6. Objective

```sql
MAXIMIZE sum(ship)                         -- per () implied
MAXIMIZE per (): sum(ship)                 -- the same, written out
MAXIMIZE when S.priority: sum(ship)        -- over priority rows only
MINIMIZE sum(unit_cost * ship) + sum(per D: D.opening_cost * open)
MINIMIZE max(per D.depotID: sum(ship) by (D.depotID))     -- two-level form
```

- **Produced once.** An omitted `per` means `per ()`. Any other `per` — a key, or
  `per row` — is an error.
- **`when`** in front filters the rows for the whole objective. One that keeps no row
  is an error (D4).
- **Body.** As today (`syntax_reference.md` §4): reducers, whole-query decisions, and
  sums of those. A tuple term must be determined by the empty key (§7), so a per-row or
  keyed decision outside a reducer is an error unless it has a single instance over the
  kept rows.
- **`by` on a top-level reducer** must also be determined by the empty key: use
  `by ()`, or a `Γ` with a single value over the kept rows.
- **Inside reducers**, `when` and `per` work as in §5.2.
- **Two-level form** (D2): `outer(per k: inner(e) by (k))` evaluates `inner` once per
  class of `k`, then `outer` over those values. The inner `per` key and the `by` key
  must be the same. All nine combinations of `SUM`, `MIN`, `MAX` are accepted. It
  replaces `MINIMIZE OUTER(INNER(e)) PER k`.
- **No objective** is a feasibility problem, as today.

## 7. The Determination Rule

A `per` key must determine everything its body reads once per class:

- every **tuple term** — a column or expression outside reducers, including the bound
  and any factor on a reducer;
- every **decision** outside reducers;
- every reducer's **`by` key**.

An inner `per` key must likewise determine the body of its reducer.

- **A decision** declared with key `Kv` is determined by `K` when all rows of each
  `K`-class belong to one `Kv`-class. A per-row decision is determined only when every
  `K`-class is a single row. A `per ()` decision is always determined.
- **Always determined**: literals, uncorrelated scalar subqueries, and anything under
  `per row`.
- **`per ()`** requires each tuple term to have one value over all kept rows.
- **Checked against the data**, on every run, before solving, over kept rows only. The
  same query can therefore pass on one dataset and fail on another.
- **NULL** counts as one value in this comparison. A NULL in a coefficient or bound is
  still the NULL error of `syntax_reference.md` §3.
- **On failure** the query stops with an error that names the key, the expression, and
  one class holding two values (§9).

**Examples:**

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

## 8. NULL and Empty Sets

| Situation | Outcome |
|---|---|
| NULL in a `decide per` key | its own decision (D5) |
| NULL in a constraint's `per` key | its own class, with a constraint (D5) |
| NULL in a `by` key | its own group (D5) |
| NULL `when` condition | row dropped |
| NULL in a coefficient or bound | error, as today |
| `when` in front of a constraint or objective keeps no row | error (D4) |
| a key value has no kept row | no class, no constraint, no error |
| a reducer has no row for a generated class | error naming the class (D8) |
| the query's input has no rows at all | empty result, as today |

## 9. Errors

| Situation | Raised | Message |
|---|---|---|
| A `per` key does not determine a value | run, before solving | `DECIDE: per D.depotID does not determine S.region (in by (S.region)). D.depotID = 'D1' has more than one value: 'East', 'West'.` |
| A `per` key does not determine a decision | run, before solving | `DECIDE: per D.depotID does not determine the decision ship (declared per row). D.depotID = 'D1' covers more than one ship.` |
| A `when` keeps no row | run, before solving | `DECIDE: when S.status = 'actve' matches no rows.` |
| A reducer has no row for a class | run, before solving | `DECIDE: sum(when S.priority: ship) has no rows for D.depotID = 'D2'.` |
| `per` other than `()` on an objective | parse | `an objective is produced once: write per (): or leave per out` |
| Parenthesized key | parse | `write per a, b: without parentheses; parentheses are only for per ()` |
| `by` not directly after a reducer | bind | `by (...) must directly follow SUM, AVG, MIN or MAX` |
| A decision in `when`, in a key, or in `by` | bind | `x is a decision; when, per and by may only use data` |
| An expression in a `per` key | parse | `a per key lists columns or relations; put expressions in by (...)` |
| Unknown key element | bind | `depot is neither a column nor a relation of the FROM clause` |
| `rowid` or a generated column in a key | bind | `g.b is not a stored column; a per key lists columns or relations of the FROM clause` |
| An unqualified `FULL OUTER JOIN USING` column in a key | bind | `depotID is merged by a FULL OUTER JOIN USING; write S.depotID or D.depotID in the per key` |
| A reducer inside a reducer (other than §6) | bind | `an aggregate inside an aggregate is not supported yet` |
| A removed spelling | parse | names the new spelling (§10) |

- The phrases `does not determine`, `matches no rows` and `has no rows for` are fixed;
  the names and values around them are quoted from the query and the data.
- Messages quote the user's SQL only, never an internal name.
- All of these are still raised under `DIAGNOSE`: nothing was solved, so there is
  nothing to diagnose.

## 10. Removed Spellings

Each is a parse error whose message names the new spelling.

| Removed | Write instead |
|---|---|
| `T.x(INT)` | `per T: x(INT)` |
| `scalar x(INT)` | `per (): x(INT)` |
| `C PER g` · `C PER (g, h)` | `per g, h: C`, with `by (g, h)` added to each reducer of `C` |
| `C WHEN c` | `when c: C` |
| `C WHEN c PER g` | `when c per g: C`, with `by (g)` as above |
| `SUM(e) WHEN c` | `sum(when c: e)` |
| `SUM(D: e) WHEN c` | `sum(when c per D: e)` |
| `MAXIMIZE E WHEN c` | `MAXIMIZE when c: E` |
| `MINIMIZE SUM(MAX(e)) PER g` | `MINIMIZE sum(per g: max(e) by (g))` |
| `MINIMIZE SUM(e) PER g` | `MINIMIZE sum(e)` |

Four behaviours change with them:

- **`PER` no longer groups reducers.** The old `SUM(x) <= 10 PER g` is
  `per g: sum(x) by (g) <= 10`; without the `by` it is a bound on the overall total.
- **A bound that varies inside a group** was reduced to the tightest value under
  `PER`. Under an explicit `per` it is the determination error; without `per` it is
  still the tightest value (§4.1).
- **NULL keys** were dropped by `PER`; they now form a class (§8).
- **Empty sets** were sometimes skipped silently — a `WHEN` matching nothing on a
  per-row constraint, a group whose filtered reducer had no rows. Both are now errors
  (§8).

## 11. Deck Examples 1–8

All run as written, in both forms, over:

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

- Example 7 is an error on data where some depot has no priority shipment (§5.2).
- Example 8 needs `S.shipmentID` to determine the per-row `ship`, which holds because
  each shipment is one result row.

## 12. Limits and Exclusions

**Limits of the first version** — each is refused with a message, never answered wrongly:

- **Reducers inside reducers**, except the objective form of §6. This includes deck
  example 9.
- **A product of two non-Boolean decisions, or `POWER(…, 2)`, inside a reducer** is
  accepted only where it is today: in a constraint whose decision-bearing reducers
  share one `by` key and have no per-row or keyed decision beside them.
- **A reducer over data only** stays limited to the bound side of a comparison.
- **A reducer whose body is only a whole-query decision** (`sum(cap)`) is still
  rejected (`syntax_reference.md` §2.2).
- **A hard-direction `MIN`/`MAX`** still needs every contributing decision to have a
  finite bound.

**Not part of this extension** (shown in the deck, deliberately left out): variant
signatures, frame selectors (`at`, `over`, `within`), `if` guards, bounds in a
declaration, the `SEMIREAL` / `SEMIINT` / `TEXT` types, `then` and `satisfy`.
