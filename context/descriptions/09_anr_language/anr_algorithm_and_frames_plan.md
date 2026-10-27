# The full ANR algorithm and frame selectors — implementation plan

Status: plan, nothing built. Baseline commit 38275dde6f (branch `Sami-Active`), reviewed
27 October 2026 against `~/Desktop/Capstone/ANR.pdf` (the "DeciQL proposals" deck, 119
pages) and `ANR Implementation.pdf`. It supersedes the first draft, which was never
committed and is no longer in the repo; the section "Why this plan differs from the
first draft" says what changed and why. When a phase ships, its content moves into `done.md`
and the owning stage's `done.md`, and this file shrinks (README convention).

What it covers: variant signatures (deck pp. 34–40), reducers nested to any depth with
`when`/`per`/`by` at every level (pp. 16–32, 79–97, 111–119), point and range frame
selectors with `else`, `all`, `every`, `cyclic`, `within` (pp. 42–56, 66–75, 99–108), in
constraints, in reducer operands and in the objective. Not covered, by the first
draft's choice and kept here: declaration bounds, `SEMIREAL`/`SEMIINT`/`TEXT`, `if`
guards, `then`/`satisfy` (pp. 9–11, 58–63).

---

## 0. The approach in one paragraph

Everything the deck asks for is reachable from three primitives the engine already has
or nearly has, and nothing needs a second engine:

1. **A wider grouping key.** A variant signature (deck p. 37) is the tuple of *values* a
   scoped expression's direct terms take on a row: tuple terms, `by` keys, decision
   instances, frame values. Grouping the kept rows by `per key ++ signature` instead of
   `per key` alone turns every "does not determine" error into one generated instance per
   distinct combination. `BuildGroupIds` already groups by a composite key with NULL a
   value; the walker that lists a clause's parts (`BuildClauseScopeChecks`) already exists.
2. **Recursive helper decisions.** Item 5's value rewrite already turns a reducer into a
   helper decision with one instance per `by` group, defined once, read by the rows that
   need it (`RewriteScopedReducers`, `EvaluateReducerValueLinks`,
   `LinearizeReducerValueLinks`). A reducer nested inside another is the same helper keyed
   by the **path** of enclosing `by` keys, over the rows kept by the path of enclosing
   `when`s, whose members are the inner scoped expression's generated values — each of
   which is a linear expression over deeper helpers, decisions and data. Deck pp. 111–119
   give the same lowering for the extremum (`x_{1,2} = max(x1, x2)` becomes a column) and
   inline the sums, which phase 7 does; until then a nested SUM is a helper too.
3. **A frame map.** A frame selector is a per-row navigation computed at execution:
   partition by `within`, order by the `over` key, find the selected position(s) for each
   anchor row. A point selector with a decision payload is a term that reads another
   row's instance, which is how a keyed decision is already read through the entity
   mapping, so it inlines into the row's own model row from the start (a per-term row
   redirection, P9); a range selector, and any frame inside a reducer operand, lowers
   through primitive 2 (a helper per anchor row). Data-only frames are values, like any
   data expression. Range sums inline later as an optimization.

So the work is: lift the nesting gate, replace the determination checks by signature
grouping, generalize helper keys and membership to paths, add frame syntax and the frame
map, propagate NULL/absence, and retire the two-level objective special case. One
language, one path, the existing tests kept green on both solvers at every phase.

---

## 1. Why this plan differs from the first draft

The first draft was verified claim by claim against the code (two surveys, 27 October).
Its code references are accurate with these corrections: `physical_decide.cpp:63-73`
(`CollectReducers`) only serves the *bound side's* data-only reducers — a decision-side
reducer never reaches execution as an aggregate, it is flattened into `DecideTerm`s by
`BuildDecidePreparedModel` at plan time; `BuildSolverInput` lives in `physical_decide.cpp`
(3662), not `ilp_model_builder.cpp`; the optimizer passes are not "unconditional" — the
whole pass can be disabled and each pass guards itself; no ANR source or test file
exists yet; and the by-key subquery provenance gap at `plan_select_node.cpp:52-82`
(`collect_subqueries` walks constraints and objective only, not `decide_group_keys`) is a
pre-existing gap, not an ANR requirement.

What changes, and why:

| First draft | This plan | Why |
|---|---|---|
| Six new components (parsed frame node, bound ANR program, ANR binder, runtime state, symbolic evaluator, ANR formulation) dispatched *before* the canonicalizer and optimizer, bypassing the flat rewrites, with new sparse objective, quadratic, bound and provenance contracts, then a phase-8 migration of the old path | Extend the existing path: the value rewrite recurses, grouping keys widen, execution gains a frame map and an absence mask | `.claude/CLAUDE.md`: consolidate rather than add a parallel abstraction; one owner per concept. A bypass branch must re-implement ABS, `IN`, `norm`, bilinear, `<>`, bound absorption, implied bounds, elastic repair and DIAGNOSE provenance before it reaches parity, and until then two engines answer the same language. The deck's own formulation (pp. 111–119) is the helper lowering. |
| Signature machinery for frames: caller anchors, retained witnesses, deferred signature finalization, caller-fallback identity | A frame term contributes its `within` key and, for a relative selector, its order key to the enclosing signature, which is what deck p. 99 stores (σ = (tid, inventory, product, period)); absence is a row mask, not a key (P13) | The anchor keys are the deck's own rule, and the first draft follows it for relative selectors; what it adds beyond the deck is the witness and deferred-finalization machinery for a fallback whose identity the anchor row already fixes. A fallback that is not reached is never evaluated (selection vectors). |
| "Add an explicit flat-column linear objective interface" and "a general sparse quadratic accumulator" | Neither | A helper is a decision column; the objective reads it through the entity mapping and counts it once per instance with the existing inner-`per` de-duplication (`sum(per g: h)`). The value rewrite does not walk the objective today, so phase 2 extends it there (§4.5), the way the ABS, `norm` and `<>`/AVG rewrites already do. Quadratic forms keep today's limits (§4.6). |
| Account allocations against DuckDB memory limits; a construction wall-clock budget; work with the DECIDE optimizer disabled | Interrupt polling and an expansion limit; the optimizer-off path is an open bug (`06_issues/bugs/todo.md`) and stays one | Scope. Memory accounting of plain vectors is a cross-cutting change no stage asked for. |
| Horizontal phases (every interface first, migration at phase 8) | Vertical phases, each shipping on the single path with the suites green on both solvers | A phase whose exit gate is "the suites pass" is observable; "interfaces defined" is not. |
| Variants everywhere except the frame partition, decided inside the plan | The same, listed as an explicit decision with the trap it hides (P2, P6) | Variants silently stop an inner `per` from de-duplicating when the body varies (D63 today refuses it). The user decides. |

Kept from the first draft, unchanged in substance: variants are the semantics of every
scoped expression; signature fields are maximal reducer/frame-free scalar subtrees as
written; the cyclic position equations; `else` per missing position and `all`;
present-NULL distinct from absent; the Boolean boundary at each comparison; an
all-NULL reducer is NULL; two-sided (exact) extremum links for a shared or nested
MIN/MAX; implied bounds before Big-M; the uncached oracle; the dual-backend and
native-mode matrix; the deck fixtures with hand-computed answers.

---

## 2. Decisions needed before coding

All sixteen were accepted as recommended on 28 October 2026 and are decisions
**D64–D79** (P1 is D64, P2 is D65, … P16 is D79). Code and tests cite them by their `D`
number from phase 1 on; each enters the decision table of `done.md` when the phase that
implements it ships.

