# Syntax Reference

> **Code-grounded syntax reference — DeciQL surface, reviewed 2026-09-26.**
>
> The current implementation is the source of truth. Every claim here describes
> behavior exercised by the current source and tests. The language is the clean
> redesign specified in `deciql_language_spec.md`; the pre-redesign spellings
> (`T.x(TYPE)`, `scalar x(TYPE)`, postfix `WHEN`/`PER`, `SUM(D: e)`) are gone and
> each raises a parser error naming the new spelling.
>
> Feature docs under `03_expressivity/` hold semantics and code pointers; this
> file is the single entry point for syntax questions.

## 1. The DECIDE clause

Two clause orders are accepted; they parse to the same plan.

**Split order** — the declaration sits between `SELECT` and `FROM`:

```sql
SELECT ...
DECIDE declarator [, declarator ...]
FROM ...
[JOIN ...]
[WHERE ...]
SUCH THAT constraint [AND constraint ...]
[objective]
```

**Single-block order** — the whole clause sits after `WHERE`:

```sql
SELECT ... FROM ... [WHERE ...]
DECIDE declarator [, declarator ...]
SUCH THAT constraint [AND constraint ...]
[objective]
```

Exactly one declaration position may be used, and `SUCH THAT` with at least one
constraint is mandatory (a declaration-level bound is not a constraint for this
purpose; write `SUCH THAT x <= 9` if nothing else applies).

## 2. Decisions

```
declarator ::= [ PER scope : ] name ( domain ) [ bounds ]
scope      ::= scope_elem [, scope_elem ...] | ()
scope_elem ::= column | relation            -- a relation expands to all of its columns
domain     ::= INT | REAL | BOOL | SEMIINT | SEMIREAL | TEXT IN ['v1', 'v2', ...]
bounds     ::= BETWEEN lo AND hi | <= hi | >= lo
```

### 2.1 Generation scope — how many decisions a declarator makes

| Spelling | One decision per … | Output |
|---|---|---|
| `x(INT)` | result row (the default) | its own value per row |
| `PER D: open(BOOL)` | distinct tuple of relation `D` | repeated on every row of the tuple |
| `PER D.region, P.productID: stock(REAL)` | distinct value of the key | repeated on every row with that key |
| `PER (): cap(INT)` | the whole query | the same value on every row |

A key is any list of columns and/or relations from the `FROM` clause. Two
declarators (or a declarator and a constraint `PER`, or a reducer `BY`) that name
the same columns share one key. A NULL in a key is a value of its own for a
decision; it generates no constraint instance (see §5).

`PER D: x` may still be referenced as `D.x` when the key is exactly one relation.

### 2.2 Domains

| Domain | Values | Result type |
|---|---|---|
| `INT` | whole numbers, `>= 0` unless bounded below | `BIGINT` |
| `BOOL` | 0 / 1 | `INTEGER` |
| `REAL` | continuous, `>= 0` unless bounded below | `DOUBLE` |
| `SEMIINT` | `0` **or** a whole number in `[lo, hi]` — both bounds required | `BIGINT` |
| `SEMIREAL` | `0` **or** a real in `[lo, hi]` — both bounds required | `DOUBLE` |
| `TEXT IN ['a', 'b', ...]` | exactly one of the listed strings | `VARCHAR` |

- **Bounds** put a decision's range beside its name: `ship(INT) BETWEEN 0 AND T.capacity`.
  A bound may be a constant or a column determined by the declarator's key, and
  is exactly the constraint `PER K: ship >= 0 AND PER K: ship <= T.capacity`
  written in `SUCH THAT`. The default lower bound is 0 and becomes negative only
  through an explicit negative bound (`x >= -5`, `BETWEEN -4 AND 4`, a negative
  `IN` literal).
- **SEMI domains** are stated through a hidden BOOL switch: `x <= hi * on` and
  `x >= lo * on`. A negative constant floor widens the box; a data floor
  (`BETWEEN lot AND cap` with `lot < 0` on some rows) needs an explicit negative
  constant bound as well.
- **TEXT decisions** are one-hot: one hidden BOOL indicator per value plus a
  sum-to-one row. A TEXT decision may only be written as `x = 'v'`, `x <> 'v'`,
  `x IN ('a', 'b')` or `x NOT IN (...)` — in a constraint body or an `IF` guard —
  and never in arithmetic or an objective. A value outside the list is a binder
  error naming the list. Values compare exactly (case-sensitive).
