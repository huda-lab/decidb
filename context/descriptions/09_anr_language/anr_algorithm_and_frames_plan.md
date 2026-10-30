# The full ANR algorithm and frame selectors — implementation plan

Status: phases 0, 1 and 2 shipped (2026-10-28), 3 (2026-10-29), 4 and 5 (2026-10-30);
phases 6 and 7 to build. Baseline commit
38275dde6f (branch `Sami-Active`), reviewed
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
| P2 (D65) | **The inner `per` and the colon form** `SUM(D: e)`: under P1, `sum(per D: D.opening_cost + T.unit_cost * ship)` over a depot with two routes stops being the D63 error and silently counts the depot twice (one member per distinct `(unit_cost, ship)`), which is the per-row answer the user wrote `per D` to avoid. Keep the error for an inner `per`, or follow the deck? | Follow the deck (one rule), and make EXPLAIN print the variant count of every scoped expression that produced more than one per class (§4.8), so the surprise is visible in the plan — EXPLAIN ANALYZE, as shipped: a plain EXPLAIN reads no data, so it has no count. Record the trap in `syntax_reference.md` §5.3. |
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
§5.6 frames, §7.1 rewritten, §10 updated) and `03_expressivity/scope/done.md` as each
phase ships it. §3.2 is in the engine since phase 1 (`syntax_reference.md` §7.1), §3.1's
path rule and §3.3 since phase 2 (§5.5); the rest is not yet. The reference evaluator built in phase 0,
`test/decide/tests/_anr_oracle.py`, encodes every rule below
(`04_testing/scope/done.md`), and the tests of §6 pin them.

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