| # | Decision | Settled as |
|---|---|---|
| P1 (D64) | **Variants replace determination errors** for tuple terms, `by` keys and decisions outside reducers, at every scoped expression, in constraints and reducer operands (deck p. 40: "Variant Signature is the semantics of every scoped expression"). The messages *"per K does not determine …"* disappear except for the objective. | Yes. Deck p. 39 proves the old answers are kept wherever the old rule held. |
| P2 (D65) | **The inner `per` and the colon form** `SUM(D: e)`: under P1, `sum(per D: D.opening_cost + T.unit_cost * ship)` over a depot with two routes stops being the D63 error and silently counts the depot twice (one member per distinct `(unit_cost, ship)`), which is the per-row answer the user wrote `per D` to avoid. Keep the error for an inner `per`, or follow the deck? | Follow the deck (one rule), and make EXPLAIN print the variant count of every scoped expression that produced more than one per class (§4.8), so the surprise is visible in the plan. Record the trap in `syntax_reference.md` §5.3. |
| P3 (D66) | **The objective** is produced once (deck p. 62). A `per ()` signature with more than one value is an error. | Yes; reword the existing message to say the objective would be produced more than once (§3.6). |
| P4 (D67) | **Point selection at a peer with several rows** (equal order key): `at(previous: x)` where the previous position holds two rows with distinct payloads. Error, variants, or first row? | Error now (§3.4), naming the peer; an error can be lifted to variants later without breaking an answer. A range selector takes every row of a selected peer. |
| P5 (D68) | **NULL order keys** are ordered as `ORDER BY` orders them under the connection's `default_null_order` (DuckDB: NULLS LAST unless changed). | Yes. One rule the user already knows. |
| P6 (D69) | **The frame partition rule** `per K` → `within P` (deck pp. 53–55) stays an *error* although P1 turns the other two dependencies of the same theorem into variants. | Follow the deck (error, §3.4). Lifting it to variants later is compatible. |
| P7 (D70) | **Cyclic ranges** keep every repeated visit: on three peers, `from 4 previous to 4 next` visits nine positions. Alternative: clamp to one cycle. | Keep repeats (the literal reading, first draft's equations). |
| P8 (D71) | **`k` and `every`** must be positive whole-number constants. Row-varying offsets are a later proposal. | Yes. |
| P9 (D72) | **Point selectors inline, ranges lower to helpers.** A point frame with a decision payload in a Rows-shape clause is a term read at the selected row (a per-term row redirection, §4.7); a range frame, and any frame inside a reducer operand, is a helper per anchor row with a definition row each. Range sums inline later (phase 7). | Yes. The redirection is how a keyed decision is already read at a row; without it the inventory-balance model (deck pp. 73–75, F2) doubles in columns and rows. The helper path reuses item 5 end to end for the rest. |
| P10 (D73) | **Nested MIN/MAX exactness**: a MIN/MAX helper consumed by anything but a linear row of known sign gets both link sides (exact); sign is composed through SUM/AVG helper equalities only. | Yes (first draft agrees). One-sided envelopes for deeper consumers need a separate proof. |
| P11 (D74) | **What stays refused** (§3.7): `norm` inside a reducer, a product or square inside a nested operand, a product of a frame term with a decision, a reducer or frame inside a frame payload, a frame in `when`/`per`/`by`/`within`/the order key. | Yes, each with a message that names the construct. |
| P12 (D75) | **Two-level objective** `outer(per k: inner(e) by (k))` (D2) keeps its spelling and loses its special path: it is a nested reducer. Its four bind-time refusals lift. | Yes. |
| P13 (D76) | **A frame term's signature fields** are the frame's `within` key and, for a relative selector (`previous`, `next`, a range using them), its order key — deck p. 99 (σ = (tid, inventory, product, period)). Not the frame's resolved value: two anchors whose selected rows hold the same data are two variants. | Follow the deck. It also keeps every signature field a data expression, so `BuildGroupIds` needs no new key source (§4.7). F19 pins the difference. |
| P14 (D77) | **Field decomposition.** A direct tuple term is a maximal reducer-free, frame-free subtree of the body (today's `CollectClauseParts`, `decide_canonicalizer.cpp:1272-1285`), read on the canonical bound tree so written associativity cannot change the count. Deck p. 110's container sketch stores `a + b` as `(a, b)` (leaf decomposition). | Keep today's rule; the deck's is a container sketch, not a stated semantics. F20 pins both the count and the associativity invariance. Beyond the deck, recorded as such. |
| P15 (D78) | **A present NULL payload** (`at(previous: T.demand)` selecting a row whose `demand` is NULL) is today's NULL coefficient or bound error, not a NULL that a reducer ignores. Deck pp. 45 and 50 let a NULL flow into a reducer. | Keep the error. Beyond the deck, recorded as such. |
| P16 (D79) | **Absence under the tightest-bound collapse** (a data-only frame as the bound of a grouped reducer with no `per`, §5 "the bound rule"): an absent anchor contributes no bound; the group takes the tightest among the present anchors; a group with no present anchor has no row. | Yes. Beyond the deck, F21 pins it. |

---

## 3. The semantic contract

This is the text to fold into `00_project_overview/syntax_reference.md` (§5.5 nesting,
§5.6 frames, §7.1 rewritten, §10 updated) and `03_expressivity/scope/done.md` in phase 0.
Nothing here is implemented; the tests of §6 pin it.

### 3.1 Scoped expressions, recursively

Grammar as today, with the frame terms of §3.4 added to the body:

```text
E ::= [when θ] [per K] : B                      -- scoped expression
B ::= f(ē1, …, ēp, G1, …, Gq)                   -- scalar composition of tuple terms and reducers
G ::= agg(E) [by (Γ)]                           -- reducer: SUM, AVG, MIN, MAX, COUNT, norm
ē ::= e | at(F [else v]: e) over (O) | agg(W [every d] [else v | all]: e) over (O)
```

Evaluating `E` on a relation `R`: keep `S = {t ∈ R : θ(t)}`; split `S` into classes by
`κ` (`per row` → one class per tuple occurrence, `per ()` → one class, `per K` → one per
distinct key value, NULL a value, D5); for each class `C`, evaluate the body once per
**signature** (§3.2). A reducer `G = agg(E') by (Γ)` evaluated for a class `C` reads the
group `U = {u ∈ S : Γ(u) = Γ(t)}` for the class's representative `t` — all of `S`, not
only `C` (deck p. 35) — and evaluates `E'` on `U`; `agg` combines the list `E'` returns.

**The path rule** (what makes this implementable with keys): a reducer at depth `d`,
under enclosing reducers with keys `Γ1 … Γd-1` and enclosing scoped expressions with
conditions `θ0 … θd-1`, reads the rows of the *clause's* kept input that satisfy
`θ0 ∧ … ∧ θd-1` and agree with the current row on `(Γ1, …, Γd-1, Γd)`. So every reducer
instance is identified by a tuple of key values, the **path key**, and the engine never
materializes a relation per scope.

### 3.2 Variant signatures

For a class `C` of `E`, the signature of a row `t ∈ C` is
`σ(t) = (⟦ē1⟧t, …, ⟦ēp⟧t, Γ1(t), …, Γq(t))` (deck p. 37): the values of the direct tuple
terms and the `by` keys of the direct reducers. `E` produces one body value per distinct
signature in `C`, each evaluated at that signature's first kept row; the class's values
are a list, and `E`'s result is the concatenation over classes, multiplicity kept
(p. 38). At the Boolean boundary every value is a constraint instance; inside a reducer
every value is a member.

- **A direct tuple term** is a maximal reducer-free, frame-free scalar subtree of the body
  as written, compared by value: `per g: x <= a + b` over rows `(1, 2)` and `(2, 1)` has
  one signature value 3. Constants are terms with one value.
- **A decision** is compared by *instance*: a per-row decision is its row, a keyed one its
  class, a query-wide one itself. `per D.depotID: ship <= 10` with `ship` per row
  generates one instance per row.
- **A frame term** contributes the frame's `within` key and, for a relative selector, the
  anchor's order key (deck p. 99; P13): two anchors in one class that look at different
  positions are two variants, two that look at the same position are one. `first` and
  `last` contribute the `within` key only. Absence (§3.5) is not a field but a mask on
  the rows.
- **`per row`** keeps every tuple occurrence, including two rows equal in every column.
- **NULL** is one value in a signature; a NULL coefficient or bound is still the §3 error
  on the instance that reads it.
- Equal body values from different classes stay separate instances; identical model rows
  collapse at formulation, as today.
- **The objective** has one class (`per ()`) and must have one signature, else an error
  names the differing terms (P3).

Conservative extension (p. 39): wherever today's determination rule holds, every class
has one signature and the generated model is today's.

### 3.3 Nested reducers

A reducer's members are the values its operand `E'` generates on the group it reads
(§3.1). For each kind:

| Reducer | Over members `m1 … mn` (NULLs removed first, §3.5) |
|---|---|
| `SUM` | `Σ mi`; a linear expression over helpers, decisions and data |
| `AVG` | `SUM / n`, `n` the number of non-NULL members — classes × signatures, not rows |
| `MIN` / `MAX` | an extremum helper over the members; exact (P10) |
| `COUNT` | data only at every depth: `count(*)` counts members, `count(e)` the non-NULL ones |
| `norm(e, p) by (Γ)` | as today at the root of a comparison or objective; refused inside a reducer (P11) |

- An inner `when` filters the members' rows; an inner `per` picks the classes among
  them; an inner `by` picks the group among the rows the enclosing path keeps.
- An empty member list caused by a `when` is the D8 error naming the reducer and the
  group, as today. An all-NULL member list (frames only) is NULL, not an error.
- Deck p. 25 / example 9 and the running example (pp. 79–97) are the acceptance fixtures
  (F1); three levels (`sum(per h: max(per k: sum(x) by (g, h, k)) by (g, h)) by (g)`) are
  tested by the grid of §6.
- The two-level objective is this rule applied to the objective (P12).

### 3.4 Frame selectors

Spellings and defaults (deck p. 43, 56):

```sql
at(F [else v]: e) over (S [asc | desc] [cyclic] [within P])
agg(W [every d] [else v | all]: e) over (S [asc | desc] [cyclic] [within P])
F ::= first | last | [k] previous | [k] next        -- k defaults to 1
W ::= from F1 to F2
```

Defaults: `asc`, not cyclic, `within ()` (one timeline over the rows), `every 1`,
`else NULL`. `k` and `d` are positive whole-number constants (P8). `e` and `v` are scalar
expressions over data and decisions with no reducer and no frame inside (P11). `S` is a
data expression; `P` is a key like `per`'s (columns and relations, `()` for one
timeline).

**Timeline.** For a frame inside a scoped expression evaluated on `S`, partition `S` by
`P`; the enclosing `per` key must identify one partition per class (P6), else
*"DECIDE: per P.category does not determine the timeline T.productID of at(first:
inventory) over (T.period within T.productID). P.category = 'Food' spans T.productID =
'A', 'B'."* Inside a partition, rows with equal `S` form one **peer**; peers are ordered
by `S` in direction δ, NULL keys ordered as `ORDER BY` orders them (P5). Positions are
`0 … n-1` over peers, not over distinct numeric distances: periods 1 and 10 are adjacent
if nothing lies between.

**Navigation** for an anchor row at peer position `a`:

```text
first = 0, last = n-1, k previous = a-k, k next = a+k
range from L to U: positions q = L + j*d for j >= 0 while q <= U   (inclusive, step d)
L > U: the range is empty (also under cyclic)
noncyclic: q outside [0, n) is missing
cyclic: q maps to ((q mod n) + n) mod n; repeated visits are kept (P7)
```

`first`/`last` do not depend on the anchor; `previous`/`next` do, so under `per K` the
anchor's order key is a signature field and splits the class into one variant per
position (§3.2, P13) — this is the intended reading of "tuple-level expressions must be
constant on the partition" (deck p. 71).

**Point selection** returns the payload at the selected peer. A selected peer holding
more than one row with distinct payloads is an error (P4): *"DECIDE: at(previous:
inventory) over (T.period within T.productID) finds 2 rows at T.productID = 'A',
T.period = 1; a position must hold one row."* Identical payloads (the same decision
instance, the same value) select once.

**Range aggregation** reads the payload on every row of every selected peer, repeated
visits included, and combines them with `agg` as §3.3 (SUM, AVG, MIN, MAX, COUNT).

**Fallback and completeness.** `else v` supplies `v`, evaluated at the anchor row, for
each requested position that is missing, and only then (an unused fallback is never
evaluated, so one that would fail is no error). `all` makes the whole frame absent when
any requested position is missing. Without either, a missing position contributes
nothing to a range and makes a point selection absent.

**Payload evaluation.** `e` is evaluated at the *selected* row: `at(previous:
2 * inventory + T.demand)` reads the previous row's `inventory` instance and `demand`.
A decision payload may be a per-row or keyed decision (read through its class at the
selected row) or a query-wide one.

### 3.5 Absence and NULL

A frame that selects nothing is **absent** (deck p. 50). Absence is not a NULL value:

| Where the absent frame sits | Effect |
|---|---|
| directly in a comparison (non-reduced) | that instance is skipped; no error, not D4 |
| in a reducer operand | that member is dropped; a reducer with no member left is absent |
| in the objective, non-reduced | error: the objective cannot be skipped |
| a range with some positions missing, no `all` | the present positions only |
| `all` with a missing position | absent |

A present payload that is NULL (a NULL `demand` at the selected row) is a NULL
coefficient or bound and keeps today's §3 error naming the expression (P15; the deck
lets such a NULL flow into a reducer, pp. 45 and 50, so this is a choice beyond it). A
`when` keeping no rows stays D4; a frame skipping every instance is not an error. A
data-only frame as the bound of a grouped reducer with no `per` follows P16: absent
anchors contribute no bound.

Top-level `AND` keeps splitting into independent comparisons: in
`at(previous: x) over (…) >= 10 AND x <= 10` the first period skips the first comparison
and keeps the second (regression test, §6).

### 3.6 Messages

New or changed (fixed phrases in bold are what tests pin):

| Situation | Message |
|---|---|
| objective with more than one signature | `DECIDE objective: 'w' takes more than one value over the kept rows (3, 5), so the objective **would be produced more than once**; move it inside a reducer or fix it with when` |
| frame partition | `DECIDE: per <K> **does not determine the timeline** <P> of <frame>. <class> spans <P> = <a>, <b>.` |
| ambiguous peer | `DECIDE: <frame> **finds** <n> **rows at** <partition>, <S> = <v>; a position must hold one row.` |
| bad `k` / `d` | `DECIDE: in <frame>, <k> **must be a positive whole number**` |
| reducer or frame in a payload | `DECIDE: a reducer **inside at(...)** is not supported; reduce outside the frame` (and the `over` form) |
| frame in a key or condition | `DECIDE: a frame selector **is not allowed in** when, per, by, within or an order key` |
| `norm` inside a reducer | `DECIDE: norm **inside a reducer** is not supported yet` |
| product of a frame term and a decision | `DECIDE: '<expr>' multiplies a decision by a value from another row, which is not supported yet` |
| expansion limit | `DECIDE: <frame> **requests** <n> **positions over** <rows> rows, above decide_frame_limit (<limit>)` |

Removed: every *"does not determine"* message outside the objective, including the
colon form's (D63) and the inner `per`'s; *"an aggregate inside an aggregate is not
supported yet"*; the four two-level objective refusals (`when` inside either reducer,
`per` inside the inner, `by` other than the `per` key, outer `avg`); the "not in the
language" line for variants and frames in §10.2.

### 3.7 Capability matrix

| Construct | Constraint, either side | Reducer operand | Objective |
|---|---|---|---|
| SUM/AVG/MIN/MAX nested to any depth, `when`/`per`/`by` at each level | yes | yes | yes |
| COUNT at any depth | data only | data only | data only |
| `norm(e, p) [by]` | at the root, as today | no (P11) | at the root, as today |
| ABS, POWER, a product of decisions | at the root, as today; over a helper as today | no (as item 5) | as today |
| point frame, data payload | yes (a value) | yes | yes |
| point frame, decision payload | yes (helper per row) | yes | yes |
| range frame SUM/AVG/MIN/MAX/COUNT, data payload | yes (a value) | yes | yes |
| range frame SUM/AVG/MIN/MAX, decision payload | yes (helper per row) | yes | yes |
| ABS over a frame term, `norm` over a frame term | yes (the frame term is a linear term) | no | yes |
| a frame term times a decision, a frame inside a frame or a reducer inside a payload | no | no | no |
| strict `<`, `>`, `<>` over a fractional helper | refused, as today | — | — |
| quadratic / bilinear | today's limits unchanged | — | today's limits unchanged |

---

## 4. Architecture by layer

Each subsection names the contract the layer exposes downward and the files that change.
Line numbers are the baseline's.

### 4.1 Parser (stage 01)

**Tokens.** The lexer (`third_party/libpg_query/src_backend_parser_parser.cpp:213-370`)
already emits `WHEN_DECIDE`, `BY_DECIDE` (after `)`) and the `PER_*` tokens only while
`in_decide_clause`. Add, under the same flag: `AT_DECIDE` for `AT` followed by `(`
(lookahead, as `PER` does) and `OVER_DECIDE` for `OVER` directly after `)`, both only at
the clause's own nesting depth, so a SQL window inside a `(SELECT …)` written in the
clause keeps its `OVER`. That depth is **new lexer state**: today the lexer tracks a CASE
depth and nested DECIDE clauses (`decide_case_depth`, `PGDecidePushLexState`), not
parentheses or subqueries; phase 3 adds a parenthesis depth that a `SELECT` opens and
tests it with a window inside a subquery of the clause. New unreserved keywords: `PREVIOUS`, `CYCLIC`, `EVERY`;
`FIRST_P`, `LAST_P`, `NEXT`, `WITHIN`, `ELSE`, `FROM`, `TO`, `ALL` exist. A function
named `at` inside a clause is written `"at"(…)` (the S7 rule).

**Productions** in `select.y` beside the reducer forms (3155–3217):

```yacc
c_expr:
  | AT_DECIDE '(' decide_frame_point opt_frame_else ':' a_expr ')' decide_over
  | func_name '(' decide_frame_range opt_frame_every opt_frame_policy ':' func_arg_list ')' decide_over
decide_frame_point:  FIRST_P | LAST_P | PREVIOUS | NEXT | Iconst PREVIOUS | Iconst NEXT
decide_frame_range:  FROM decide_frame_point TO decide_frame_point
opt_frame_every:     /* empty */ | EVERY Iconst
opt_frame_else:      /* empty */ | ELSE a_expr
opt_frame_policy:    /* empty */ | ELSE a_expr | ALL
decide_over:         OVER_DECIDE '(' a_expr opt_asc_desc opt_cyclic opt_within ')'
opt_cyclic:          /* empty */ | CYCLIC
opt_within:          /* empty */ | WITHIN '(' ')' | WITHIN decide_key_list
```

`decide_key_list` is `per`'s key list (columns and relations, no parentheses). `%expect`
stays 4 or the new conflicts are explained in `01_parser/done.md` "Conflicts".

**Parsed node.** One new `ParsedExpression` class, `DecideFrameExpression`
(`ExpressionClass::DECIDE_FRAME`), the way `WindowExpression` is a class of its own:
`kind` (POINT/RANGE), `from`/`to` (`{position, k}`), `every`, `all`, `payload`,
`fallback`, `order`, `descending`, `cyclic`, `within` (key list), and for RANGE the
reducer name. `Copy`, `Equals`, `Hash`, `ToString` (renders exactly the written form with
defaults omitted, D32), serialization through `nodes.json`. The transformer builds it
from the new productions. `RefuseRemovedDecideSpellings` and `decide_parsed_markers.cpp`
learn to descend into it.

**Tests.** `test/decidb/test_frame_parser.test` (sqllogic): every spelling parses and
prints back; defaults omitted on echo; `"at"` as a function name; a SQL window inside a
subquery of the clause still parses; `OVER` outside a clause unchanged; the exact parse
errors (`every 0`, `3 first`, `else` twice). `test/common/test_decidb_plan_serialization.cpp`:
round trips of every frame form.

### 4.2 Binder (stage 02)

- **Lift the gate** at `decide_binder.cpp:256-261` (`ValidateScopePlacement`): a reducer
  inside a reducer binds. `ValidateSumArgumentInternal` (1294–1304) recurses into the
  nested body. `IsTwoLevelObjective` (282–310), the `two_level` flag and
  `BindTwoLevelObjective` (`decide_objective_binder.cpp:68-156`) are deleted: the
  objective binds like a constraint body under `per ()` (P12).
- **Frames.** `DecideFrameExpression` binds to a wrapper tagged `FRAME_TAG`
  (`decide.hpp`, beside `PER_CLASS_TAG`) with children `{payload, fallback?}` and an
  alias `__frame_<i>__` naming `LogicalDecide::frames[i]` — the pattern `by (G)` uses
  with `__group_key_<i>__` (1771–1820). A RANGE frame binds its reducer as a
  `BoundAggregateExpression` whose group is the frame (the `__frame_<i>__` tag on the
  aggregate instead of `__group_key_<i>__`). `ValidateScopeData` (165–176) refuses a
  frame in `when`, `per`, `by`, `within` and the order key; a reducer or frame inside a
  payload or fallback is refused; `k`, `d` validated (P8); payload type is the frame's
  type, fallback cast to it with the binder's usual implicit cast.
- **Decision-bearing status** of a frame (payload mentions a decision) is a property the
  canonicalizer reads, like a reducer's.
- **Degree** (`decide_degree.cpp`): a frame term has the payload's degree; a product of a
  decision-bearing frame term with a decision is refused (P11).

**Tests.** The sqllogic file above for bind errors; `test_scope_objective.py`'s two-level
refusals become answer tests (§6).

### 4.3 Logical plan (stage 03)

- `LogicalDecide::frames` (`vector<DecideFrameSpec>`: bound order expression, direction,
  cyclic, kind, from/to, every, all, bound within-key expressions, written text). Serialized
  as property **220** (219 stays retired); `DecideFrameSpec` in `nodes.json` ids 100–110.
- `LogicalDecide::signatures` (`vector<DecideSignature>`: the scoped expression it
  belongs to — clause id, reducer site, or the objective —, its `per` key, and its fields,
  each `VALUE(expr) | DECISION(var) | BY_KEY(group idx) | FRAME(frame idx)`), property
  **221**, built by the canonicalizer in place of the `DETERMINES` checks (§4.4).
- `DecideScopeCheck` keeps `KEEPS_A_ROW` and `READS_A_ROW`; `DETERMINES` is retired from
  the builders (the enum value stays so serialized plans still read) and a new
  `FRAME_PARTITION` kind carries the `per` key and the frame index (§3.4).
- `ReducerValueLink` gains `path_group_keys` (the enclosing `by` key indices, outermost
  first), `path_whens` (the enclosing `when`s), `signature` (the operand's
  `DecideSignature` index) and `frame` (for a frame-grouped reducer). Still never
  serialized (optimizer-derived).
- `EnumerateExpressions` visits frame order/within expressions, payloads and fallbacks;
  `plan_select_node.cpp:94-104` plans subqueries in them and — closing the pre-existing
  gap — in `by` keys, with provenance tags (52–82).

### 4.4 Canonicalizer (stage 04)

- `BuildClauseScopeChecks` / `BuildReducerScopeChecks` / `BuildObjectiveScopeChecks`
  (1312–1525) become `BuildClauseSignature` and friends: the same walk over the bound
  clause before canonicalization, emitting one `DecideSignature` per scoped expression
  (the clause, every reducer operand at every depth, the objective) instead of
  per-field `DETERMINES` checks. Fields are the maximal reducer-free, frame-free subtrees
  (§3.2). `KEEPS_A_ROW` and `READS_A_ROW` are built as today; `FRAME_PARTITION` for every
  frame under a `per K` other than `per row`.
- `ClassifyScoped` (883–967): a comparison holding a nested reducer or a decision-bearing
  frame anywhere is **VALUE** (the helper shape). A data-only frame or nested data-only
  reducer is a value and stays on the bound side (S10). The AGGREGATE fast path is not
  widened in this plan (phase 7 inlines SUM helpers instead).
- `ValidateCanonicalComparison`, `VerifyCanonical`: a frame term is a tuple term.

### 4.5 Optimizer (stage 05)

`RewriteScopedReducers` (`decide_rewrite_scoped.cpp`) recurses, innermost first:

1. For a reducer at depth `d` with path keys `(Γ1 … Γd)` and path conditions, allocate a
   helper through `AddAuxiliaryDecision`, keyed by its path key (an `EntityScopeInfo`
   with `value_link`; `Scalar()` only when every key on the path is `()`), labelled by the
   reducer's SQL (`sum(when S.p per S.cid: max(ship) by (S.cid)) by (D.rid)`).
2. Replace the reducer node in its parent's body by the helper reference, so the parent
   operand is affine in helpers, decisions and data. A body that still multiplies
   decisions is refused as today.
3. For a frame-grouped reducer (RANGE), or a decision-bearing point frame, allocate a
   helper keyed per anchor row (`Row()` scope) whose link carries the frame index; the
   frame map at execution gives its groups.
4. Record the link with `path_group_keys`, `path_whens`, `signature`, the sides a MIN/MAX
   helper needs (`ExtremumPushOf`, composed through SUM/AVG definitions; both sides past
   any other consumer, P10), clause id and removal group (the clause's, as today — a
   helper never crosses clauses, stage 05 todo §1 unchanged).

**The objective is a named deliverable of phase 2, not reuse.** Today
`RewriteScopedReducers` walks `decide.decide_constraints` only
(`decide_rewrite_scoped.cpp:259-303`), the objective is canonicalized separately by
`CanonicalizeObjective` (`decide_canonicalizer.cpp:798-862`) with no VALUE shape, and
`Helpers::objective` serves only the `OBJECTIVE_ROW_TAG` rows the ABS, `norm` and
`<>`/AVG rewrites derive from the objective. Phase 2 adds: an objective VALUE
classification in `CanonicalizeObjective`/`ClassifyScoped` (a nested reducer or a
decision-bearing frame in the objective); the value rewrite walking `decide_objective`
under `OBJECTIVE_ROW_TAG` the way `RewriteAbs` (`decide_rewrite_abs.cpp:346`),
`RewriteNorm` (`decide_rewrite_norm_in.cpp:140`) and `RewriteNotEqualAvg`
(`decide_rewrite_notequal_avg.cpp:99`) already do, allocating helpers from the
objective; the outermost objective reducer then written as `sum(per <path key>: h)`,
`min`/`max` over the helper instances, or `avg` of them, riding the existing SUM, MIN/MAX
(PATH A of `LinearizeMinMaxObjective`, members being the helper read at each
instance's representative row through the inner-`per` de-duplication) and AVG paths;
and `RewriteAvgToSum` taking the member count execution reports for the outer AVG.
Only after that lands are `RewriteMinMaxObjectiveTree`'s two-level detection
(747–880), PATH B of `LinearizeMinMaxObjective` (`linearization_minmax.cpp:634-826`),
`DecideObjective::per_columns` and `PER_CONSTRAINT_TAG` deleted.

`AbsorbVariableBounds` recurses through helper definitions only for the easy
single-decision case already covered by D41; nothing new.

### 4.6 Model formulation (stage 06)

`LinearizeReducerValueLinks` states each helper's definition per instance from its
member list as today (an equality for SUM/AVG, `EmitExtremumLink` for MIN/MAX). Two
generalizations:

- A **member** may reference a helper instance (the inner helper read at the member's
  representative row) — already how a keyed decision is read at a row through the entity
  mapping, so the `ReducerValueLinkSpec` member format is unchanged if members stay
  `(row, per-term coefficients)` with the inner helper as one of the terms.
- **Order of definition**: links are emitted innermost first so `DeriveReducerValueBounds`
  boxes an outer helper from already-boxed inner ones; the fixed-point loop with the ABS
  derivation stays.

Implied bounds (`linearization_bigm.cpp:258`) and elastic widening
(`OpenElasticColumnBox`) read `EvaluatedConstraint`s only; a helper's definition is a
member list (`ReducerValueLinkSpec::rows`) until `LinearizeReducerValueLinks`
(`linearization_reducer_value.cpp:93-160`) emits it as rows, so no new sparse interface
is needed for either analysis.

**Bound flow today is one-directional, and phase 2 adds the other direction.**
`DecidePropagateImpliedBounds` (`physical_decide.cpp:3759`) boxes a helper from a
single-variable root row; `DeriveReducerValueBounds` (`linearization_reducer_value.cpp:64-91`)
boxes a helper from the reach of its *members*; nothing flows from a helper to its
members, and a MIN/MAX helper's closing Big-M is read from its own box (133–143). Two
consequences: a nested MAX consumed by a SUM under a root of known sign needs only its
envelope and no Big-M (P10), so F1 passes on both backends; a MIN/MAX helper that needs
its closing side over an unbounded member refuses on HiGHS under forced lowering with
*"requires a finite bound on 'x'"* — which `test_scope_value_rewrite.py:244` pins today
for one level. Phase 2 adds a backward pass beside `DeriveReducerValueBounds` in the
stage 06 fixed-point loop (`physical_decide.cpp:4005-4030`): a helper's box flows to its
members and inner helpers, through an equality for SUM/AVG and through the envelope for
MIN/MAX, so `sum(max(x) by (g, h)) by (g) = K` over otherwise unbounded `x` derives a
finite Big-M from `K`. One column box per helper still covers every instance
(`05_optimizer/todo.md` §2), and the cover widens with depth; phase 2 either takes that
item (a box per instance) or states the loosening as a limit with a dump pin. Pins:
the two-sided case above on HiGHS under `DECIDB_NATIVE_CONSTRUCTS=force`, as the named
refusal before the pass and a pass after it.

Quadratic objectives keep the single-group rule (`decide_linear_form.cpp:1402-1415`).

### 4.7 Execution (stage 08)

`PhysicalDecide::Finalize` (2073) keeps its order; three steps change and one is added:

1. **`BuildEntityMappings`** (2015): a helper's scope groups the rows its path keeps by
   its path key (`BuildGroupIds` over `path_whens`), NULL a value, dropped rows on one
   spare entity, as today for one level.
2. **Frame maps** (new, between entity mappings and the checks): for each
   `DecideFrameSpec`, over the rows its scoped expression keeps: partition ids by the
   within key (`BuildGroupIds`), the `FRAME_PARTITION` check per `per` class, a sort of
   each partition by the order key (DuckDB `OrderType` / `OrderByNullType` from
   `default_null_order`; the open collation bug applies here as to every key), peer
   indices, then for each anchor row the selected rows per §3.4 and an `absent` flag.
   Output `FrameMap { row → (partition, peer); row → selected rows; absent mask }`. The
   requested position count is checked against `decide_frame_limit` (a setting, default
   generous) before allocation; `context.interrupted` is polled per partition.
3. **`RunScopeChecks`** (2239): `KEEPS_A_ROW`, `READS_A_ROW`, `FRAME_PARTITION` only.
4. **Signature grouping** replaces the class loop of the rows shape and the inner-`per`
   de-duplication: `LookupOrBuildPerGroupIds` over the key `per key ++ signature fields`.
   Under P13 every field is a data expression the executor can evaluate — a `VALUE`
   field's expression, a `BY_KEY` field's key expressions, a `FRAME` field's `within` and
   order expressions, a keyed decision's entity-key expressions — or the row itself (a
   per-row decision makes the class per row, no key needed; a query-wide decision adds no
   field). So `BuildGroupIds` (436–470, 632–661) works unchanged on expression keys.
   One model row (or member) per group at its first kept row. Absence is a row mask
   applied after `when` (it does not feed D4): a row whose non-reduced frame is absent
   joins no group. `EvaluatedConstraint::rows_stand_with_their_class`, today `!has_when`
   (2610) and read by implied-bound soundness (`linearization_bigm.cpp:280-290`), is
   false for a class that emits more than one variant row, since no single row then
   stands for the class.
5. **Point frames in a Rows-shape clause** are not helpers (P9): the frame's term is a
   `DecideTerm` with a frame index, and its coefficient and instance are read at the
   selected row the map names (a per-term row redirection, the way a keyed decision is
   read at a row through the entity mapping; `INVALID` where absent). Stage 06 accumulates
   it like any term; implied bounds treat a row with a redirected term as multi-variable.
   F2 pins the model at 6 columns and 4 rows.
6. **`EvaluateReducerValueLinks`** (3583) recurses innermost first: for a link, the groups
   are its helper's entities (or the frame map's selected rows for a frame link); within a
   group, the members are the signature groups of its operand (step 4, restricted to the
   group's rows and the operand's `when`); a member's coefficients come from
   `EvaluateTermColumns` at its representative row, where an inner helper is read through
   its own entity mapping; a member whose frame field is absent is dropped; a group with
   no member left marks the helper instance absent, and absence propagates to the
   consumer (a root row referencing an absent instance is skipped; a member referencing
   one is dropped). AVG divides by the members left.
7. **Bound-side data reducers**: `CollectReducers` (63–73) and `EvaluateRhsReducerPerGroup`
   (1114–1247) recurse the same way, over values instead of columns; a data-only frame
   is evaluated from the map (payload at the selected rows, fallback at the anchor on the
   absent rows only, through `ExecuteOnReadRows` selection vectors); under the
   tightest-bound collapse an absent anchor contributes no bound (P16).
8. **Readback** is unchanged: helpers are never read back (item 7).

Provenance: every helper instance and every generated row carries its clause id,
`per`-key values and signature index; the DIAGNOSE `group` label (D28) prints the `per`
key value, followed by the variant's distinguishing data fields when a class has more
than one variant (`D.id = 'd1', D.rid = 'East', S.m = 'Air'`); the representative row
is the `data_row` of the expanded scope (D33). Atomic DROP keeps clause granularity;
definitions stay structural.

### 4.8 EXPLAIN and DIAGNOSE

- Echo: frames print as written with defaults omitted; a nested reducer prints nested.
- Accounts (`RenderDecideRewriteAccount`): "a helper decision per (D.rid, S.cid) group"
  for a path-keyed helper; "reads the previous period's inventory; a helper per row" for a
  point frame; "a sum over the frame from previous to next every 1; a helper per row" for
  a range; "generates 3 constraints: d1 has 2 variants over (D.rid, S.m, D.c), d2 has 1"
  under a clause whose classes produced more than one variant (P2); "skips 2 instances
  with no previous position" for absence.
- DIAGNOSE: §4.7 labels; a frame-generated definition is structural; the unbounded
  remedy and the relaxed-bound re-formulation see helpers as today.

---

## 5. Phases

Each phase is one review unit, ships on the single path, and ends with the full
verification list of §6.9 green. Phases 3–5 may start once phase 1's signature contract
is merged; phase 2 and phase 3 are independent of each other.

### Phase 0 — Contract, oracle, fixtures (no engine change)

- The contract is §3 of this plan. `syntax_reference.md` and
  `03_expressivity/scope/done.md` describe implemented behaviour only, so each phase
  updates them as it ships (phase 1: §7.1 and §10; phase 2: §5.5; phases 3–5: §5.6).
  Decisions D64–D79 are settled (§2).
- Build `test/decide/tests/_anr_oracle.py` (§6.2) and `_anr_cases.py` (§6.3), and the
  oracle's own tests (`test_anr_oracle_selfcheck.py`) reproducing the deck's hand-computed
  results: pp. 35, 36, 97, 108, 115, 119.
- Record baselines: the golden corpus on both solvers, `benchmark/decide` timings for the
  existing flat workloads (`profile_pipeline.py`).
- Exit: the oracle reproduces every deck value; the docs state every rule of §3.

### Phase 1 — Variants (layers 3, 4, 8; no new syntax)

- `BuildClauseSignature` family (§4.4), `LogicalDecide::signatures` (221), the execution
  grouping (§4.7 step 4), the objective singleton error (P3), EXPLAIN variant counts,
  DIAGNOSE variant labels; delete the `DETERMINES` builders and the two determination
  messages at `physical_decide.cpp:2310-2334`.
- Keep the `=` contradiction check (*"cannot hold two different values at once"*,
  `physical_decide.cpp:783-857`): with no `per` the clause is per row, so under the deck
  `sum(x) by (g) = cap` with two `cap` values is two instances that cannot both hold, and
  the check is an early diagnosis of that infeasibility, not a determination rule. Its
  four pins stay (`test_error_binder.py:237, 610`, `test_error_infeasible.py:183`,
  `test_scope_reducer_by.py:207`).
- Tests: `test_anr_variants.py` (§6.4, F19–F21 included). Conversions, every file named:
  `test_scope_determination.py` (every "does not determine" case becomes an answer
  judged by the oracle; the objective ones keep an error), `test_scope_combinations.py`
  (cells that expected `not_determined` now solve), `test_scope_reducer_inner.py` and
  `test_qualified_reducer.py` (the inner-key and colon-form checks; P2's trap pinned),
  `test_scope_deck_examples.py` (pp. 34–36), `test_error_infeasible.py:195`,
  `test_per_clause.py:322`, `test_reduced_bound_data_column.py:71`,
  `test_scope_value_rewrite.py`, `test_scope_bare_clauses.py`,
  `test/decidb/test_scope_parser.test`, and `_scope_oracle.py` (retire
  `ScopeError.not_determined` and the `check` at 219 and 323 once `ScopedModel`
  delegates). The `rows_stand_with_their_class` rule of §4.7 step 4 is a phase-1 contract.
- Exit: no "does not determine" outside the objective anywhere in the suites (a grep of
  `test/` is part of the gate); golden corpus identical or re-captured with every diff
  explained.

### Phase 2 — Nested reducers (layers 2, 3, 5, 6, 8)

- Lift the gate; recursive `RewriteScopedReducers` with path keys; recursive
  `EvaluateReducerValueLinks` and bound-side evaluation; definition order; the backward
  bound pass and the per-instance box decision (§4.6); **the objective deliverable of
  §4.5** (objective VALUE shape, the value rewrite over `decide_objective`, PATH A over
  helper instances, the outer AVG's member count); then delete the two-level special
  path (binder, `RewriteMinMaxObjectiveTree` 801–878, PATH B,
  `RenderTwoLevelObjective`/`RenderTwoLevelInner`, `per_columns`, `PER_CONSTRAINT_TAG`).
- Tests: `test_anr_nested.py` (§6.5); `test_scope_objective.py`'s refusals become
  answers; the "aggregate inside an aggregate" pins in `test_scope_bare_clauses.py` and
  `test_scope_reducer_by.py` become answers; the nesting grid; the two-sided HiGHS pin of
  §4.6.
- Exit: F1 (both variants) and the grid pass on Gurobi and HiGHS and under
  `DECIDB_NATIVE_CONSTRUCTS=on|off|force`; no "aggregate inside an aggregate" anywhere.

### Phase 3 — Frame syntax (layers 1, 2, 3; execution refuses at one gate)

- Lexer tokens and the new parenthesis-depth state (§4.1), productions,
  `DecideFrameExpression`, binding to `FRAME_TAG` and `LogicalDecide::frames` (220),
  `FRAME_PARTITION` checks, `EnumerateExpressions`, subquery planning, serialization,
  echo. Execution throws *"DECIDE: frame selectors are not supported yet"* from one
  place.
- Tests: `test_frame_parser.test`, the C++ round trips, `test_anr_frames.py::TestSyntax`
  (bind errors, echo, EXPLAIN of the plan before execution).
- Exit: every form parses, binds, round-trips and prints; ordinary SQL unchanged
  (`test/sql` windows and the sqllogic suite).

### Phase 4 — Point selectors (layers 4, 5, 6, 8)

- Frame maps; data-only point frames as values; decision point frames as redirected
  terms in a Rows-shape clause and as per-row helpers inside a reducer operand (P9);
  absence propagation; `else`; `cyclic`; `desc`; `k`; peer error (P4); the objective.
- Tests: `test_anr_frames.py::TestPoint` (§6.6): F2, F4, F7–F9, F15, F19, deck examples 1,
  4, 5, the DeQL comparison (pp. 73–75); F2's model-size pin (6 columns, 4 rows).
- Exit: F2's four comparisons and two skips, hand-pinned, at the pinned model size; both
  solvers.

### Phase 5 — Range selectors

- `from … to …`, `every`, `else` per position, `all`; SUM/AVG/MIN/MAX/COUNT over the
  selected rows; data-only ranges as values; decision ranges as helpers; the expansion
  limit and interrupt polling.
- Tests: `test_anr_frames.py::TestRange` (§6.6): F5, F6, deck examples 2, 3, 6; cyclic
  repeats; reversed ranges; `all` versus `else`.
- Exit: the range matrix of §6.6 passes on both solvers.

### Phase 6 — Composition, diagnostics, guards

- Frames inside nested reducer operands and reducers over frames (deck example 3);
  frames and nested reducers in one clause; `norm` and ABS over a frame term; the AND
  split regression; DIAGNOSE labels and DROP groups for every new row kind; `decide_frame_limit`;
  cancellation during map construction and membership evaluation.
- Tests: `test_anr_composition.py`, `test_anr_diagnostics.py` (§6.7, §6.8).
- Exit: every row of the capability matrix (§3.7) has a passing test or a pinned refusal.

### Phase 7 — Inlining, performance, closure

- Inline a SUM/AVG helper whose only consumer is a linear root row (deck p. 115 inlines
  the sums), and a range SUM/AVG frame in a Rows-shape clause, as redirected terms (one
  per selected position; stage 06 accumulates them like like-terms) — only with a model
  dump showing the smaller model and the suites unchanged. Singleton MIN/MAX groups
  collapse to their member (deck p. 112: `c2 → x3`).
- Benchmarks (§6.10); profile counters; docs: `done.md` per stage, `09_anr_language/done.md`
  items 8–10, `04_testing/scope/done.md`, `README.md`; delete this file's shipped parts.
- Exit: measured no regression on the flat workloads; the nested and frame workloads
  grow linearly in rows at fixed depth and width.

---

## 6. Tests

### 6.1 Principles

- Every semantic rule of §3 has a test whose expected value was computed by hand or by an
  oracle that reads no SQL, never by DeciDB. The deck's own numbers are pinned verbatim.
- Every refusal of §3.6 and §3.7 is a test; a lifted refusal's pin becomes its answer test.
- Mandatory fixtures run on Gurobi and HiGHS with explicit backend fixtures (not the
  ambient `DECIDB_FORCE_SOLVER` alone) and under the three `DECIDB_NATIVE_CONSTRUCTS`
  modes in separate processes; expected refusals are tests, not skips.
- The uncached `scope_solver` fixture judges everything new (`oracle_cache.py:47` hashes
  only the test function's source).
- Tiny tables are `VALUES` (`_scope_cases.table`), so TPC-H is never a prerequisite.
- Structural pins use EXPLAIN and the model dump (`dump_counts`), never internal names
  (`assert_no_internal_leak`).

### 6.2 The recursive oracle — `_anr_oracle.py`

A reference evaluator for the full language, replacing `ScopedModel`'s special cases
(`Nested`, `check`'s `not_determined`) once phase 2 ships; `ScopedModel` then delegates
to it so the existing suites keep their statements.

```python
# expressions, as plain Python, no SQL
Const(v) | Col(fn: Row -> value) | Var(name) | Add(a, b) | Mul(fn_or_const, e) | Abs(e)
Agg(kind, operand: E, by: key | None)                 # SUM AVG MIN MAX COUNT
At(frame: Frame, payload: Expr, fallback: Expr | None)
Range(kind, frame: Frame, payload: Expr, every=1, fallback=None, all=False)
Frame(order: fn, desc=False, cyclic=False, within: key | (), start: ('first'|'last'|'previous'|'next', k), end=None)
E(when: fn | None, per: PER_ROW | GLOBAL | key, body: Expr | Cmp(lhs, sense, rhs))
```

`evaluate(E, rows)` returns a list of values, each a symbolic linear form over
`(decision, instance)` plus `CONSTANT`, an `Extremum(kind, members)` node, `NULL`, or
`ABSENT`, following §3.1–§3.5 literally: filter, classes, signatures as frozen tuples of
*evaluated* direct terms (symbolic forms frozen by instance identity, never by solved
value), reducer groups over the filtered relation, frames by partition/sort/peers/positions.
`solve(model, clauses, objective, solver)` emits extrema as auxiliaries with pick
indicators (as `ScopedModel.solve_scoped` does) and strict senses stepped (D60);
`judge(answer, …)` checks DeciDB's rows against the same generated instances. The two
share one generator. `errors`: `kept_no_rows`, `reducer_empty`, `objective_variants`,
`frame_partition`, `ambiguous_peer`, with the phrases of §3.6.

Two checks the first draft insisted on, kept: signatures are frozen on symbolic identity
*before* any value is substituted, so two distinct instances that happen to solve to the
same number never collapse; and for tiny whole-number fixtures `status_grid(model,
domains)` fixes every native decision to each assignment of its domain, asks DeciDB for
the feasibility status of the fixed model, and compares it with the oracle's — the check
that catches de-duplication after substitution and lost multiplicity.

Self-check (`test_anr_oracle_selfcheck.py`): the oracle's generated instances for F1
equal the deck's three constraints (p. 97, p. 115 with the extremum nodes, p. 119 for the
SUM variant), F2's four comparisons (p. 108), F3's three-plus-one variants (p. 35, 36).

For nonlinear tiny cases (ABS over a frame, products) `_brute_force.py` enumerates.

### 6.3 Fixtures — `_anr_cases.py`

Hand-computed; each is also a self-check case of the oracle.

**F1 — the running example** (deck p. 80). One table `t(tid, id, rid, m, cid, p, d, c)`:

```text
1 d1 East Air    c1 T 20 30
2 d1 East Air    c1 T 15 30
3 d1 West Ground c2 T 10 50
4 d1 West Ground c3 F  5 50
5 d2 West Air    c4 T 25 45
6 d2 West Air    c4 T 15 45
DECIDE ship(INT) SUCH THAT ship <= 100 AND
per t.id: sum(when t.p per t.cid: max(ship) by (t.cid)) by (t.rid) + 0.10 * sum(t.d) by (t.m) <= t.c
MAXIMIZE sum(ship)
```

Generated: `max(x1,x2) + 7.5 <= 30`, `x3 + max(x5,x6) + 1.5 <= 50`,
`x3 + max(x5,x6) + 7.5 <= 45` (3 instances: d1 has 2 variants, d2 has 1). Optimum 218
(`x1 = x2 = 22, x3 = 0, x4 = 100, x5 = x6 = 37`). The `sum(…)` inner variant (p. 119):
`x1 + x2 + 7.5 <= 30`, `x3 + x5 + x6 + 1.5 <= 50`, `x3 + x5 + x6 + 7.5 <= 45`; optimum
159. Pins: EXPLAIN says 3 instances and names the two helpers; the dump has one
extremum helper instance per `(rid, cid)` group that holds two members and none for the
singleton `c2` after phase 7.

**F2 — previous inventory** (deck p. 99). `t(tid, product, period, cap)` rows
`(1,A,1,5) (2,A,2,3) (3,B,1,1) (4,B,2,4) (5,B,3,8) (6,B,4,2)`, `DECIDE inventory(INT)`,
`inventory <= t.cap AND inventory >= at(previous: inventory) over (t.period within t.product)`,
`MAXIMIZE sum(inventory)`. Generated: `x2 >= x1, x4 >= x3, x5 >= x4, x6 >= x5`; rows 1 and 3
skipped. Optimum 13. With `cyclic`: adds `x1 >= x2, x3 >= x6`; optimum 10. With
`at(next: …)` and `<=`: the same four rows; 13. With `desc`: `previous` under `desc` is
`next` under `asc`.

**F3 — variants** (deck p. 34). `P(depotID, region, shipment, lim)`: D1 East S1 100; D1
East S2 90; D1 West S3 80; D1 West S4 80; D2 East S5 50; D2 East S6 50. `DECIDE ship(INT)`,
`per P.depotID: sum(ship) by (P.region) <= P.lim`, `MAXIMIZE sum(ship)`: D1 → 3 variants
(East ≤ 100, East ≤ 90, West ≤ 80), D2 → 1 (East ≤ 50); optimum 130. With
`by (P.region, P.lim)`: `ship1 <= 100, ship2 <= 90, ship3 + ship4 <= 80, ship5 + ship6 <= 50`;
320. Today both are the determination error.

**F4 — cyclic ring** (deck p. 67). `s(loop, idx, cap)`: `(L,1,1) (L,2,10) (L,3,4)`,
`DECIDE load(INT)`, `load <= s.cap AND abs(load - at(previous: load) over (s.idx cyclic
within s.loop)) <= 2`, `MAXIMIZE sum(load)`: 7 (`1, 3, 3`); without `cyclic`: 8
(`1, 3, 4`, station 1 skipped).

**F5 — weekly step** (deck p. 67). 28 rows `d(contract, day)`, `DECIDE delivery(INT)`,
`delivery <= 5 AND sum(from first to last every 7: delivery) over (d.day within
d.contract) <= 10`, `MAXIMIZE sum(delivery)`: positions days 1, 8, 15, 22; every anchor
generates the same row, which collapses to one; optimum 130. `from 2 next to last every 7`
at day 1 samples days 3, 10, 17, 24.

**F6 — rolling with policies** (deck pp. 45, 49, 68). `t(product, period)` A, periods
1–4, `DECIDE produce(INT)`, `sum(from previous to next <policy>: produce) over (t.period
within t.product) >= 6`, `MINIMIZE sum(produce)`: default (truncate) 12; `all` 6
(ends skipped); `else 1` 10 (`x1 + x2 >= 5`, `x3 + x4 >= 5`).

**F7 — duplicate peers.** Periods `(1, 1, 2)` in one product: `at(previous: x)` at
period 2 is the P4 error; `sum(from previous to previous: x)` is `x1 + x2`.

**F8 — NULL order key.** A NULL period sorts last (P5): `at(last: x)` selects it.

**F9 — k > 1.** `at(2 previous else 0: x)` over five periods: rows 1, 2 take 0.

**F10 — scalar field.** `DECIDE per t.g: x(INT)`, rows `(g=1, a=1, b=2), (g=1, a=2, b=1)`:
`per t.g: x <= t.a + t.b` is one instance `x <= 3`; `per t.g: x <= t.a` is two
(`x <= 1`, `x <= 2`).

**F11 — join fan-out.** A join repeating a tuple twice: `per row` counts both; an inner
`per D` counts once when the body is determined and twice when it varies (P2's trap,
pinned both ways with EXPLAIN's variant line).

**F12 — a per-row decision under a key.** `per D.depotID: ship <= 10` is one instance
per row.

**F13 — objective variants** (P3): `MAXIMIZE sum(x) - w * s` with a row-varying `w` is the
reworded error.

**F14 — nested data reducer on the bound side.** `sum(x) by (g) <= avg(per h: max(v) by
(g, h)) by (g)` evaluated as values.

**F15 — smoothing in the objective.** `x >= t.d AND MINIMIZE sum(abs(x - at(previous else
0: x) over (t.period within t.product)))` with `d = (2, 5, 1, 4)`: optimum 5 (brute force
over `0..5`).

**F16 — three levels.** `per g: sum(per h: max(per k: sum(x) by (g, h, k)) by (g, h)) by (g)
<= cap` over a 2×2×2 key grid; oracle-judged.

**F17 — DIAGNOSE on a variant.** F3 with `lim` 50 made 0: the finding names
`P.depotID = 'D2'` and the variant `P.region = 'East'`; expanded scope reports the
representative row; DROP of the clause removes every variant and every helper
definition it owns.

**F18 — AND split.** `at(previous: x) over (…) >= 10 AND x <= 10`: the first period
skips the first comparison and keeps `x <= 10` (a violated cap stays violated).

**F19 — frame fields in a signature** (P13). `t(product, period, demand)` A, periods 1–4,
demand `5, 5, 5, 5`; `DECIDE x(INT)`; `per (): sum(x) <= sum(per t.product: at(previous:
t.demand) over (t.period within t.product))`, `MAXIMIZE sum(x)`. The inner `per product`
class has one variant per period (the order key is a field): members 5, 5, 5 (period 1
absent), bound 15, optimum 15. Comparing frames by resolved value would give one member
and 5; the test pins 15.

**F20 — field decomposition** (P14). F10's table: `per t.g: x <= t.a + t.b` is one
instance (`x <= 3`); `per t.g: x <= t.a + sum(y) + t.b` and `per t.g: x <= t.a + t.b +
sum(y)` generate the same number of instances (fields are read on the canonical tree).

**F21 — absence under the tightest bound** (P16). `t(g, period, cap)` with one group of
three periods, caps `9, 4, 7`; `sum(x) by (t.g) <= at(previous: t.cap) over (t.period
within t.g)` with no `per`: period 1 is absent, periods 2 and 3 pair the group sum with
caps 9 and 4; the group's row takes 4. `MAXIMIZE sum(x)` is 4.

### 6.4 `test_anr_variants.py` (phase 1)

F3 both forms; F10; F11; F12; F13; a NULL key value as its own variant; equal body values
in two classes stay two instances; `=` with two bound values is two instances (the old
"contradiction" message goes); `<>` keeps every exclusion; a keyed decision determined by
the `per` key is one instance; a `per ()` clause over a per-row decision is one per row;
EXPLAIN's variant line; DIAGNOSE's labels; `DECIDB_VERIFY_SERIALIZER=1` over the file.
Conversions listed under phase 1.

### 6.5 `test_anr_nested.py` (phase 2)

F1 (MAX and SUM inner, both solvers, three native modes), F14, F16; the grid: depth 2 and
3 × inner scope (`none`, `when`, `per`, `when per`) at each level × reducer kind at each
level × the decision per row or keyed, every cell oracle-judged (the shape of
`test_scope_combinations.py`, ~800 cells, marked `correctness`); AVG at an inner level
divides by members; COUNT at an inner level counts members; a `when` emptying an inner
group is D8 naming the path group; the objective forms `sum(per g: max(x) by (g))`,
`min(per g: sum(x) by (g))`, `avg(per g: …)`, `max(per g: sum(when c: x) by (g))`, `by`
other than the `per` key — each the former refusal turned answer; `MAX(x) <= 10` through
two helper levels on HiGHS (bound derivation order); `norm` inside a reducer refused;
a product inside a nested operand refused; model-size pins for F1.

### 6.6 `test_anr_frames.py` (phases 3–5)

`TestSyntax`: every spelling echoes with defaults omitted; bind errors of §3.6; a frame in
`when`/`per`/`by`/`within`/order refused; `"at"` as a function; round trips.
`TestPoint`: F2 (all four forms), F4, F7, F8, F9, F12, F15, F18; deck examples 1, 4, 5;
the DeQL comparison query (pp. 73–75) as written; a keyed-decision payload; a data
payload on the bound side; a query-wide fallback decision; `first`/`last` under `per
T.productID` (one instance per product) and under `per P.category` (P6 error);
`previous` under `per T.productID` (variants per period); an unused fallback that would
fail (`1 / 0`) is no error.
`TestRange`: F5, F6; deck examples 2, 3, 6; every endpoint pair (`first`, `last`,
`k previous`, `k next` on both ends); `every d` with a non-dividing span; a reversed range
is empty (absent) under both policies; cyclic multi-wrap counts repeats (three peers,
`from 4 previous to 4 next` is nine members); `all` versus `else` versus default on every
fixture; MIN, MAX, AVG, COUNT over a range; a range over a keyed decision; a range on the
bound side; the expansion limit message.

### 6.7 `test_anr_composition.py` (phase 6)

A frame inside a nested reducer operand; a nested reducer beside a frame in one clause;
`norm(x - at(previous else 0: x) over (…), 1)` in the objective and under a bound; ABS over
a range sum; frames in a `per K` clause with a `by` reducer; variants and frames together
(F3's table with a period column); a frame on a table with duplicate joined rows.

### 6.8 `test_anr_diagnostics.py` (phase 6)

F17; a frame-generated definition is structural (never offered for repair); the
unbounded remedy through a helper over a frame; `DIAGNOSE` of F1 with `c` reduced; atomic
DROP of a clause removes its helpers' definitions and no other clause's; no internal
name in any output (`assert_no_internal_leak`).

### 6.9 Verification list (every phase)

```sh
make grammar-build                                   # phases 3+; else make release
LC_ALL=C make decide-test                            # golden check + full pytest, Gurobi
LC_ALL=C DECIDB_FORCE_SOLVER=highs make decide-test
LC_ALL=C DECIDB_VERIFY_SERIALIZER=1 test/decide/.venv/bin/python3 -m pytest test/decide/tests
for m in on off force; do DECIDB_NATIVE_CONSTRUCTS=$m test/decide/.venv/bin/python3 -m pytest test/decide/tests -k 'anr'; done
build/release/test/unittest "[decidb]"
build/release/test/unittest "test/decidb/*"
make format-check && git diff --check
```

Golden diffs are inspected before re-capture; a tie-only result change is recorded
separately from a model change.

### 6.10 Benchmarks (phase 7)

`benchmark/decide/queries`: nested depth 1–3 over 10³–10⁶ rows at fixed key cardinality;
variants per class 1–100; point and range frames with width 1–50 and step 1–7 over
10³–10⁶ rows and 1–10⁴ partitions; cyclic with repeats. Recorded through
`profile_pipeline.py` spans: entity mapping, frame maps (sort), signature grouping,
membership evaluation, formulation, solve; counters: rows visited, groups, members,
positions requested, helpers, model rows and nonzeros. The existing flat workloads are
the regression baseline.

---

## 7. Risks and their guards

| Risk | Guard |
|---|---|
| Lost multiplicity (a duplicate signature's row still feeds a deeper member or a timeline) | Signature grouping decides *instances*; membership is computed from the path-kept rows, never from the instances (§4.7 step 5); F11 and the grid pin it |
| The same `by` value under two ancestor groups sharing one helper instance | Helper keys are path keys (§3.1); F1's `West` group reads rows of two depots and is one instance, while a `(rid, cid)` pair is one instance per pair |
| Under-constrained MIN/MAX helpers | P10: both link sides past any non-linear consumer; `DECIDB_NATIVE_CONSTRUCTS` matrix |
| Stale Big-M in a repair model | Unchanged from item 5: definitions are stated against the active `FormulationBox`; `test_anr_diagnostics.py` reformulates |
| Frame expansion blow-up | `decide_frame_limit` before allocation; interrupt polling per partition |
| Silent de-duplication loss (P2) | EXPLAIN's variant line; F11 pinned both ways; the trap documented |
| Lexer keywording breaks SQL windows in a subquery of the clause | depth-aware `OVER_DECIDE` (new lexer state, phase 3); a test with a window inside the clause's subquery |
| A nested MIN/MAX refuses on HiGHS where a bound exists only at the root | the backward bound pass of §4.6 in phase 2, pinned both before (the named refusal) and after |
| A reducer shared by two clauses duplicates a whole nested helper tree | known limit, `05_optimizer/todo.md` §1, grows with depth; a dump pin records the duplication so a later sharing rule has a baseline |
| Two paths during migration | None exists: each phase lands on the one path; the two-level special case is deleted in phase 2, not kept beside the general one |

---

## 8. Review record (27 October 2026)

Two independent reviewers, one on architecture and delivery and one on semantics and
testability, compared this plan with the first draft, verified both against the code and
the deck, then rebutted each other. Both chose this plan as the base (confidence 80%
and 70%) and converged on the corrections now folded in above: the objective is a named
phase-2 deliverable (§4.5); the bound-flow claim of §4.6 was wrong and is replaced by the
one-directional truth plus a backward pass; a frame's signature fields are its keys, as
deck p. 99 stores them (P13), which also removes the need for a new grouping-key source;
point selectors inline from phase 4 (P9); the `=` contradiction check stays; the
conversion inventory is complete; the lexer depth state is named as new work; the deck
divergences are decisions P14–P16 with fixtures; the first draft's symbolic-freeze and
status-grid oracle checks are adopted. Every number in §6.3 was re-derived by
enumeration during the review. What the first draft still holds over this plan: its
sparse emission would avoid a helper per anchor row for *range* frames, which here waits
for phase 7.