- Hidden switches and indicators never appear in `SELECT *` or in `EXPLAIN`'s
  declaration list; `EXPLAIN` shows the rows they became beneath the clause as
  written.

## 3. Constraints

```
constraint  ::= [ WHEN known ] [ PER scope ] [ IF unknown ] : body
              | body
body        ::= term op term                      -- =, <>, <, <=, >, >=, BETWEEN, IN
term        ::= arithmetic over columns, decisions, reducers and frames
```

The three prefixes are ordered **filter → generate → guard** and end in one colon:

| Prefix | Reads | Meaning |
|---|---|---|
| `WHEN θ` | known data only | keep the rows where θ holds before anything else happens; a decision inside θ is rejected with a pointer to `IF` |
| `PER K` | a key | generate one instance of the body per distinct value of K (`PER ()`: one for the query; omitted: one per row) |
| `IF b` | decisions | impose the instance only where b holds; a guard over known data is rejected with a pointer to `WHEN` |

```sql
SUCH THAT
    ship BETWEEN 0 AND capacity * open                          -- one row per tuple
AND PER D: SUM(ship) BY (D) <= stock                            -- one row per depot
AND WHEN priority = 'critical' PER R: SUM(ship) BY (R) >= demand
AND PER D IF NOT open: SUM(ship) BY (D) <= 0                    -- guarded
AND IF status = 'repair': ship <= capacity / 2
```

### 3.1 Generation and the functional-dependency rule

Every value an instance reads directly must be **one value per instance**: a
column in the key, a column of a relation wholly in the key, a column of a base
table whose `PRIMARY KEY`/`UNIQUE` columns lie in the key, a decision whose own
key lies in the key, a query-wide decision, a constant, or a reducer whose `BY`
key is itself determined. Anything else is rejected at bind time with the fix
spelled out:

```
column 'cap' is not determined by the generation key PER grp: add it to the key,
name its relation in the key, declare a PRIMARY KEY the key covers, or reduce it
with BY (grp)
```

`PER grp, cap: SUM(x) BY (grp) <= cap` is the usual repair; when the added
column refines the reducer's `BY` key, the instances share the group's sum and
their bounds are reduced onto it (the tightest for an inequality, every one for
`<>`, a contradiction for `=`).

### 3.2 Guards — `IF`

`IF b:` makes the instance the implication `b ⟹ body`. `b` is a BOOL decision
(`IF open:`), its negation (`IF NOT open:`), a TEXT comparison (`IF status =
'open':`), a linear comparison over integer-valued decisions with `<=`, `<`,
`>=`, `>` (`IF ship > 0:`), or an `AND` / `OR` of BOOL decisions and their
negations (`IF open AND NOT repairing:` is the linear guard `open - repairing
>= 1`; `IF a OR b:` is `a + b >= 1`). A comparison inside such a combination,
`=`/`<>` guards and guards over `REAL` decisions are rejected by name. A guard is stated natively as an indicator constraint where the
backend has one (Gurobi) and as a Big-M row otherwise (HiGHS), which needs a
finite bound on every decision the guarded row reads. Guards are supported on
linear bodies; a MIN/MAX, `<>`, ABS, quadratic or bilinear body cannot be
guarded yet.

### 3.3 Comparisons, casts and NULL

- `<>` and strict `<` / `>` step the bound on the integer lattice and are refused
  at bind time when the compared side is not provably whole-numbered (a `REAL`
  decision, a fractional column or multiplier). `<>` keeps every distinct bound
  value as its own exclusion.
- `BETWEEN a AND b` is `>= a AND <= b`; `x IN (v1, ..., vk)` on a decision becomes
  one-hot indicators.
- Products of two decisions are bilinear (McCormick for BOOL × anything on both
  backends; Gurobi otherwise); `POWER(e, 2)` in a constraint is a quadratic
  constraint (Gurobi only). Triple products are rejected.
- `CAST`/`::` over a decision is rejected; data casts keep DuckDB semantics. The
  solver works in `DOUBLE`.
- A NULL in any value the solver reads is an error naming the column; `WHEN`,
  `PER` and a frame's order key exclude NULL rows instead.

## 4. Reducers

```
reducer ::= agg ( [ WHEN known ] [ PER scope ] : expr ) [ BY ( keys ) ]
agg     ::= SUM | AVG | MIN | MAX
```

