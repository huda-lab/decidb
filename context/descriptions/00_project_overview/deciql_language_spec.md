# DeciQL Language Specification (target — clean redesign)

> **Status: IMPLEMENTED (2026-09-26), with the exceptions listed here.**
>
> This document is the language design from the NYUAD "DeciQL proposals" deck
> (Mai, HUDA Lab). It is a **clean redesign** and the engine now speaks it: the
> old overloaded `PER`, `T.x(TYPE)`, `scalar x(TYPE)`, postfix `WHEN` and
> `SUM(D: e)` are gone (each raises a parser error naming the new spelling), and
> the test suite, golden corpus and benchmarks were migrated with it.
>
> What the engine does **today** is recorded in [`syntax_reference.md`](syntax_reference.md);
> read that first for the shipped surface. The open decisions of §10 were settled:
>
> | decision | settled as |
> |---|---|
> | OPEN-1 `per` placement in declarations | prefix: `PER F.city: open(BOOL)` |
> | OPEN-2 FD-failure policy | reject with a named error (§6.3); Variant Signature (§7.2) is **not** implemented |
> | OPEN-3 `norm(e, 2)` | squared L2 (unchanged) |
> | OPEN-4 backend reach | indicator constraints (`IF`, `<>`) native on Gurobi, Big-M on HiGHS; lexicographic `THEN` as staged solves on both; nothing new is gated |
> | OPEN-5 `SATISFY` | both `SATISFY` and an omitted objective are accepted |
>
> Not yet implemented from this spec: derived keys (`BY (D, S.day + S.transit)`,
> §9 #8 — a key names columns or relations), nested reducers in a *constraint*
> (§9 #9's `sum(... max(...) by (c)) by (d) <= cap`; only the objective form
> `MAX(PER k: SUM(e) BY (k))` is formulated), a frame inside a reducer or a
> reducer inside a frame's body ("nested frame aggregation"), range frames
> reducing with `MIN`/`MAX`/`AVG` (only `SUM` and `AT`), `ABS` or `POWER` over a
> frame term, a non-constant `ELSE` value, `IF` guards on
> MIN/MAX/`<>`/ABS/quadratic/bilinear/`IN` bodies (except an easy MIN/MAX over the
> instance's own group, `per k if b: max(x) by (k) <= K`), a comparison inside an `AND`/`OR`
> guard (pure `AND`/`OR` of BOOL decisions is supported), guards comparing a
> `REAL` decision, `=`/`<>` guards, frames or TEXT decisions inside an objective,
> and MIN/MAX, `<>`, ABS, quadratic or bilinear bodies in a constraint that reads
> rows outside its own instance (a reducer keyed differently from `PER`, a reducer
> beside a per-row term, a frame; an easy MIN/MAX against a bound-side reducer, as
> in `max(x) <= min(cap)`, is supported), a MIN/MAX compared with a frame, and a
> sum or difference of MIN/MAX terms under a `when`/`per` prefix. Each is refused with a message naming the
> restriction. Variant Signature (§7.2) is not implemented: the FD rule of §6.3
> rejects instead.
>
> Reviewed against the deck on 2026-09-29: the explicit default `per row` (§6.1,
> deck p16) is accepted as `PER ROW`; `else NULL` (deck p43) as the spelled
> default; a parenthesized key (`PER (a, b)`) as the same key; deck example 5's
> data factor on a reducer (p21) binds when the reducer's `BY` key determines it;
> deck p52's `per T.productID: at(first ...)` binds (an absolute selector needs
> only the `WITHIN` partition determined). Two amendments to §6.2/§6.3 are marked
> *2026-09-29* below: NULL is a key value for generation too, and a reducer over
> no rows has no value rather than being an error.
>
> Reading order: §1 principles → §3–4 grammar → §6–7 semantics → §8 lowering →
> §9 conformance examples.

---

## 1. Design principles

The whole redesign rests on **separating three jobs** that today's syntax
conflates into `PER` and implicit reducer scope:

| Job | Question it answers | Construct |
|---|---|---|
| **Generation** | How many constraints / variables are created? | *prefix* `per K` |
| **Aggregation** | Which rows does one reducer consume? | *postfix* `by (Γ)` on a reducer |
| **Navigation** | Which ordered row/range does an expression read? | frame selector + `over (… within P)` |

Two more roles condition a constraint:

| Role | Meaning | Construct |
|---|---|---|
| **Filter** | Drop *known* data before generating | `when θ` (θ has no decisions) |
| **Guard** | Activate a constraint on an *unknown* (decision) condition | `if b` (b contains decisions) |

Design goals carried from the deck: **maximize readability, minimize
repetition, make statement generation precise.** Every construct below is the
minimal capability needed to describe a surveyed real-problem family (M1–M15).

The correctness spine is a single recurring idea: at every scoped expression the
**generation key must functionally determine** every value the constraint reads —
each direct tuple term, each reducer group key, each frame partition, and each
guard. This is what makes "one constraint per key" well-defined. §6.3 states it
formally; the deck proves it (Theorems, deck p31/p55).

---

## 2. Notation and conventions

- Grammar is EBNF: `x?` optional, `x*` zero-or-more, `x+` one-or-more,
  `a | b` alternation, `'kw'` a literal token, `UPPER` a nonterminal defined
  elsewhere, lower-case a lexical class (`ident`, `int_lit`, `string_lit`).
- Semantic brackets `⟦E⟧R` denote the value of expression `E` over relation `R`,
  following the deck.
- `Rθ` is the relation after the `when` filter; `κ` the resolved generation key;
  `γ` a reducer group key; `π` a frame partition key; `b` a decision guard.
- `R ⊨ X → Y` means "in relation R, X functionally determines Y."
- **OPEN** marks a decision this spec has not settled (see §10).

---

## 3. Lexical structure

### 3.1 New reserved words

None beyond today's `DECIDE`, `SUCH`, `MAXIMIZE`, `MINIMIZE`. `WHEN`, `THEN`
and `IF` are already SQL keywords. `PER`, `BY`, `OVER`, `WITHIN`, `AT`,
`SATISFY`, `PREVIOUS`, `NEXT`, `FIRST`, `LAST`, `EVERY`, `CYCLIC`, `SEMIREAL`,
`SEMIINT` stay unreserved: they remain usable as column, alias and table names
everywhere, including inside a DECIDE body, because the lexer emits their
DECIDE-only tokens by context (§3.2).

### 3.2 New unreserved / contextual words

Kept unreserved so they remain usable as ordinary identifiers, recognized only
inside a DECIDE phase (same gating strategy the engine already uses for the
`WHEN_DECIDE` token):

`first`, `last`, `previous`, `next`, `from`, `to`, `every`, `else`, `all`,
`cyclic`, `asc`, `desc`, `between`, `and`, `in`, `satisfy`.

New type names: `SEMIREAL`, `SEMIINT`, `TEXT` (added to
`type_name_keywords.list`).

> **Gating requirement.** As today, DECIDE-only tokens (`BY`, `WITHIN`, `OVER`,
> `AT`, frame words) must be lexed as their special forms **only** inside the
> `SUCH THAT` / objective phase, so they never collide with global SQL. This is
> the parser's job (stage 01) and must not leak upward. See §8.1.

---

## 4. Concrete grammar

The outer statement skeleton is unchanged from today (two DECIDE positions,
both parsing to the same plan). Only the *internals* of the declaration,
constraint, and objective are redesigned.

### 4.0 Statement

```ebnf
decide_query ::=
    'SELECT' select_list
    ( 'DECIDE' decl_list )?            -- split order: declaration before FROM
    'FROM' from_expr
    ( 'WHERE' predicate )?
    ( 'DECIDE' decl_list )?            -- single-block order: after WHERE
    'SUCH' 'THAT' constraint_list
    objective_decl?
```

Exactly one `DECIDE` position may be used (a declaration in both, or a
declaration with no `SUCH THAT`, is an error — unchanged from today).

### 4.1 Variable declaration

```ebnf
decl_list   ::= declarator ( ',' declarator )*
declarator  ::= ( 'PER' scope ':' )? name '(' domain ')' bounds?
scope       ::= scope_elem ( ',' scope_elem )*
scope_elem  ::= qualified_column | relation_name       -- relations expand to their columns (§6.1)
domain      ::= 'BOOL' | 'INT' | 'REAL'
              | 'SEMIREAL' | 'SEMIINT'                  -- require bounds (§6.4)
              | 'TEXT' 'in' '[' string_lit ( ',' string_lit )* ']'
bounds      ::= 'between' simple_expr 'and' simple_expr
              | ( '<=' | '>=' ) simple_expr
simple_expr ::= constant | column_in_scope             -- restricted: no decisions, no reducers
```

Notes and examples (deck p6–11):

- `PER F.city: open(BOOL)` — one decision per **distinct city**, not per row.
- `PER D.region, P.productID: stock(REAL)` — decision per **multi-column key**
  spanning two relations, without materializing the distinct tuples first.
- A relation in the scope expands to all its columns:
  `PER D: open(BOOL)` ⟹ `PER D.depotID, D.stock, D.opening_cost: open(BOOL)`.
- No `PER` ⟹ **row-scoped** (one decision per surviving result row) — the
  default that matches today's bare `x(TYPE)`.
- Declaration-level bounds put a variable's range beside its name instead of in
  `SUCH THAT`: `PER T.routeID: ship(INT) between 0 and T.capacity`.

> **OPEN-1 (per placement in declarations).** The deck is inconsistent: p6 writes
> the key as a prefix (`decide per F.city: open(BOOL)`) while p9/p74 trail it
> (`ship(INT) between 0 and T.capacity per T.routeID`). This spec picks the
> **prefix `PER scope:` form** for consistency with constraints (generation key
> is always a prefix). Confirm before freezing the grammar.

### 4.2 Constraint declaration (ANR + frames + conditional)

A constraint is a **scoped boolean**. The three optional prefixes are ordered
`when` (filter) → `per` (generate) → `if` (guard):

```ebnf
constraint_list ::= scoped_bool ( 'AND' scoped_bool )*
scoped_bool     ::= ( 'WHEN' known_bool )?
                    ( 'PER' scope )?
                    ( 'IF' unknown_bool )?
                    ':' bool_body
bool_body       ::= term compare_op term            -- =, <>, <, <=, >, >=, between, in
```

Inside a body, a term is a deterministic scalar composition `f(…)` of tuple
expressions, **reducers**, and **frame expressions**:

```ebnf
term        ::= f_of( tuple_expr | reducer | frame_expr )    -- +, -, *, POWER, abs, …

reducer     ::= agg '(' scoped_expr ')' ( 'BY' '(' group_keys ')' )?
scoped_expr ::= ( 'WHEN' known_bool )? ( 'PER' scope )? ':' term  -- nesting; ':' optional when no prefix
agg         ::= 'sum' | 'avg' | 'min' | 'max'                 -- COUNT over decisions stays rejected
group_keys  ::= expr ( ',' expr )*

frame_expr  ::= 'AT' '(' selector ( 'else' value )? ':' expr ')' 'OVER' '(' order ')'
              | agg '(' range ( 'every' int_lit )? ( 'else' value | 'all' )? ':' expr ')' 'OVER' '(' order ')'
selector    ::= 'first' | 'last' | int_lit? 'previous' | int_lit? 'next'   -- omitted k = 1
range       ::= 'from' selector 'to' selector
order       ::= sort_key ('asc'|'desc')? 'cyclic'? ( 'WITHIN' partition )?  -- omitted dir = asc, within = ()
```

Binding rules for the postfix operators (deck p28, p43):

- **Postfix `BY (…)` binds to the immediately preceding aggregation**, exactly
  like SQL's postfix `OVER (…)`.
- **`OVER (…)` binds to the immediately preceding frame expression.** Sibling
  frame expressions in one constraint may each use a different order / partition
  / cyclic policy.

Defaults (deck p16, p43, p56): no `PER` ⟹ *per row*; no `BY` ⟹ `BY ()`
(the current scope); omitted `k` ⟹ 1; omitted direction ⟹ `asc`; omitted
`within` ⟹ `within ()`; omitted `every` ⟹ `every 1`; omitted `else` ⟹
`else NULL`.

### 4.3 Objective declaration

```ebnf
objective_decl ::= objective ( 'THEN' objective )*        -- lexicographic
                 | 'SATISFY'                              -- feasibility only
objective      ::= ( 'MINIMIZE' | 'MAXIMIZE' ) ( 'PER' '(' ')' ':' )? expr
```

- `THEN` chains **lexicographic** objectives: optimize the first; among its
  optima optimize the second; and so on (deck p62–63).
- `SATISFY` asks only for a feasible assignment (replaces today's "omit
  MAXIMIZE/MINIMIZE"). The two forms are mutually exclusive.
- An objective is generated **exactly once**, so an omitted outer `PER` resolves
  to `PER ()`, never per-row. Any other `PER` on an objective is rejected (a
  parser error names the `PER ():` form and the nested-reducer alternative).
- A later stage must be linear (`SUM`/`AVG` of decision terms); a `MIN`/`MAX`,
  `ABS`, quadratic or `norm` body belongs in the first stage.

---

## 5. Abstract syntax (AST sketch)

What stage-01 produces and stage-02 binds. One node per construct so ownership
is unambiguous (stage numbers per `.claude/CLAUDE.md`):

```
Declarator   { scope: Key, name, domain: Domain, bounds: Bounds? }
Domain       = Bool | Int | Real | SemiReal | SemiInt | TextEnum(values)
Constraint   { filter: KnownBool?, gen: Key?, guard: UnknownBool?, body: Compare }
Reducer      { agg, arg: ScopedExpr, group: Key? }          -- group = BY (…)
ScopedExpr   { filter: KnownBool?, gen: Key?, body: Term }
Frame        { kind: Point|Range, sel, fallback: Else, order: Order }
Order        { key, dir, cyclic: bool, within: Key? }
Objective    = Lexicographic([ (Sense, Expr) ]) | Satisfy
Key          = list of (qualified_column | relation)        -- pre-expansion
```

`Key` is the one shared shape carrying generation (`per`), aggregation (`by`),
and partition (`within`) — three *uses* of the same "set of columns/relations"
structure. Keeping them one type, three fields, is what the "per Trap"
resolution buys us: the *role* is the field, not the spelling.

---

## 6. Static semantics (binder — stage 02)

### 6.1 Scope resolution and relation expansion

A `Key` is resolved by expanding every relation element into its represented
columns (deck p7), then qualifying each column to its source relation. After
expansion a key is a list of qualified columns. Qualifiers only affect name
resolution — `T.ship(INT)` and `ship(INT)` under the same scope differ only in
name, never in decision cardinality (deck p7).

Resolution of the `per` clause to a key `κ` (deck p29):

```
κ = { rowid }   if per omitted or 'per row'   -- Tuple Per
    ∅           if 'per ()'                     -- Global Per
    K           if 'per K'                      -- Local Per
```

Reducer group `γ`: `∅` if `BY` omitted (⟹ `BY ()`), else the given `Γ`.
Frame partition `π`: `∅` if `WITHIN` omitted, else the given `P`.

### 6.2 Type system

| Domain | Values | Bounds |
|---|---|---|
| `BOOL` | {0,1} | implicit |
| `INT` | ℤ≥0 by default; signed with an explicit negative bound | optional |
| `REAL` | [0,∞) by default; signed with an explicit negative bound | optional |
| `SEMIREAL` | {0} ∪ [x,y] | **required** `between x and y` |
| `SEMIINT` | {0} ∪ ([x,y] ∩ ℤ) | **required** `between x and y` |
| `TEXT` | one of a finite string set | **required** `in [...]` |

Rules carried from today (still hold): `INT` result is `BIGINT`; `<>` / strict
`<` / `>` require a provably integer LHS; a NULL in any value the solver reads
is an error, not a zero. A NULL in a **key** is not a value the solver reads: it
is a key value of its own, as in SQL's `GROUP BY` (*2026-09-29*: generation
included — a `PER K` constraint imposes its instance on the NULL-keyed rows, and
`WHEN K IS NOT NULL` excludes them; the earlier rule that a NULL generation key
produced no instance left the NULL-keyed decision unconstrained by every keyed
clause). An aggregation key (`BY`) and a frame partition (`WITHIN`) put
NULL-keyed rows in a group of their own; a frame's order key puts a NULL-keyed
row on no timeline (its instance is skipped); a decision keyed on NULL exists and
reads back.

A `SEMI` range whose floor is a *data* column that is negative on some rows
needs an explicit negative constant bound as well (`x(SEMIINT) BETWEEN lo AND hi
... SUCH THAT x >= -K`): only a constant floor widens the column box. A
declaration bound is a constant or a column; a decision or a reducer in a
bound is rejected (write it in `SUCH THAT`), as is an empty constant range
(`BETWEEN 5 AND 2`). `TEXT` decisions are only usable where an enum makes
sense — comparisons `status = 'open'` and as generation predicates (deck p11,
p58); arithmetic on a `TEXT` decision is rejected.

### 6.3 Well-definedness (the core theorems)

Implementation note: a column is also determined by a key when it belongs to a
base table whose `PRIMARY KEY` / `UNIQUE` columns all lie in the key — the
schema's own functional dependency, which is how `PER S.shipmentID: ... BY
(S.depotID, S.day)` is admitted when `shipmentID` is the table's key and refused
over an unkeyed `VALUES` list; a decision is determined the same way (its own key
inside the generation key, or covered by a table key of its relation — for a
per-row decision, a table key of every relation in `FROM`). A reducer over an
**empty** row set (a `WHEN` that admits nothing, a `BY` group with no rows) has
no value (*2026-09-29*: the deck's NULL policy of §7.3 applied to reducers): the
instance is not imposed; beside another reducer that does read rows it
contributes nothing; a reduced constraint none of whose instances reads a row
is refused naming its prefix; in an objective it is an error.

For **every** scoped expression reached recursively — with filtered relation
`Rθ`, resolved key `κ`, direct tuple terms `eᵢ`, reducer groups `γⱼ`, frame
partitions `πℓ`, and guard `b` — the binder must be able to prove:

```
Rθ ⊨ κ → eᵢ      (each direct tuple term is constant per generated class)
Rθ ⊨ κ → γⱼ      (each reducer group is identified by the class)      [deck p27]
Rθ ⊨ κ → πℓ      (each frame timeline is identified by the class)     [deck p53]
Rθ ⊨ κ → b       (the guard is identified by the class)               [deck p59]
```

If all hold, `⟦E⟧R` is well-defined (deck Theorems p31, p55). The default rules
are special cases that always hold: `per row` gives `{rowid} → anything`;
`by ()` / `within ()` give `κ → ∅`.

When a dependency **cannot** be proven, there are two admissible policies:

- **Reject** — emit a named error (the safe, today-compatible choice).
- **Variant Signature** — do *not* reject; instead emit one constraint per
  distinct `(e₁…eₚ, γ₁…γq)` signature within the class (deck p34–40). This is a
  conservative extension: when the FD holds, each class has exactly one
  signature and the result is identical to the reject-world semantics.

> **OPEN-2 (FD failure policy).** Choose reject vs Variant Signature as the
> default, per construct. Variant Signature is more expressive but changes the
> binder (it must enumerate signatures) and the executor (it emits N
> constraints). Recommendation: ship **reject** first with a precise message,
> add Variant Signature behind an explicit opt-in later.

### 6.4 Rejection catalogue (initial)

Each must produce a concise, SQL-level message naming the offender:

1. `SEMIREAL`/`SEMIINT`/`TEXT` without required bounds/domain.
2. A generation key that cannot be resolved to real columns/relations.
3. FD failure (§6.3), under the reject policy: name `κ` and the offending
   `γ`/`π`/`b`.
4. `PER` on an objective that is not `PER ()`.
5. `if` guard that contains **no** decision (should be a `when` filter);
   `when` filter that **does** contain a decision (should be an `if` guard).
6. A frame `over` whose `within` key is not determined by the surrounding `per`
   (special case of #3).
7. Carried forward: `COUNT` over a decision; arithmetic on a `TEXT` decision;
   non-integer LHS under `<>` / strict inequality; NULL reaching the solver.

---

## 7. Dynamic semantics (denotational)

### 7.1 ANR evaluation

For a scoped expression `E = [when θ][per K]: B` with
`B = f(e₁,…,eₚ, G₁,…,Gq)` and `Gⱼ = αⱼ(Eⱼ) by (γⱼ)` (deck p29–30):

1. Filter: `Rθ = { t ∈ R : θ(t) }`.
2. Resolve `κ`, partition `Rθ` into classes `Rθ/κ`.
3. For each class `C` and reducer `j`, the group `ργⱼ(C)` is the γⱼ-class of
   `Rθ` containing `C` (well-defined by `κ → γⱼ`).
4. `⟦Gⱼ⟧C = αⱼ(⟦Eⱼ⟧ over ργⱼ(C))`.
5. `⟦E⟧R = [ f(⟦e₁⟧C,…,⟦eₚ⟧C, ⟦G₁⟧C,…,⟦Gq⟧C) | C ∈ Rθ/κ ]`.

The result is a **list** of body values — for a constraint body, each list
element becomes one imposed constraint instance.

### 7.2 Variant Signature (general case)

When `κ → eᵢ`/`γⱼ` need not hold, for class `C` define the signature
`σ(t) = (⟦e₁⟧t,…,⟦eₚ⟧t, γ₁(t),…,γq(t))`; enumerate distinct signatures
`Σ(C)`; for each `s` pick a representative and evaluate reducers over
`Uⱼ,ₛ = { u ∈ Rθ : γⱼ(u) = γⱼ(tₛ) }`. Then
`⟦E⟧R = ⨄_{C} [ v_{C,s} | s ∈ Σ(C) ]` (disjoint union, multiplicity preserved;
deck p37–40). Reduces to §7.1 when every class has one signature.

### 7.3 Frame selectors

A frame expression navigates a **peer-ordered timeline** (deck p54):

1. Resolve direction `δ` (default `asc`) and partition `π` (default `()`).
   Require `κ → π`.
2. For class `C`, `ρπ(C)` is the unique partition containing `C`.
3. Group `ρπ(C)` by the sort key `S`, order peer groups by `δ`; `cyclic` wraps
   the last peer group to the first.
4. Apply the selector (`first`/`last`/`k previous`/`k next`) or range
   (`from … to … every d`) to pick a point or a set of peer groups.

Point vs range spellings:
- `at(F else v: e) over (O)` — read expression `e` at position `F`; if the
  position doesn't exist use `v` (default NULL).
- `agg(from F₁ to F₂ [every d] [else v | all]: e) over (O)` — aggregate `e`
  over the selected positions; `every d` steps (e.g. `every 7`); `all` requires
  a complete frame (else NULL); `else v` fills missing positions.

Implementation notes: the timeline is built over the WHEN-filtered rows `Rθ`,
so a `WHEN` that removes the previous period removes that position too. `at(F:
e)` reads **one** row of the selected position; if the position holds several
peers the query is refused (make the order key unique within the partition, or
reduce with a range). A range's endpoints are normalised (`from previous to 2
previous` ≡ `from 2 previous to previous`), a range that straddles the current
position includes it, `every d` steps from the lower endpoint, and `else v`
contributes `v` once per missing position. `else` and `all` are mutually
exclusive; distances run from 1 to 999. `else v` is a constant.

**NULL / boundary policy** (deck p49–50):
- A NULL **consumed by a reducer** is ignored while any non-NULL value remains
  (the reduced-NULL case — still a valid constraint).
- A **non-reduced NULL** that reaches the boolean body **skips that constraint
  instance** (not an error — distinct from the "NULL to the solver is an error"
  rule, which is about *data* values, not missing frame positions).

### 7.4 Conditional generation (`when` / `if`)

For `E = [when θ][per K][if b]: B` (deck p58–59), θ is known and `b` contains
decisions. For each class `C ∈ Rθ/κ`, the imposed instance is the implication:

```
instance(C) = ⟦b⟧C ⟹ ⟦B⟧C
```

Require `κ → b`. `θ` (known) filters *rows* before generation; `b` (unknown)
guards the *constraint*, lowering to an indicator / big-M implication (stage 05).
`b` is a BOOL decision, its negation, a TEXT comparison, a linear comparison over
integer-valued decisions (`<=`, `<`, `>=`, `>`), or an `AND` / `OR` of BOOL
literals (`if open and not closed`, which is the linear guard `open - closed
>= 1`). A guarded body is linear.

### 7.5 Objectives

- `MINIMIZE/MAXIMIZE e` evaluates `e` once at `PER ()`.
- `A THEN B THEN …` is lexicographic: solve for A's optimum, freeze it as a
  constraint (or use the backend's lexicographic API), then optimize B, etc.
  "Optimum" is the solver's: the frozen row admits a relative slack of `1e-6`
  (exact for integer-valued objectives), so a later `REAL` stage may move an
  earlier stage within that tolerance. A later stage that is unbounded under
  the earlier optima is an error naming the stage.
- `SATISFY` sets no objective; any feasible point is returned.

---

## 8. Lowering to the pipeline

How each construct threads the eight stages. **New behavior must live with the
layer that owns it** (`.claude/CLAUDE.md`), not beside its first caller.

### 8.1 Stage 01 — parser
- New keywords (§3), gated frame/`by`/`within`/`at` tokens.
- Grammar productions for `decl_list`, `scoped_bool`, `reducer`, `frame_expr`,
  `objective_decl`.
- Transformer builds the §5 AST, attaching postfix `BY`/`OVER` to the correct
  preceding node (association is grammar-owned; never re-associated later).

### 8.2 Stage 02 — binder
- Resolve keys and expand relations (§6.1); resolve `κ`, `γ`, `π`.
- Prove the FD conditions (§6.3); apply the rejection catalogue (§6.4).
- Type the new domains; mark reducers/frames for later lowering.

### 8.3 Stage 03 — logical plan
- `LogicalDecide` gains: per-declarator generation keys, reducer group keys,
  frame partitions, guard expressions, lexicographic objective list. Extend
  serialization for each (round-tripped by `DECIDB_VERIFY_SERIALIZER=1`).

### 8.4 Stage 04 — canonicalizer
- Still the *one* shape boundary. Each generated constraint instance
  (per class, per signature) is canonicalized once: decisions left, bound right.
  Frame-derived terms and reducer terms are canonicalized like any other term.

### 8.5 Stage 05 — optimizer / rewriting
- `if` guards ⟹ indicator / big-M implications.
- Frame ranges / cyclic wrap ⟹ concrete index sets over the materialized
  timeline (may reuse the min/max and abs machinery already present).
- Lexicographic `THEN` ⟹ staged solves or a backend lexicographic call.

### 8.6 Stage 06 — model formulation
- New domains: `SEMIREAL`/`SEMIINT` ⟹ a binary "on" var + range bounds;
  `TEXT` enum ⟹ one-hot binaries with a sum-to-one constraint.
- Multi-column generation keys ⟹ variable indexing by composite key
  (`VarIndexer` extension).

### 8.7 Stage 07 — solver
- Indicator constraints and lexicographic objectives: Gurobi-native where
  available; big-M / staged-solve fallback for HiGHS. Gate genuinely
  unsupported classes via `SolverModelClass` (as SOCP is gated today).

### 8.8 Stage 08 — execution
- Materialize the buffer, build entity mappings per generation key, evaluate
  frame timelines and reducer groups against the buffer, enumerate constraint
  instances (and signatures under Variant Signature), solve, map values back by
  scope. Non-reduced-NULL instances are dropped here, not errored.

---

## 9. Conformance examples (from the deck)

Every example below must parse, bind, lower, and solve to the deck's stated
meaning. These become the golden/oracle tests under `test/decide/`.

**ANR generation × aggregation matrix** (deck p17–25):

| # | Query (abbreviated) | Meaning |
|---|---|---|
| 1 | `: ship <= 0.20 * sum(: ship) by ()` | tuple per, global agg |
| 2 | `per (): sum(: ship) by () <= netCap` | global per, global agg |
| 3 | `per D.depotID: reserve >= share * sum(: S.demand) by ()` | local per, global agg |
| 4 | `per D.depotID: sum(: ship) by (D.depotID) <= D.capacity` | local per, local agg |
| 5 | `: ship <= share * sum(: ship) by (D.depotID)` | tuple per, local agg |
| 6 | `per D.depotID: reserve >= 0.10*sum(S.demand) by (D.region) + 0.02*sum(S.demand) by (D.country)` | multiple aggregations |
| 7 | `per D.depotID: sum(when S.priority: ship) by (D.depotID) <= D.priorityCapacity` | filtered local agg |
| 8 | `per S.shipmentID: ship <= 0.25*sum(: ship) by (D.depotID, S.dispatchDay + S.transitDays)` | derived group key |
| 9 | `per D.depotID: sum(when S.priority per S.customerID: max(: ship) by (S.customerID)) by (D.depotID) <= cap` | nested reducers |

**Frame examples** (deck p44–50): previous-position balance; rolling range
`from previous to next`; nested frame aggregation; two different `over` orders in
one abs; cyclic wrap; boundary policies (`else NULL` / `else 0` / `all`); the
reduced-NULL vs non-reduced-NULL skip distinction.

**Conditional** (deck p58): `TEXT status ∈ ['closed','open','repair']` +
`per D.depotID if status='open': sum(ship) by (D.depotID) <= D.capacity`.

**Objectives** (deck p63): lexicographic nurse scheduling
`minimize sum(understaffed) then minimize sum(overtime) then maximize sum(pref*work)`.

**End-to-end** (deck p73–75): the multi-period production plan (balance, ramp,
opening/closing endpoints, objective) expressed with `within product` and no
outer `per` — the canonical demonstration that timeline navigation is *not*
constraint generation ("the per Trap").

---

## 10. Open decisions (must settle before implementing past them)

- **OPEN-1** — `per` placement in declarations: prefix (this spec) vs trailing.
- **OPEN-2** — FD-failure policy: reject-first vs Variant Signature default (§6.3).
- **OPEN-3** — `norm`/L2 spelling: squared vs square-root Euclidean norm, and
  the SOCP direction restriction (inherited from
  `03_expressivity/problem_types/todo.md`).
- **OPEN-4** — Backend reach: which new classes (indicator, lexicographic,
  semicontinuous, one-hot text, cyclic frames) target Gurobi+HiGHS now vs are
  gated as Gurobi-only or unsupported. Slide 2 warns some features need CP-SAT /
  Hexaly / IBM CP entirely.
- **OPEN-5** — `SATISFY` vs the current "omit objective" spelling: keep both, or
  make `SATISFY` mandatory in the new surface.

## 11. Migration from the current `DECIDE`

The clean redesign changes meaning, so it is **not** source-compatible with
today's queries. The breaking points:

| Today | New | Note |
|---|---|---|
| `PER col` (grouping a constraint) | `per col:` (generation) **and** `by (col)` (aggregation) split | The "per Trap". A current `SUM(x) <= K PER g` becomes `per g: sum(x) by (g) <= K` (local/local) or `per g: sum(x) by () <= K` (local/global) — the two were indistinguishable before. |
| bare `x(TYPE)` row-scoped | `x(TYPE)` still row-scoped (`per` omitted) | unchanged default |
| `T.x(TYPE)` entity-scoped | `per T: x(TYPE)` (relation expands to key) | generalized to arbitrary keys |
| `scalar x(TYPE)` | `per (): x(TYPE)` | global generation |
| omit `MAXIMIZE/MINIMIZE` | `SATISFY` | (OPEN-5) |
| postfix `WHEN` | `when θ` (filter) or `if b` (guard) | split by known vs decision |
| bounds in `SUCH THAT` | declaration-level `between … and …` | additive; SUCH THAT still allowed |

A migration path (each is a separate, testable step; ordering by cost):

1. **Additive, non-breaking first**: declaration-level bounds, `SATISFY`,
   `THEN` objectives, extended domains — none touch `PER` semantics.
2. **The `per`/`by`/`within` split** — the breaking core; do it once, migrate
   the whole test suite with it, keep the old grammar behind nothing (clean
   redesign, no dual mode per the chosen compatibility stance).
3. **Frame selectors** — the largest new subsystem (stages 05/06/08).
4. **Variant Signature** — only if OPEN-2 resolves toward it.

---

*Provenance: NYUAD "DeciQL proposals" deck (Mai, HUDA Lab), 76 slides. This spec
consolidates its scattered grammar fragments and denotational rules into one
definition and maps them onto DeciDB's eight-stage pipeline. It records design
intent; it is not a claim about implemented behavior.*