- **A direct tuple term** is a maximal subtree of the body with no decision, reducer or
  frame (today's `CollectClauseParts` rule, D77), compared by value: `per g: x <= a + b`
  over rows `(1, 2)` and `(2, 1)` has one signature value 3. Constants are terms with one
  value. It is read on the canonical tree, so the data terms of one additive context,
  read through `+`, `-` and constant factors, form one field: `x + a <= b + sum(y)` has
  the one data field `b - a` beside the decision `x`, and written associativity never
  changes the count. A data term under a row-varying factor or inside `abs` belongs to
  that factor's or that `abs`'s own context.
- **A decision** is compared by *instance*, whatever its coefficient: a per-row decision
  is its row, a keyed one its class, a query-wide one itself. `per D.depotID: ship <= 10`
  with `ship` per row generates one instance per row. (Phase 0's evaluator first read a
  decision's terms into the data field as one linear form, which differs only where
  coefficients cancel, `a * x + b * x`; phase 1 aligned it to this rule.)
- **A frame term** contributes the frame's `within` key and, for a relative selector, the
  anchor's order key (deck p. 99; P13): two anchors in one class that look at different
  positions are two variants, two that look at the same position are one. `first` and
  `last` contribute the `within` key only. A fallback the anchor reaches is a field too,
  compared by value; one it does not reach is neither a field nor evaluated. Absence
  (§3.5) is not a field but a mask on the rows.
- **`per row`** keeps every tuple occurrence, including two rows equal in every column.
- **NULL** is one value in a signature; a NULL coefficient or bound is still the §3 error
  on the instance that reads it.
- Equal body values from different classes stay separate instances, two identical model
  rows (formulation merges none, and none needs merging).
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
| `COUNT` | data only at every depth: `count(*)` counts members, `count(e)` the non-NULL ones; no member at all is 0, as SQL counts an empty set |
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
T.period = 1; a position must hold one row."* Identical payloads select once: the same
value of data, the same linear form over decisions (one constant and one coefficient on
each decision instance, so `0 * x` reads none).

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
| in a reducer operand | that member is dropped; a reducer with no member left is absent, except `COUNT`, which is 0 |
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
| ambiguous peer | `DECIDE: <frame> finds <n> rows at <partition>, <S> = <v>; **a position must hold one row**.` |
| objective with an absent frame | `DECIDE objective: <frame> **selects no position**; the objective is produced once and cannot be skipped. Give the frame an else.` (phase 4 refuses a frame that may miss inside a reducer over decisions, so "reduce the frame" is no remedy yet) |
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
| point frame, decision payload | yes (read in place, D72) | yes, read in place; one that may miss from phase 6 | yes |
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

Shipped in phase 2 but for frames (step 3), as `05_optimizer/done.md`, "The value
rewrite", describes; the text below is the plan it was built from.

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

Shipped in phase 2 (`06_model_formulation/done.md`, "Helper decisions of the value
rewrite"), the backward pass tightening helper columns only; a box per helper instance
did not ship (`05_optimizer/todo.md` §2).

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
4. **Signature grouping** (shipped in phase 1: `BuildSignatureIds`, `physical_decide.cpp`)
   numbers the kept rows by `per key ++ signature fields` through `BuildGroupIds`, then
   splits by each decision field's instance (a keyed decision's entity, the row for a
   per-row one; a query-wide decision adds no field). The rows shape emits one model row
   per variant at its first kept row; the inner-`per` de-duplication reads a reducer's own
   variant scope (`EntityScopeInfo::signature`), so its consumers are unchanged. Phase 4
   adds the `FRAME` field (its `within` and order expressions) and absence: a row mask
   applied after `when` (it does not feed D4), a row whose non-reduced frame is absent
   joining no group. `EvaluatedConstraint::rows_stand_with_their_class` stays `!has_when`:
   a row it leaves out stands with its *variant's* first row, which carries the same
   decisions and data, so implied-bound soundness (`linearization_bigm.cpp:280-290`)
   holds.
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
  a range; under a clause whose classes produced more than one variant (P2), in EXPLAIN
  ANALYZE only, since only the data can tell — shipped in phase 1 as
  `Kind::VARIANTS`: *"per t.g: 2 constraints from 1 class, one per distinct (a) within a
  class"*, *"sum(per D: ...): 3 values from 2 classes, one per distinct (T.unit, ship)
  within a class"*; "skips 2 instances with no previous position" for absence.
- DIAGNOSE: §4.7 labels; a frame-generated definition is structural; the unbounded
  remedy and the relaxed-bound re-formulation see helpers as today.

---

## 5. Phases

Each phase is one review unit, ships on the single path, and ends with the full
verification list of §6.9 green. Phases 3–5 may start once phase 1's signature contract
is merged; phase 2 and phase 3 are independent of each other.

### Phase 0 — Contract, oracle, fixtures (shipped 2026-10-28)

No engine change. Built: the reference evaluator `_anr_oracle.py`, every fixture of §6.3
in `_anr_cases.py`, and `test_anr_oracle_selfcheck.py`, which checks the evaluator
against the deck's hand computations, exhaustive enumeration, today's scope oracle on its
784-cell grid, today's engine, and the engine's feasibility with every decision fixed
(`04_testing/scope/done.md`). Baselines: the golden corpus identical on Gurobi and HiGHS,
and the medium-tier timings in `05_performance/04_model_construction/done.md`. Found: a
bound-side `count(e)` refuses a NULL instead of counting the non-NULL values
(`06_issues/bugs/todo.md`), fixed in phase 2.

### Phase 1 — Variants (shipped 2026-10-28)

Built as §4.3, §4.4 and §4.7 step 4 describe: `DecideSignature` (property 221, the
objective's 222) built by `BuildConstraintSignatures` / `BuildObjectiveSignatures` on the
canonical tree, the execution grouping (`BuildSignatureIds`), the objective's
produced-once error (P3), EXPLAIN ANALYZE's variant line (P2), DIAGNOSE's variant label;
the `DETERMINES` builders and both determination messages deleted, the `=` contradiction
check kept. One choice beyond the text above: a relation-qualified reducer gets a scope of
its own (`EntityScopeInfo::signature`) instead of a signature index on every term kind,
so every stage that de-duplicates by a qualifier reads variants unchanged. Tests:
`test_anr_variants.py`, and every conversion listed in the draft of this phase
(`test_scope_determination.py` became `test_scope_variants.py`). Record:
`09_anr_language/done.md`, "phase 1".

### Phase 2 — Nested reducers (shipped 2026-10-28)

Built as §3.1, §3.3, §4.2, §4.4–§4.7 describe for nesting: the gate lifted, the path
rule stated once (`DecideReducerPath`) and read by the D8 checks, the variant rows and
the recursive value rewrite (path keys, D73's pinning), the backward bound pass
(`DeriveReducerMemberBounds`, helper columns only; the box per instance stays
`05_optimizer/todo.md` §2), nested bound-side reducers and `count(e)`, and the
objective as nested helpers (D75) with the two-level path deleted. Choices beyond the
text above: the outermost objective reducer is a helper too, except a SUM over group
SUMs or AVGs, which is the objective's own sum (a square in it stays a convex QP, and an
inner AVG divides per group); a path key is a group key the optimizer adds, not
`path_group_keys` / `path_whens` on the link; a MIN/MAX of a square inside nested
reducers is refused (D74), where the deleted path answered it wrongly. Tests:
`test_anr_nested.py` (§6.5) and the conversions listed in `done.md`, "phase 2". Record:
`done.md`, "phase 2".

### Phase 3 — Frame syntax (shipped 2026-10-29)

Built as §4.1–§4.3 describe: the lexer tokens with the new parenthesis and subquery
state, the productions, `DecideFrameExpression`, `LogicalDecide::frames` (220),
`EnumerateExpressions`, subquery planning, serialization and the echo; one gate refuses
*"DECIDE: frame selectors are not supported yet: <frame>"*. Choices beyond the text above:
a point frame binds to `first(e)` tagged `__frame_<i>__` rather than a `FRAME_TAG`
wrapper (an aggregate is opaque to every layer that opens a sum); the gate stands in
`CreatePlan` just before canonicalization, the first layer that would read a frame, so
`EXPLAIN`, `DIAGNOSE` and `CREATE VIEW` refuse too, no plan holds a frame and the bound
plan's round trip of `frames` waits for phase 4 (first at the optimizer's entry, the
review of 2026-10-29 moved it: the canonicalizer refused some frame clauses first, for
reasons of its own); the `FRAME_PARTITION` check moved to phase 4, since nothing past the
gate runs; `previous`, `cyclic` and `every` are identifiers read by their text, whatever
its case, not keywords; a fallback is any number, in its own type; a frame's payload is
linear in the decisions. Tests: `test_frame_parser.test`, the C++ round trips,
`test_anr_frames.py::TestSyntax`. Record: `done.md`, "phase 3".

### Phase 4 — Point selectors (layers 4, 5, 6, 8) (shipped 2026-10-30)

Built as the design below says; the record, with the choices made beyond it, is
`done.md`, "phase 4".

- Frame maps; data-only point frames as values; decision point frames as redirected
  terms (P9); absence propagation; `else`; `cyclic`; `desc`; `k`; peer error (P4); the
  objective; the `FRAME_PARTITION` check (P6, moved from phase 3) and the gate lifted for
  points.
- Tests: `test_anr_frames.py::TestPoint` (§6.6): F2, F4, F7–F9, F15, F18, F19, F21, the
  laziness and partition cases, deck examples 1, 4, 5, the DeQL comparison (pp. 73–75);
  F2's model-size pin (6 columns, 4 rows).
- Exit: F2's four comparisons and two skips, hand-pinned, at the pinned model size; both
  solvers.

**Design (written 2026-10-29, before the code).** One idea carries the phase: *a point
frame is a tuple term read at another row*, so every layer above execution keeps it as
one opaque term, and execution turns it into two things the rest of the engine already
handles — data columns beside the input's, and decision instances read at another row.

| Layer | Owns in phase 4 | Contract it hands down |
|---|---|---|
| 2 binder | `else NULL` written (a fallback NULL before the query runs) is no fallback (D80) | the `first(e)` marker of phase 3 |
| 3 plan | `DecideFrameSpec` gains `rows`, `scope_key`, `source_clause_id`, `reduced` (serialized, filled by layer 4); `DecideScopeCheckKind::FRAME_PARTITION` and `DecideSignatureFieldKind::FRAME`, each naming a frame | the frame's timeline and placement |
| 4 canonicalizer | A frame marker is a tuple term, never a reducer (`ContainsReducer` skips it): a decision-bearing one sits left, a data-only one right; it decides no shape of its own. Places every frame: its timeline rows are its scoped expression's path rows (§3.1, `DecideReducerPath`), its timelines the path's `by` keys and then `within`. Builds `FRAME_PARTITION` (D69) for a frame under `per K`, and the frame's signature field (D76) in place of descending into it. Refuses, until phase 6, a frame that may select no position (relative, not cyclic, no `else`) inside a reducer over decisions: placement is where a frame is known to stand inside one. Holds every constraint's frame to a clause id, so a frame with none is the objective's | placed frames, checks, fields |
| 5 rewrites | Treat the marker as an opaque tuple term (ABS, `<>`, the value rewrite and MIN/MAX copy it with the term) | the marker, in every row a rewrite derives |
| 5 linear form | Lowers each marker it meets. A payload decision term `c·x` becomes `READ(f, c)·x` read at the selected row (`DecideTerm::frame_read = f`); the payload's data `d` becomes `READ(f, d)`; a fallback's terms `c'·y` and data `d'` become `FALLBACK(f, c')·y` and `FALLBACK(f, d')`, read at the anchor. A data-only frame on the bound side becomes one column of its own type, `VALUE(f, e, v)`: the payload at the selected row, the fallback at the anchor (no arithmetic joins them, so a DATE or BOOLEAN payload reads as one); inside a bound-side reducer, `PRESENT(f)` joins the reducer's own `when`, so an absent member is dropped by the machinery that drops a filtered one | `DecidePreparedModel::frame_columns`: each `{kind, frame, value[, decision]}`, read by the expressions as chunk column `input_columns + j` |
| 6 formulation | A frame read is a variable of its own scope, `FRAME_READ`: `VarIndexer::Get(v, r) = Get(base, rows[r])`; it owns no column, its type is its base's, and its box is its base's, re-intersected after implied bounds. The per-row fast path never takes one | `SolverInput::frame_reads` |
| 8 execution | Frame maps (timelines sorted by DuckDB sort keys under `default_null_order`, D68), D67, the frame columns appended to the materialized input (lazily: a payload is computed on each selected peer's first row only, a fallback on the anchors that reach it only; where neither stands a coefficient is 0 and a value NULL), `FRAME_PARTITION`, the signature `FRAME` field, frame-read variables, and **presence**: an instance whose comparison holds a frame directly (outside reducers) that selects nothing is skipped — its rows, and every row a rewrite derived from the same comparison, at that anchor; under the tightest-bound collapse an absent anchor gives no bound and a group with none has no row (D79); a bound-side reducer left with no member is absent, which absents the instance; the objective's root frame selecting nothing is the §3.6 error | the model; readback reads the input's own columns only |

Why this split, rather than the plan's earlier "redirect inside the model builder":
column resolution has some thirty call sites in stage 06, and a site that forgot a
redirect would read the anchor's own column silently. `VarIndexer::Get` is the one place
every one of them already calls, and a frame read's per-variable arrays are copied from
its base, so a site that reads a box or a type by variable gets a sound value without
knowing frames exist. The same reasoning makes frame values data columns: every
expression evaluator (`when`, coefficients, bounds, reducers, signatures) reads chunk
columns, so none learns a new node.

Choices beyond the text above, recorded as such:

- **Inside a reducer operand a point frame is read in place too** (D72 said a helper per
  anchor row there): `sum(at(previous else 0: x))` is a sum of redirected terms, which
  needs no helper and no definition row; a MIN/MAX reads each member at its selected row
  the same way. Ranges still lower to helpers (phase 5).
- **A frame that may select no position inside a reducer over decisions is refused until
  phase 6**, which owns members dropped from decision reducers and absent reducers; data
  reducers drop absent members from phase 4 (F19).
- `else NULL` written is the default (D80); a fallback that evaluates to NULL where it is
  reached is a NULL bound or coefficient, as a present NULL payload is (D78).
- A frame in the objective follows D66: `at(previous: x)` at the objective's root reads
  one position per anchor, so it takes more than one value and is refused as produced
  more than once; `at(first: x) over (S)` with no `within` is one value.

Slices, each green before the next: (1) the plan fields, placement, `ContainsReducer`,
the gate on ranges only, frame maps and columns, data frames on the bound side, presence,
laziness, `FRAME_PARTITION`, D67 — F8, F21, `lazy_*`, `partition_*`, F7; (2) the signature
field — F19; (3) frame reads — F2, F9, F18, F4, F15, deck examples 1, 4, 5, the DeQL
query, keyed and query-wide payloads and fallbacks; (4) the refusals, EXPLAIN and DIAGNOSE
pins, docs, the full verification list.

### Phase 5 — Range selectors (shipped 2026-10-30)

Built as the design below says; the record, with the choices made beyond it, is
`done.md`, "phase 5".

- `from … to …`, `every`, `else` per position, `all`; SUM/AVG/MIN/MAX/COUNT over the
  selected rows; data-only ranges as values; decision ranges as helpers; the expansion
  limit and interrupt polling.
- Tests: `test_anr_frames.py::TestRange` (§6.6): F5, F6, deck examples 2 and 6 (example
  3 is a point frame that may miss inside a reducer over decisions, phase 6); cyclic
  repeats; reversed ranges; `all` versus `else`.
- Exit: the range matrix of §6.6 passes on both solvers.

**Design (written 2026-10-30, before the code).** Words used below: an anchor's **range**
is the list of values a range frame reads for it — every row of each position it
selects (a repeated visit again), and, with `else v`, one `v` per missing position; a
**window** is one range several anchors share. Two anchors share a window when nothing
about the anchor itself changes what is read: under `first`/`last` at both ends, every
anchor of a timeline reads the same range; under a relative end, every anchor of a peer
does, except where it reaches an `else` that reads the anchor's own row.

One idea per kind of payload:

- **Over data, a range is a value**, as a point frame over data is: one column of the
  frame's own reducer type (`sum` of an INTEGER is a HUGEINT, `max` of a DATE a DATE),
  computed by running the bound aggregate itself over each window's values, so SQL's
  types and arithmetic hold. A NULL among a window's values makes that window's value
  NULL, which its reader reports as today's NULL bound or coefficient (D78); `count`
  skips it, as SQL's `count(e)` does.
- **Over decisions, a range is a helper decision per window** (D72), defined once per
  window: an equality for SUM and AVG, an extremum link for MIN and MAX. The helper
  replaces the frame in the tree before any other rewrite, so ABS, `norm`, `<>`,
  MIN/MAX and every reducer read it as an ordinary keyed decision and the canonical
  shape stands. A window over a timeline is one helper for the whole timeline, so
  `x <= sum(from first to last: x) over (t.period)` over n rows is one definition of n
  terms, not n of them.

| Layer | Owns in phase 5 | Contract it hands down |
|---|---|---|
| 2 binder | A range's `else` and payload take one type, as a point's do; the range's reducer is bound over it | the marker `agg(e)` tagged `__frame_<i>__` |
| 3 plan | `DecideFrameSpec::MaySelectNothing()` for ranges, exact per endpoint pair: a range may read nothing when it may be empty (a reversed pair such as `from next to previous`, or `from first to 2 previous` near the start), when `all` meets a missing position, or when, with no `else`, every requested position may lie off the timeline; `count` is never empty (0), only `all` makes it absent | the frame's placement and policy |
| 4 canonicalizer | Unchanged: a range marker is a tuple term like a point's; placement, D69 and the signature field (D76) already read `Relative()`; the phase-6 refusal of a frame that may read nothing inside a reducer over decisions now covers ranges | placed frames |
| 5 rewrites | `RewriteFrameRanges`, first among the rewrites: each range over decisions becomes a helper decision with one instance per window (a scope of its own, built by execution from the frame map) and a `ReducerValueLink` naming the frame; a MIN/MAX helper's sides are read off the way its comparison pushes it, through `+`, `-`, data factors and enclosing SUM/AVG, both sides elsewhere (D73) | the helper, in every row a rewrite derives |
| 5 linear form | A range over data becomes one `REDUCED` column; one inside a bound-side reducer that may read nothing adds its `PRESENT` column to the reducer's `when`, as a point does; a range's `else` gives `FALLBACK` columns, read by the helper's definition and by the signature where an anchor reaches it | `DecidePreparedModel::frame_columns`, `ReducerValueLink::fallback_terms` |
| 6 formulation | A helper's members are handed over **listed**: per instance, the row it is read at and its members, each a constant and terms (decision, row, coefficient). The rows form of today, which assumes the groups partition the rows, cannot state windows that share rows; execution now lists every link, and the three passes over links read one form | `ReducerValueLinkSpec::members` |
| 8 execution | The frame map's windows and their values (rows with visit counts, missing positions), in O(values) work: positions off a timeline that does not wrap are counted, never visited; `decide_frame_limit` (default 10,000,000 values per frame) is checked as they are listed, and an interrupt polled; absence per anchor; the `REDUCED` column through the aggregate's own state functions; the helper scope's entities (one per window); each link's members listed (a window's rows read once each, AVG scaled by the window's value count, a fallback scaled by its missing positions) | the model |

Choices beyond the text above, recorded as such:

- The helpers are keyed per **window**, not per anchor row (§4.5 step 3 said a helper
  per row): the definition rows then grow with the windows, not with anchors × width.
- The limit counts the **values** a frame reads (each row read at a selected position,
  each `else` standing in for a missing one), which is what memory holds, rather than
  the positions it requests: ties in the order key can make one position many rows.
- A range in the objective's root follows D66 as a point does: `sum(from first to last:
  x) over (t.period)` is one value; one relative to the anchor is produced once per
  position and refused.

### Phase 6 — Composition, diagnostics, guards

- Frames inside nested reducer operands and reducers over frames (deck example 3);
  frames and nested reducers in one clause; `norm` and ABS over a frame term; the AND
  split regression; DIAGNOSE labels and DROP groups for every new row kind; `decide_frame_limit`;
  cancellation during map construction and membership evaluation.
- Tests: `test_anr_composition.py`, `test_anr_diagnostics.py` (§6.7, §6.8).
- Exit: every row of the capability matrix (§3.7) has a passing test or a pinned refusal.

### Phase 7 — Inlining, performance, closure

- Inline a SUM/AVG helper whose only consumer is a linear root row (deck p. 115 inlines
  the sums) or a MIN/MAX link's members (Q4's group sums, built 170 ms slower since phase
  2, `05_performance/04_model_construction/done.md`), and a range SUM/AVG frame in a
  Rows-shape clause, as redirected terms (one per selected position; stage 06
  accumulates them like like-terms) — only with a model dump showing the smaller model
  and the suites unchanged. Singleton MIN/MAX groups
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

Built in phase 0; its statements, its readings (`instances`, `solve`, `judge`,
`feasible`, `brute_force`) and its error kinds are described in
`04_testing/scope/done.md`. What remains: `_scope_oracle.ScopedModel` delegates to it, so
the existing suites keep their statements and lose the special `Nested` form (deeper
nests are stated to `_anr_oracle` directly meanwhile). (`ScopeError.not_determined` was retired in phase 1, when `ScopedModel`
learned variants on its own statements.)

### 6.3 Fixtures — `_anr_cases.py`

Built in phase 0: F1–F21 with their variants, and the edge cases beside them, each with
its SQL, its oracle statement and its hand-computed answer (`04_testing/scope/done.md`).
`Case.phase` is the first phase whose engine runs a case through DeciDB:

| Phase | Cases |
|---|---|
| 0 (today's engine) | F10_sum, F11_determined, F11_per_row, null_bound, kept_none |
| 1 | F3_region, F3_region_limit, F10_split, F11_varying, F11_average, F12, F13, F20_split_terms, F20_joined_terms |
| 2 | F1_max, F1_sum, F14, F16, F16_small, count_non_null, nested_d8 |
| 4 | F2_previous, F2_cyclic, F2_next, F2_desc, F4_cyclic, F4_open, F7_point, F8_last, F8_last_desc, F8_first, F8_first_desc, F9, F15, F18, F18_cap_kept, F19, F21, lazy_unused, lazy_reached, partition_spanned, partition_held |
| 5 | F5_weekly, F5_offset, F6_truncate, F6_all, F6_else, F7_range, objective_absent, cyclic_repeats, reversed, count_empty |
| 6 | F17 |

### 6.4 `test_anr_variants.py` (phase 1, shipped)

Described in `04_testing/scope/done.md`.

### 6.5 `test_anr_nested.py` (phase 2, shipped)

Described in `04_testing/scope/done.md`.

### 6.6 `test_anr_frames.py` (phases 3–5)

`TestSyntax`: every spelling echoes with defaults omitted; bind errors of §3.6; a frame in
`when`/`per`/`by`/`within`/order refused; `"at"` as a function; round trips.
`TestPoint`: F2 (all four forms), F4, F7, F8, F9, F12, F15, F18; deck examples 1, 4, 5;
the DeQL comparison query (pp. 73–75) as written; a keyed-decision payload; a data
payload on the bound side; a query-wide fallback decision; `first`/`last` under `per
T.productID` (one instance per product) and under `per P.category` (P6 error);
`previous` under `per T.productID` (variants per period); an unused fallback that would
fail (`CAST(t.s AS INTEGER)` of a text; DuckDB reads `1 / 0` as infinity, not an error) is no error.
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