- **`WHEN θ` inside a reducer** filters its own rows (`SUM(WHEN priority: ship)`).
- **`PER K` inside a reducer** counts one term per distinct K tuple instead of one
  per join-result row (`SUM(PER D: opening_cost * open)` charges each depot once).
  The body must be a function of K.
- **`BY (Γ)`** is the aggregation key: the reducer's value for an instance is
  taken over the Γ-group containing the instance. Omitted or `BY ()` means the
  whole (WHEN-filtered) input. The generation key must determine Γ.
- The colon is optional when there is no prefix: `SUM(ship)` and `SUM(: ship)`
  are the same.

The generation × aggregation matrix, all of which parse, bind and solve:

| # | Constraint | Meaning |
|---|---|---|
| 1 | `: ship <= 0.2 * SUM(: ship) BY ()` | one row per tuple against the global sum |
| 2 | `PER (): SUM(ship) <= netCap` | one row for the query |
| 3 | `PER D: reserve >= share * SUM(demand) BY ()` | per depot, global reducer |
| 4 | `PER D: SUM(ship) BY (D) <= D.capacity` | per depot, its own sum |
| 5 | `: ship <= share * SUM(ship) BY (D)` | per tuple, its depot's sum |
| 6 | `PER D: reserve >= 0.1 * SUM(demand) BY (region) + 0.02 * SUM(demand) BY (country)` | several keys |
| 7 | `PER D: SUM(WHEN priority: ship) BY (D) <= cap` | filtered reducer |
| 8 | `PER S: ship <= 0.25 * SUM(ship) BY (D, S.day + S.transit)` | derived key |
| 9 | `MAX(PER g: SUM(x) BY (g))` (objective) | nested: one inner value per key |

Reducers do not nest in a constraint (`SUM(PER c: SUM(x) BY (c)) BY (d) <= 5`
  is refused; only the objective form nests). A reducer over an empty row set is
  an error. A key (`PER`, `BY`, `WITHIN`) names columns or relations, never a
  decision or a derived expression.

Other rules carried over: `AVG` divides by the counted rows; `MIN`/`MAX` in a
constraint are per-row in the easy direction and Big-M/native in the hard one; a
scalar factor may multiply a reducer (`2 * SUM(x)`) but a row-varying one may
not; `norm(e, p)` (`p` in `0`, `1`, `2`, `'inf'`) is an L_p term over a decision
expression and takes the same `WHEN` prefix (`norm(WHEN c: e, 1)`).

## 5. Frames — navigating a timeline

```
frame  ::= AT ( selector [ ELSE v ] : expr ) OVER ( order )
         | SUM ( FROM selector TO selector [ EVERY d ] [ ELSE v | ALL ] : expr ) OVER ( order )
selector ::= FIRST | LAST | [k] PREVIOUS | [k] NEXT
order    ::= key [ ASC | DESC ] [ CYCLIC ] [ WITHIN partition ]
```

A frame reads `expr` at other rows of the instance's timeline: the rows of its
`WITHIN` partition (default: the whole input) ordered by `key`; rows with equal
keys are peers at one position. `PREVIOUS`/`NEXT` count from the instance's own
position; `CYCLIC` wraps; `DESC` reverses the walk.

```sql
SUCH THAT stock = AT(PREVIOUS ELSE 0: stock) OVER (period WITHIN product) + produce - demand
AND SUM(FROM 2 PREVIOUS TO PREVIOUS: produce) OVER (period WITHIN product) <= 7
```

- `AT` reads one row of the selected position: the position must hold one row
  (make the key unique within the partition, or reduce with a range), else the
  query is refused. A range sums every row of every selected position; its
  endpoints are normalised and a straddling range includes the current
  position; `EVERY d` steps from the lower endpoint.
- `ELSE v` is a constant and contributes `v` once per missing position.
- The timeline is built over the `WHEN`-filtered rows; a NULL order key puts a
  row on no timeline.
- A missing position: inside a range it is skipped while any position remains;
  under `ELSE v` it reads `v`; under `ALL`, or for `AT` with no `ELSE`, the whole
  instance is **skipped** (not an error). An instance whose own key is NULL is
  skipped too.
- Frames appear in constraints only (an objective has no position), reduce with
  `SUM` only (`MIN`/`MAX`/`AVG` ranges are not available yet), never inside a
  reducer or under `ABS`, never with a reducer in their body, and their order
  key and partition read known data.

## 6. Objectives

```
objective ::= stage [ THEN stage ... ] | SATISFY
stage     ::= ( MAXIMIZE | MINIMIZE ) [ PER () : ] expr
```

- An objective is evaluated once; everything it reads directly must be one value
  for the query (a reducer, a `PER ()` decision, a constant).
- `THEN` chains lexicographic stages: each later stage is optimized among the
  optima of the stages before it (the solver freezes each stage's value as a row
  before solving the next, with a `1e-6` relative slack that is exact for
  integer-valued objectives). Later stages must be linear (`SUM`/`AVG` of
  decision terms); a `MIN`/`MAX`/`ABS`/quadratic body belongs in the first
  stage. A later stage that is unbounded under the earlier optima is an error.
- `SATISFY` (or omitting the objective) asks for any feasible assignment.
- The first stage may be linear, quadratic (`SUM(POWER(e, 2))`, convex on both
  backends; non-convex on Gurobi), bilinear, `MIN`/`MAX` (flat or nested `MAX(PER
  k: SUM(e) BY (k))`), or a `norm`.

## 7. `DIAGNOSE` — asking a query to explain itself

`DIAGNOSE` is a prefix on a `SELECT` that carries a `DECIDE` clause:

```sql
DIAGNOSE SELECT id, x FROM t DECIDE x(INT) SUCH THAT x <= 5 AND x >= 8 MAXIMIZE SUM(x);
```

It runs the query and reports on the run instead of returning its rows. **It is
the only thing that starts the diagnostics engine.**

### 7.1 What it returns

```
state | clause | suggested_change | amount | total | scope | edit_source | group | row
```

| column | type | what it holds |
| --- | --- | --- |
| `state` | VARCHAR | `infeasible`, `unbounded`, or `feasible` |
| `clause` | VARCHAR | the clause as you wrote it (prefixes first: `PER grp: SUM(x) BY (grp) >= 5`), or the runaway decision's name |
| `suggested_change` | VARCHAR | the smallest edit that addresses this finding (`WHEN region = 'B': buy <= <cap>`) |
| `amount` | DOUBLE | how far a bound moves, escaping instances, or the achievable objective |
| `total` | BIGINT | denominator for an unbounded escape count; otherwise NULL |
| `scope` | VARCHAR | `row` or `entity` for an unbounded escape count; otherwise NULL |
| `edit_source` | VARCHAR | what kind of finding this row is (below) |
| `group` | VARCHAR | the `PER` key, or the categorical slice a decision escapes on |
| `row` | BIGINT | the emitted row this finding covers, under the `expanded` slack scope |

`edit_source` values: `source_literal`, `virtual_offset`, `expanded_row`,
`expanded_group`, `remove_only` (`<>`, `IN`, L0), `unreachable_bound`,
`rigid_conflict`, `runaway_+inf` / `runaway_-inf`, `achievable_objective`,
`unbounded_after_fix`, `undiagnosed`. A query that solves returns one
`feasible` row. Because it is a relation it composes with `SELECT ... FROM
(DIAGNOSE ...)`.

### 7.2 Restrictions and tuning

A query without the prefix reports its state and stops. The inner query must
contain exactly one `DECIDE`; `DIAGNOSE` takes no options; a query that fails
before it can be solved still raises. Settings:
`diagnose_decide_infeasible_slack_scope` (`query` | `expanded`),
`diagnose_decide_escape_rate`, `diagnose_decide_categorical_ratio`,
`diagnose_decide_min_categories`. A time limit or Ctrl-C is ordinary execution
behaviour: a checkpoint report and, at a terminal, an offer to keep solving.

## 8. Inspecting a decision plan — `EXPLAIN`

`EXPLAIN`, `EXPLAIN ANALYZE` and `EXPLAIN (FORMAT JSON)` work on a DECIDE query.
The DECIDE node lists the declared decisions, the objective (every `THEN` stage
on its own line), and the constraints: each clause as written, with its prefixes
first, then indented canonical or rewritten rows only when the model differs
materially from that spelling. Hidden switches and indicators appear only in
those rewritten rows.

## 9. Solver backends

Gurobi and HiGHS. What differs: quadratic constraints, non-convex quadratic
objectives and non-Boolean bilinear products are Gurobi only; indicator
constraints (`<>`, `IF` guards) and MIN/MAX/ABS are native on Gurobi and Big-M
on HiGHS, which then needs finite bounds on the decisions involved. Plan
serialization is checked with `DECIDB_VERIFY_SERIALIZER=1`; the built model can
be dumped with `DECIDB_DUMP_MODEL=1`.
