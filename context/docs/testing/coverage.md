# Test coverage by feature

What each feature area's tests cover, and whether they check optimality (oracle) or only feasibility. The quality
levels and the rules for new tests are in [README.md](README.md); the scoping language's suite is in
[scope_suite.md](scope_suite.md); open gaps are in [todo.md](todo.md).

## Clause order

Tests live in `test/decide/tests/test_clause_order.py`.

Covers the two accepted positions of the `DECIDE` declaration — the paper's
split order (declaration between `SELECT` and `FROM`, constraints and objective
after the joins) and the single-block order (whole clause after `WHERE`). The
feature itself is described in `../language/functions.md` → "Two clause
orders" and `../language/guide.md` §1.

### Scenarios covered

- **Split order is correct, not just parseable** (`test_split_order_oracle`,
  oracle-verified): a knapsack over `customer JOIN nation` written in the paper's
  order. An oracle here is what rules out the declaration reaching the binder
  detached from its constraints — a shape that would still parse.
- **The two orders agree** (`test_orders_return_the_same_rows`): one problem with
  a unique optimum written both ways; rows compared directly, so a divergence
  shows up as a different selection rather than an alternate optimum.
- **The two orders produce the same plan** (`test_orders_produce_the_same_plan`):
  normalized `EXPLAIN` output compared across the orders. This is the direct
  statement of "nothing downstream of the parser distinguishes them".
- **Every declaration scope parses in the split slot**: one decision per row
  (throughout), `per (): cap(INT)` (`test_scalar_declaration_in_split_slot`, one
  shared value across rows) and `per n: keep(BOOL)`
  (`test_entity_scoped_declaration_in_split_slot`, one value per nation across the
  join's repeated rows, with a colon-form reducer `SUM(n: keep)`).
- **The `in_decide_clause` lexer flag is cleared between the slots**:
  `test_case_when_in_join_on_between_slots` and
  `test_case_when_in_where_between_slots`. This is the hazard the split order
  introduced — the declaration arms the DECIDE lexer context, and the SQL
  between it and `SUCH THAT` must not see it, or `CASE WHEN` would lex as the
  clause's `when`.
- **Malformed splits are parser errors** (`TestClauseOrderErrors`): a declaration
  in both slots, a declaration with no `SUCH THAT`, and a `SUCH THAT` with no
  declaration. Separately, `test_declaration_between_from_and_where_rejected`
  pins that there are exactly **two** slots — `FROM ... DECIDE ... WHERE ...` is
  not a third one, which is a plausible guess once the split order exists.
- **The remaining source shapes**: a subquery source
  (`test_split_order_over_subquery_source`, keyed `per t:` on the subquery, so it
  exercises the binding path of a keyed decision over a subquery), a `WITH` CTE
  (`test_split_order_over_cte_source`), and a three-table fan-out join
  (`test_split_order_three_table_fanout_join`, oracle-verified — the declaration
  slot closes before the first `JOIN` is parsed, so this is the widest gap the
  reassembly spans).
- **`when` and `per` in the split order**: a `when` in front of a constraint
  (`test_split_order_with_when`), `per K: ... by (K)` on a constraint
  (`test_split_order_with_per`, which also asserts the group cap holds), and the
  two-level objective `min(per k: sum(...) by (k))`
  (`test_split_order_with_nested_per_objective`). Each is compared against the
  single-block spelling of the same query rather than asserted in isolation,
  since the claim under test is equivalence.

`test_declaration_in_both_slots_rejected` pins the duplicate-declaration message:
declare variables either before `FROM` or with `SUCH THAT`, not in both slots.

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| split order | JOIN + WHERE | ✓ |
| split order | per-row declaration | ✓ |
| split order | keyed declaration (`per n: x`) | ✓ |
| split order | query-wide declaration (`per (): x`) | ✓ |
| split order | colon-form reducer `SUM(n: ...)` | ✓ |
| split order | `CASE WHEN` in an intervening `JOIN ... ON` | ✓ |
| split order | `CASE WHEN` in an intervening `WHERE` | ✓ |
| split order | `EXPLAIN` | ✓ |
| split order | subquery source / CTE source | ✓ |
| split order | three-table fan-out join | ✓ |
| split order | `when` in front of a constraint | ✓ |
| split order | `per` on a constraint | ✓ |
| split order | two-level objective | ✓ |

## Variable types

Covers `BOOL`, `INT`, `REAL`, and multi-variable queries. Tests live in:
- `test/decide/tests/test_var_boolean.py` — BOOL
- `test/decide/tests/test_var_integer.py` — omitted-type rejection (the type is mandatory)
- `test/decide/tests/test_var_real.py` — REAL
- `test/decide/tests/test_var_multi.py` — multiple variables

Note: keyed decisions (`per K: x(TYPE)`) are covered in
[../entity_scope/](coverage.md#keyed-decisions) and by `test_scope_declare.py`
([../scope/](scope_suite.md)); query-wide decisions (`per (): x(TYPE)`) in
[../scalar_scope/](coverage.md#query-wide-decisions).

### Scenarios covered

All oracle-verified:

- **BOOL** (`test_var_boolean.py`, plus many files): classic 0/1 knapsack and a weight-limit variant; broad coverage across all constraint types and MAXIMIZE/MINIMIZE objectives via the rest of the suite.
- **INT** (`test_cons_perrow.py`, `test_cons_mixed.py`, `test_cons_between.py`): explicit `x(INT)`, per-row upper bound + aggregate, column-derived upper bound (`x <= ps_availqty`), BETWEEN with INT. `test_var_integer.py` covers the omitted-type parser error rather than a positive case.
- **REAL** (`test_var_real.py`): basic LP (continuous MAXIMIZE), upper bound on REAL, mixed BOOL + REAL, `when` and `per` on aggregate constraints, MINIMIZE (coefficient-sign path), forced non-integer optimum (readback sanity).
- **Multiple variables** (`test_var_multi.py`, plus interactions elsewhere): two variables with separate constraints (BOOL + INT), two boolean variables (cross-constraint), paired INT + REAL (no BOOL), three variables (BOOL + INT + REAL), mixed BOOL + REAL in the same SUM term and in the same query; mixed BOOL + REAL with ABS (`test_abs_linearization.py`); BOOL + INT under a `per` key (`test_per_interactions.py`).

#### Error cases

`test_error_binder.py`: variable name conflicts with column, duplicate DECIDE
variable, unknown variable in constraint. Unknown type annotation is
parser-level (`test_error_parser.py`).

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| BOOL | all features (broadly) | ✓ |
| INT | all features (broadly) | ✓ |
| REAL | MAXIMIZE objective | ✓ |
| REAL | MINIMIZE objective | ✓ |
| REAL | `per` constraint | ✓ |
| REAL | `when` constraint (aggregate) | ✓ |
| BOOL + INT | same query | ✓ |
| BOOL + INT | `per` key | ✓ |
| BOOL + REAL | same query | ✓ |
| BOOL + REAL | ABS linearization | ✓ |
| BOOL + REAL | bilinear (McCormick) | ✓ |
| INT + REAL | QP constraint | ✓ |

## Keyed decisions

Tests live in `test/decide/tests/test_entity_scope.py` (keyed decisions over joins;
the file and its `test_entity_scoped_*` tests keep their names). The declaration
itself — keys of columns and relations, NULL as a class, pruning, errors — is covered
by `test_scope_declare.py` ([`scope_suite.md`](scope_suite.md)). Syntax:
`../language/guide.md` §2.3.

`decide per n: keepN(BOOL)` over `customer JOIN nation` gives one decision per nation
(`per n` keys on every column of `n`), shared by every customer row of that nation.
Oracle-verified tests compare DeciDB's output against an independently formulated
gurobipy model via `compare_solutions` or the scope oracle.

### Scenarios covered

Oracle-verified groups in `test_entity_scope.py`:

- **Core**: BOOL selection, one value per class under a tight `sum`, INT (per-row and
  aggregate bounds), REAL (single table, DOUBLE readback), keyed and per-row decisions
  in one query (`VarIndexer`'s four-block layout `[row | keyed | query-wide | auxiliary]`),
  two keyed decisions from different relations.
- **`when` / `per` composition**: a `when` in front of a constraint and of the
  objective, one- and multi-column `per` keys, `when` + `per` together.
- **Aggregate interactions**: easy and hard (Big-M) MAX/MIN, hard MIN on a keyed INT,
  keyed + `when` + hard MAX, ABS (per-row auxiliaries feeding a keyed column), AVG
  (alone and under `per`), `<>` (alone and per region via `add_ne_indicator`),
  `BETWEEN`, `=`.
- **Source shapes and NULL**: an uncorrelated scalar subquery bound shared across `per`
  classes; a three-table fan-out join with the decision keyed on the middle relation and
  `per` on an outer one; a NULL key value as one shared class
  (`test_entity_scoped_null_key`, keyed `per n.nk:`); a keyed decision beside a `per`
  constraint over the same nullable column, where NULL is one class in both
  (`test_entity_scoped_vs_per_null_semantics`, the `per` half judged by the scope
  oracle); a per-row baseline on a 1-to-many orders × lineitem join
  (`test_row_scoped_vars_on_fanout_join`).

Rows with unique caveats:

- `test_entity_scoped_mixed_when_per` — keyed + per-row + `when` + `per`,
  oracle-verified but Gurobi-only; the fixture skips on HiGHS-only hosts.
- `test_entity_scoped_nonexistent_table` — error test: `per nonexistent:` is "neither
  a column nor a relation of the FROM clause".
- `test_entity_scoped_var_in_when_condition_error` — error test: "keepN is a decision;
  when, per and by may only use data".
- `test_entity_scoped_when_entity_invisible` — error test: a `when` that keeps no row
  is "when c.c_acctbal > 9998 matches no rows" (D4).
- `test_entity_scoped_over_subquery_of_base_table` /
  `test_entity_scoped_over_cte_of_base_table` — `FROM (SELECT ... FROM base) t DECIDE
  per t: x(BOOL)` and the same shape through a `WITH` CTE keep one decision per row of
  `t`; they guard the child bindings being read before `CreatePlan` moves the projection
  expressions out (`plan_decide.cpp`).

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| keyed decision | BOOL | ✓ |
| keyed decision | INT | ✓ |
| keyed decision | REAL | ✓ |
| keyed decision | per-row decision (mixed) | ✓ |
| keyed decision | `when` (constraint) | ✓ |
| keyed decision | `when` (objective) | ✓ |
| keyed decision | `per` (one column) | ✓ |
| keyed decision | `per` (several columns) | ✓ |
| keyed decision | `when` + `per` | ✓ |
| keyed decision | MAX easy (≤ K) | ✓ |
| keyed decision | MAX hard (≥ K) | ✓ |
| keyed decision | MIN easy (≥ K) | ✓ |
| keyed decision | MIN/MAX + `when` (triple) | ✓ |
| keyed decision | ABS | ✓ |
| keyed decision | AVG (alone and under `per`) | ✓ |
| keyed decision | `<>` | ✓ |
| keyed decision | `<>` + `per` | ✓ |
| keyed decision | `BETWEEN` | ✓ |
| keyed decision | bilinear (Boolean factor) | ✓ (`test_bilinear.py::test_bilinear_entity_scoped`) |
| keyed decision | QP objective (convex) | ✓ (`test_quadratic.py::test_qp_entity_scoped_objective`) |
| keyed decision | two keyed relations | ✓ |
| keyed decision | uncorrelated scalar subquery bound + `per` (three-way) | ✓ |
| keyed decision | NULL key value (one shared class) | ✓ |
| keyed decision | three-table fan-out join + `per` on an outer relation | ✓ |
| keyed decision | source is a subquery / CTE over a base table | ✓ |
| keyed decision vs `per` | NULL key: one class in both | ✓ |
| per-row decision | 1-to-many fan-out join | ✓ |

## Query-wide decisions

Tests live in `test/decide/tests/test_scalar_scope.py` (the file keeps its name), plus
`test_scalar_var_in_aggregate.py` for a query-wide decision as a term of an aggregate
constraint and `test_clause_order.py::test_scalar_declaration_in_split_slot` for the
pre-`FROM` declaration position.

`DECIDE per (): x(TYPE)` is one decision for the whole query, beside one per row
(`x(TYPE)`) and one per class of a key (`per K: x(TYPE)`). Semantics:
`../language/functions.md`; syntax: `../language/guide.md`
§2.2.

### Scenarios covered

- **Shared bound, oracle-verified** (`test_scalar_shared_bound`): the paper's §3.1
  shape — one cap constrains every row and is penalised in the objective, so the
  optimum trades a single cap value against the per-row gain it unlocks. A per-row cap
  would decouple the rows entirely, which is what makes this discriminating.
- **The objective coefficient is applied once, not per row**
  (`test_scalar_objective_coefficient_applied_once`, oracle-verified):
  `MINIMIZE 2*cap - SUM(x)` is constructed so the two readings drive `cap` to opposite
  ends of its domain. This is the case that would silently multiply a query-wide
  coefficient by input cardinality.
- **One column regardless of cardinality**
  (`test_scalar_is_one_column_regardless_of_cardinality`) and **the value is repeated
  on every output row** (`test_scalar_value_repeated_on_every_row`).
- **A row-invariant reducer body is rejected** (`TestScalarReducerRejected`, error
  tier): `SUM(cap)` and `AVG(cap)` in objective and constraint position, and
  `SUM(cap1 + cap2)`. A query-wide decision mixed with row-varying data or a per-row
  decision is legal and is weighted once per counted row
  (`test_scalar_times_data_inside_reducer_is_weighted_by_row_data`,
  `test_scalar_plus_row_scoped_term_inside_reducer_is_legal`,
  `test_scalar_var_in_aggregate.py`).
- **`scalar` is an ordinary name** (`test_scalar_still_usable_as_identifier`,
  `test_query_wide_decision_beside_a_column_named_scalar`): it is an unreserved
  keyword, so a column, alias or CTE named `scalar` still resolves, including beside a
  query-wide decision, and as the name of a decision (`test_scope_removed_syntax.py`).
- **Empty input** (`test_scalar_empty_input`): an empty selection returns an empty
  relation, exactly as for a per-row decision. There is no special case.
- **Composition with the rewrite-heavy features**, all oracle-verified except the
  `<>` case, which is pinned to an exact value. These are per-row constraints that put
  the query-wide decision under a rewrite: `ABS(x - cap) <= 2`
  (`test_scalar_with_abs_per_row_constraint`), `POWER(x - cap, 2) <= 4`
  (`test_scalar_with_quadratic_constraint`, Gurobi only), `b * cap <= 4`
  (`test_scalar_with_bilinear_per_row_constraint`, McCormick), and `cap <> 8`
  (`test_scalar_with_not_equal`, Big-M). Each rewrite appends auxiliaries to the
  global block, which sits immediately above the query-wide block in `VarIndexer`'s
  four-block layout — these are the cases where a misplaced block boundary would
  surface. Query-wide decisions beside reducers under `<>`, beside boolean products and
  squared reducers, and in products of decisions are in `test_scope_bare_clauses.py`.
- **The query-wide coefficient survives both reducer desugarings**, oracle-verified.
  `norm(e, 1)` becomes `SUM(ABS(e))` and a quadratic objective becomes
  `SUM(POWER(e, 2))`, so both put the decision inside a reducer body mixed with
  row-varying data, and both must pick up the same per-row weighting the plain
  `SUM(data * cap)` path uses.
  - `test_scalar_inside_norm_is_weighted_per_row` —
    `norm(l_linenumber * cap - 3, 1) <= 85`. The bound is chosen so the three readings
    of the coefficient give three different answers: per-row and data-weighted
    (correct, `cap = 1`), one collapsed ABS over the totals (`cap = 2`), and the data
    multiplier dropped (`cap = 6`). The test asserts the fixture still separates them,
    so it fails loudly if the data shifts.
  - `test_scalar_inside_quadratic_objective_is_weighted_per_row` —
    `MINIMIZE SUM(POWER(l_linenumber * cap - 4, 2))` with `cap` REAL, so the optimum is
    the continuous least-squares point `4*SUM(d)/SUM(d^2)` rather than an integer a
    misreading could hit by accident; dropping the data multiplier would put it at 4.
    REAL also keeps the model a plain QP, so this runs on both backends instead of
    needing Gurobi's MIQP.
- **A query-wide decision as the bound of a reduced constraint**, with and without a
  `per` key (`test_scalar_as_aggregate_rhs`, `test_scalar_as_aggregate_rhs_with_per`:
  `per c_nationkey: sum(x) by (c_nationkey) <= cap`). Canonicalization moves it to the
  decision side, and the same single column takes part in every class's row.
- **Diagnostics** (`test_unbounded_scalar_reports_a_single_instance`): an unbounded
  query-wide decision produces one runaway finding with no amount or categorical slice
  — there is no subset of rows to characterize.

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| query-wide | per-row decision in the same query | ✓ |
| query-wide | row-varying comparison (`ship <= cap`) | ✓ |
| query-wide | bare in objective (`MINIMIZE cap`) | ✓ |
| query-wide | additive beside a reducer (`MINIMIZE 2*cap - SUM(x)`) | ✓ |
| query-wide | JOIN source | ✓ |
| query-wide | split clause order | ✓ (`test_clause_order.py`) |
| query-wide | colon-form reducer | ✓ when weighted by the relation's data; a bare row-invariant body is rejected |
| query-wide | empty input | ✓ |
| query-wide | ABS (per-row constraint) | ✓ |
| query-wide | quadratic constraint (QCQP, Gurobi) | ✓ |
| query-wide | bilinear `b * cap` (McCormick) | ✓ |
| query-wide | `<>` (Big-M disjunction) | ✓ |
| query-wide | unbounded diagnostics report | ✓ |
| query-wide | reduced-constraint bound | ✓ |
| query-wide | `per` | ✓ |
| query-wide | `norm(...)` (L1, desugars to `SUM(ABS(...))`) | ✓ |
| query-wide | quadratic objective (`SUM(POWER(...))`) | ✓ |

## Relation-qualified reducers

Tests live in `test/decide/tests/test_qualified_reducer.py`, plus
`test_clause_order.py::test_entity_scoped_declaration_in_split_slot` for a
qualified reducer in the split clause order.

`SUM(D: expr)` reduces over `D`'s tuple identities instead of over join-result
rows: it means `sum(per D: expr)`, one value per class of `D`'s columns, and `D`
must determine the body, checked against the data at run time ("per D does not
determine ..."). Semantics: `../language/functions.md` and
`../language/scopes.md`; syntax: `../language/guide.md`
§5.1. The equivalence with `sum(per D: e)` is pinned in `test_scope_reducer_inner.py`
and `test_scope_removed_syntax.py::test_colon_form_stays`.

The fixture is `customer JOIN nation`, where the join repeats each nation once
per customer and nations have unequal customer counts — that inequality is what
makes the row-weighted and identity-weighted optima diverge.

### Scenarios covered

- **Qualified SUM charges each entity once**
  (`test_qualified_sum_charges_each_entity_once`, oracle-verified).
- **Qualified and unqualified diverge** (`test_qualified_and_unqualified_diverge`,
  oracle-verified): the same query both ways, with two oracles. This is the test
  that would fail if the qualifier were parsed and then dropped — the failure
  mode the feature is most exposed to.
- **Qualified AVG's denominator is the distinct-identity count, not the row
  count** (`test_qualified_avg_denominator_is_distinct_entities`,
  oracle-verified). The denominator falls out of the de-duplication mask rather
  than being computed separately, so this pins the mask's placement.
- **Qualified MIN / MAX** (`test_qualified_minmax`, parametrized over the
  easy-direction pair): accepted and carried, with no effect — dropping repeats
  of a value already present cannot move an extremum.
- **Constraint position** (`test_qualified_reducer_in_constraint`,
  oracle-verified).
- **Under a `per` key** (`test_qualified_reducer_with_per`,
  `per n.n_nationkey: sum(n: ...) by (n.n_nationkey) <= 8`): de-duplication runs
  inside each `by` group; every class holds one nation, so its charge is that
  nation's cost, counted once.
- **Mixed qualified and unqualified reducers in one objective**
  (`test_mixed_qualified_and_unqualified_objective`, oracle-verified): pins that
  the mask is per *term*, not per clause.
- **Hard-direction MIN / MAX with a qualifier**:
  `test_qualified_hard_max_objective` (`MAXIMIZE MAX(n: ...)`, the Big-M
  direction) and `test_qualified_hard_min_constraint` (`MIN(n: ...) <= K`). The
  easy directions emit no auxiliaries; these confirm that de-duplication cannot
  move an extremum under the Big-M encoding either, where the mask and the
  indicator rows meet.
- **The objective's `when` composed with the qualifier**
  (`MAXIMIZE when c: sum(n: ...)`):
  `test_qualified_reducer_with_aggregate_local_when_in_objective` uses nations 5
  and 14, whose row counts invert the ranking (identity weights 5 and 14, row
  weights 45 and 28), so a dropped qualifier changes *which nation is chosen*,
  not merely the objective value.
  `test_aggregate_local_when_filters_inside_a_qualified_reducer` then flips the
  answer using the filter alone. Together they pin that both filters apply — the
  qualifier's keep mask and the `when` share one `TermFilterState` slot.
- **A reducer's own `when` with a relation key, in a constraint**:
  `test_qualified_reducer_with_aggregate_local_when_in_constraint`
  (`sum(when n.n_nationkey > 5 per n: keepN) <= 1`). Nation 5 sits outside the
  filter so it never counts against the cap and is always kept; nations 14/15/16 do
  count and only the value-maximizing one (16) fits, so nation 5's keep value is
  what distinguishes a correctly scoped `when` from a dropped one.
- **Composed MIN/MAX keeps the qualifier**
  (`test_composed_minmax_preserves_the_qualifier`,
  `test_composed_minmax_preserves_the_qualifier_in_a_constraint`): adding
  `+ MAX(...)` to a clause holding `SUM(D: ...)` must not change what the qualifier
  means. Both use the inverting 5/14 fixture, so a dropped qualifier changes the
  chosen nation (objective) or empties the selection (constraint).
- **A composite qualifier weights differently from both alternatives**
  (`test_three_relation_composite_qualifier_differs_from_single_and_unqualified`,
  oracle-verified): a customer/orders/lineitem chain where `sum(c, o: ...)`,
  `sum(c: ...)` and the unqualified `sum(...)` weight the same decision by distinct
  order count, by 1, and by lineitem row count respectively — each checked against
  its own independent oracle model. A two-relation fixture cannot tell these apart, which is
  why this one needs three.
- **Naming every relation the query joins is a no-op**
  (`test_two_relation_composite_qualifier_equals_unqualified_when_query_has_only_those_two_relations`):
  with no third, unnamed relation left to fan out, `SUM(D, T: expr)` and the
  unqualified `SUM(expr)` agree row for row.
- **A query-wide decision inside a qualified reducer is weighted and de-duplicated**
  (`test_qualified_reducer_scalar_times_entity_data_is_weighted_and_deduplicated`,
  oracle-verified): `SUM(n: (n_nationkey + 1) * cap)` over `customer JOIN nation`
  weights `cap` by the sum over *distinct* nations, not over join-result rows.
- **A qualified reducer as a bound** (`test_qualified_reducer_as_a_bound`,
  `SUM(n: keepN) <= SUM(n: n_nationkey) / 25`): the qualifier de-duplicates the
  bound side too.
- **Rejections** (error tier). At bind time: an unknown relation name, including one
  inside a multi-relation list
  (`test_unknown_relation_in_multi_relation_qualifier_rejected`), and a row-invariant
  body, i.e. a query-wide decision standing alone inside the reducer
  (`test_query_wide_decision_alone_inside_qualified_reducer_rejected`). At run time,
  the data check `sum(per n: ...)` takes: a column from another relation ("per n does
  not determine c.c_acctbal (in sum(n: c.c_acctbal * keepN))") and a per-row decision
  ("per n does not determine the decision y (in sum(n: ...))").

### Caveats

- `test_row_scoped_decision_inside_qualified_reducer_rejected` pins settled
  semantics: `n` does not determine a per-row decision, so de-duplicating a
  repeated identity cannot pick one of its values. The message quotes the reducer
  as written.
- `test_query_wide_decision_alone_inside_qualified_reducer_rejected` pins the
  general row-invariance rule (a body with nothing to aggregate over), while the
  paper's §3.2.2 carve-out for a weighted query-wide decision is covered by the
  neighbouring positive test.
- `test_composed_minmax_preserves_the_qualifier` and
  `…_in_a_constraint` guard the qualifier on the composed path:
  `ComposedMinMaxTerm::qualifier_scope_idx` carries it, and without it
  `SUM(D: ...) + MAX(...)` falls back to row semantics. Objective and constraint are
  separate code paths, hence two tests.

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| qualified reducer | SUM | ✓ |
| qualified reducer | AVG | ✓ |
| qualified reducer | MIN / MAX (easy direction) | ✓ |
| qualified reducer | constraint position | ✓ |
| qualified reducer | objective position | ✓ |
| qualified reducer | `per` key | ✓ |
| qualified reducer | unqualified reducer in the same objective | ✓ |
| qualified reducer | keyed decision | ✓ |
| qualified reducer | per-row decision (run-time data check) | ✓ |
| qualified reducer | a column of another relation (run-time data check) | ✓ |
| qualified reducer | bound side | ✓ |
| qualified reducer | query-wide decision times entity data | ✓ (oracle) |
| qualified reducer | query-wide decision as the whole body (rejected) | ✓ |
| qualified reducer | split clause order | ✓ (`test_clause_order.py`) |
| qualified reducer | hard-direction MAX objective | ✓ |
| qualified reducer | hard-direction MIN constraint | ✓ |
| qualified reducer | the objective's `when` | ✓ |
| qualified reducer | a reducer's own `when` with a relation key (constraint) | ✓ |
| qualified reducer | composed MIN/MAX (objective) | ✓ |
| qualified reducer | composed MIN/MAX (constraint) | ✓ |
| qualified reducer | multi-relation qualifier, 3 relations | ✓ (oracle) |
| qualified reducer | multi-relation qualifier, no-op case | ✓ |
| qualified reducer | unknown relation in a multi-relation list (rejected) | ✓ |

## Constraint operators

Covers the constraint operators `=`, `<`, `<=`, `>`, `>=`, `<>`, `BETWEEN`, and `IN`, plus structural forms (per-row, aggregate, multi-constraint, mixed). Tests live in:
- `test/decide/tests/test_cons_comparison.py` — all 6 comparison operators
- `test/decide/tests/test_cons_between.py` — BETWEEN
- `test/decide/tests/test_cons_in.py` — IN on decision variables
- `test/decide/tests/test_cons_perrow.py` — per-row constraints
- `test/decide/tests/test_cons_aggregate.py` — aggregate (SUM) constraints
- `test/decide/tests/test_cons_mixed.py` — per-row + aggregate combined
- `test/decide/tests/test_cons_multi.py` — multiple aggregate constraints

### Scenarios covered

#### Comparison operators

All 6 operators tested on both per-row and aggregate constraints,
oracle-verified (`test_cons_comparison.py` and others).

- **`<>` (NE Big-M)** (`test_cons_comparison.py`, plus `test_aggregate_local_when.py` for a reducer's own `when`): under a `when` in front and inside a reducer, with the `<>` binding or not, and a mixed-sign coefficient column (both-sided disjunction). `<>` on REAL LHS is rejected.
- **Strict `<` / `>` rejection coverage** (`test_cons_comparison.py`): rejected on REAL LHS (per-row and aggregate), non-integer coefficient, mixed BOOL + REAL LHS, quadratic path (`SUM(POWER(real, 2))`), and bilinear Bool × Real path.
- **Strict `<` / `>` accepted shapes** (oracle-verified): pure INT SUM (per-row and aggregate), BOOL × INT bilinear LHS, INT × INT bilinear LHS (Gurobi), `SUM(ABS(integer_expr))`.
- Regression: `<=` / `>=` on REAL LHS still work.

#### BETWEEN

- **Oracle-verified** (`test_cons_between.py`): per-row BETWEEN, fractional bounds on REAL var, column-derived bound (`x BETWEEN 0 AND col`) combined with multi-constraint + aggregate, aggregate BETWEEN standalone desugar (two constraints from one BETWEEN), and over a reducer with its own `when` (`test_aggregate_local_when.py`).
- BETWEEN on a keyed decision: `test_entity_scope.py::test_entity_scoped_between_constraint`, oracle-verified on the objective.
- Error tests (`test_error_binder.py`): non-scalar RHS, DECIDE var in BETWEEN bounds.

#### IN

- **Oracle-verified** (`test_cons_in.py`): `x IN (values)` on decision variable, non-integer values on REAL var, `x IN (0, 1)` on BOOL (no-op optimization), single-value IN rewritten to `x = v`, IN under a `when` in front.
- Error tests (`test_error_binder.py`): `SUM(x) IN (...)` rejected, DECIDE var in IN RHS rejected.

#### Structural forms

All oracle-verified: per-row bounds (`test_cons_perrow.py`), aggregate
constraints (`test_cons_aggregate.py`), per-row + aggregate combined
(`test_cons_mixed.py`), multiple aggregate constraints (`test_cons_multi.py`),
subquery RHS (`test_cons_subquery.py`), correlated subquery RHS
(`test_cons_correlated_subquery.py`).

#### Edge cases

All oracle-verified: RHS = 0 forces all zero, negative objective coefficients
(`test_edge_cases.py`); negative coefficients in aggregate constraints — both
signed column and negative constant literal multiplier
(`test_cons_comparison.py`).

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| Comparison operators | aggregate SUM | ✓ (all 6) |
| Comparison operators | per-row | ✓ (all 6) |
| `<>` | a bound varying within a `by` group | ✓ (`test_per_clause.py`: every exclusion kept without a `per` key; the determination error under one) |
| `<>` | keyed decision | ✓ |
| `<>` | REAL LHS (rejected) | ✓ |
| BETWEEN | keyed decision | ✓ |
| BETWEEN | a reducer's own `when` | ✓ |
| BETWEEN | `per` + REAL var (fractional bounds) | ✓ (`test_per_clause.py::test_real_between_per_oracle`) |
| IN | `when` | ✓ |
| IN | BOOL domain restriction | ✓ |
| Negative coefficients | objective | ✓ |
| Negative coefficients | aggregate constraint (signed column) | ✓ |
| Negative coefficients | aggregate constraint (constant literal) | ✓ |
| Multiple constraints | different operators | ✓ |

## `when`

Tests live in:
- `test/decide/tests/test_when_constraint.py` — `when c:` in front of an aggregate constraint
- `test/decide/tests/test_when_perrow.py` — `when c:` in front of a per-row constraint
- `test/decide/tests/test_when_objective.py` — `MAXIMIZE|MINIMIZE when c: E`
- `test/decide/tests/test_when_compound.py` — compound AND/OR conditions
- `test/decide/tests/test_when_grammar.py` — parenthesized and bare conditions
- `test/decide/tests/test_aggregate_local_when.py` — a reducer's own `when`, `agg(when c: e)`

The scoping language's own suites (kept rows, NULL conditions, the empty-set errors,
the grid) are in [`scope_suite.md`](scope_suite.md). Syntax:
`../language/guide.md` §4, §5.3, §6.3 and §7.2.

### Scenarios covered

All oracle-verified unless noted:

- **`when` in front of an aggregate constraint** (`test_when_constraint.py`):
  string-equality and numeric conditions, a constant coefficient in the reducer, `<>`
  in the condition, every row kept (the same as no `when`), NULL in the condition column
  (the row is dropped), an explicit `IS NOT NULL` condition, conditional and
  unconditional constraints together, a different `when` per constraint, constraint
  order invariance.
- **`when` in front of a per-row constraint** (`test_when_perrow.py`): force to zero,
  force selection, a numeric row filter, every row kept, and a REAL decision with no
  implicit [0, 1] cap (`test_when_perrow_real`).
- **`when` on the objective** (`test_when_objective.py`): MAXIMIZE and MINIMIZE under a
  `when`, an unconditional constraint beside it, the same and different conditions on
  the constraint and the objective, every row kept.
- **Compound conditions** (`test_when_compound.py`): AND and OR, nested, mixed string
  and numeric, on aggregate and per-row constraints and on the objective.
- **Condition grammar** (`test_when_grammar.py`): parenthesized `NOT`, comparison and
  arithmetic conditions on a reducer's `when` and on the objective's; a bare comparison
  after the objective's `when` gives the same answer as its parenthesized spelling. A
  condition runs to its colon, so no condition needs parentheses.
- **A reducer's own `when`** (`test_aggregate_local_when.py`): independent filters on
  additive reducers (one reducer, three, overlapping filters, filtered beside
  unfiltered, in the constraint and the objective); a `when` in front applied before a
  reducer's own; composition with AVG, `per`, AVG + `per`, easy MAX, hard MAX (`>=`,
  Big-M indicators only on kept rows), bilinear terms (constraint and objective), keyed
  decisions, `BETWEEN`, `<>` (plain and under `per`); constant offsets and scalar
  multipliers around a filtered reducer; a data column multiplying or dividing a
  filtered reducer (`col * sum(when w: x) <= 20`), which takes the value shape and is
  judged by the scope oracle.

#### Empty row sets

A `when` in front that keeps no row is an error, "when c matches no rows." (D4):
`test_when_constraint.py::test_when_no_rows_match`,
`test_when_perrow.py::test_when_perrow_no_matches`,
`test_when_objective.py::test_when_objective_no_match`. A reducer whose own `when`
keeps no row is an error, "agg(when c: e) has no rows." (D8):
`test_aggregate_local_when.py::test_aggregate_local_when_all_filtered_out` and the
mixed-filter cases, and the empty MIN/MAX, SUM and AVG cases of `test_edge_cases.py`
(see [`coverage.md#min-and-max`](coverage.md#min-and-max)). A `when` in front that leaves one
`per` class without a kept row generates nothing for that class
(`test_per_multi_column.py::test_multi_column_per_when_eliminates_all_in_group`).

#### Error cases

A decision in a condition is refused with "x is a decision; when, per and by may only
use data" — `test_error_binder.py` (plain and compound) and
`test_aggregate_local_when.py` (a reducer's `when` and the objective's). The removed
spellings of a condition are refused by the parser (`../language/errors.md` §3) —
`test_scope_removed_syntax.py`.

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| `when` | aggregate constraint | ✓ |
| `when` | per-row constraint | ✓ (BOOL, INT, REAL) |
| `when` | objective | ✓ |
| `when` | compound AND/OR | ✓ |
| `when` | `per` | ✓ |
| `when` | MIN/MAX (easy) | ✓ |
| `when` | MIN/MAX (hard, a reducer's own `when`) | ✓ |
| `when` | AVG | ✓ |
| `when` | ABS (objective) | ✓ |
| `when` | QP | ✓ |
| `when` | quadratic constraint | ✓ |
| `when` | `<>` comparison under a `when` in front | ✓ (`test_cons_comparison.py`) |
| `when` | `<>` (a reducer's own) | ✓ |
| `when` | keyed decisions | ✓ |
| `when` | bilinear | ✓ |
| `when` | NULL in condition column | ✓ |
| `when` | a data column scaling a filtered reducer | ✓ (scope oracle) |

## `per` constraints and two-level objectives

Tests live in:
- `test/decide/tests/test_per_clause.py` — `per K:` on constraints
- `test/decide/tests/test_per_multi_column.py` — keys of several columns
- `test/decide/tests/test_per_qualified.py` — relation-qualified key columns (`per s.s_nationkey:`)
- `test/decide/tests/test_per_objective.py` — the two-level objective `outer(per k: inner(e) by (k))`
- `test/decide/tests/test_per_interactions.py` — `per` composed with auxiliary-variable features (hard MIN/MAX, ABS, several decisions)

The scoping language's own suites (generation, `by`, determination, the grid) are in
[`scope_suite.md`](scope_suite.md). Syntax: `../language/guide.md`
§4, §5.2 and §6.2.

### Scenarios covered

#### `per K:` on constraints

All oracle-verified:

- **Basic** (`test_per_clause.py`, `test_per_multi_column.py`): one key column with
  `per K: sum(x) by (K) <= c`, keys of two and three columns (`per a, b:` with
  `by (a, b)`), INT decisions, a `per` constraint beside an objective `when`, a `when`
  in front of a multi-column `per` (on another column and on a key column), `<>` per
  class, two `per` constraints on different columns, a `when` that leaves some classes
  without a kept row (those classes generate nothing).
- **NULL keys** (`test_per_clause.py::test_per_null_group_key`): a NULL key value is a
  class of its own; `when grp is not null per grp:` leaves those rows out. Both forms
  are judged by the scope oracle.
- **Bounds that vary within a group** (`test_per_clause.py`): with no `per` key,
  `sum(x) by (grp) <> cap` is generated once per row, so every excluded value is kept;
  under `per grp:` a bound that varies within a class is the determination error
  (`test_per_not_equal_varying_bound_under_per_key_does_not_determine`).
- **Qualified key columns** (`test_per_qualified.py`): one and several qualified
  columns, mixed qualified and bare, equivalence with the unqualified spelling, a
  table alias, a qualified column in a `when` and in the two-level objective, and an
  unknown qualifier rejected cleanly.
- **Auxiliary-variable interactions** (`test_per_interactions.py`): hard `max(e) >= K`
  and hard `min(e) <= K` per class (Big-M per group), `max(e) = K` (easy and hard
  together), `sum(abs(...))` (ABS auxiliaries per group), BOOL and INT decisions
  together, a `when` with several decisions, a QP objective beside a `per` constraint.
- **Degenerate shapes**: one-row classes, a class whose reducer has zero coefficients,
  and a NULL key under a `when` (`when active per grp:`, judged by the scope oracle) —
  all `test_per_interactions.py`; a `per` equality (two-sided bound per class,
  `test_abs_linearization.py`); a feasibility query with a `per` constraint
  (`test_edge_cases.py`); an uncorrelated scalar subquery as the bound
  (`test_cons_subquery.py`); a `BETWEEN` per class on REAL decisions
  (`test_per_clause.py::test_real_between_per_oracle`).

#### Two-level objectives

`test_per_objective.py` covers `outer(per k: inner(e) by (k))` with outer ∈ {SUM, MIN,
MAX} and inner ∈ {SUM, MIN, MAX, AVG}: `sum(per k: sum(e) by (k))` equal to the flat
`sum(e)`, every `sum(per k: max/min(...))` direction (easy and hard inner),
`max/min(per k: sum(...))`, `sum(per k: avg(...))` with unequal and extremely unequal
groups, `max(per k: avg(...))` and `min(per k: avg(...))`, the two-level form under a
`when` in front, a single class, and hard inner MIN/MAX over `power(...)` (Gurobi).
`max(per g: max(c * x) by (g))` with mixed-sign coefficients is in `test_min_max.py`.

#### Error cases

- An outer AVG over the classes is refused ("not supported yet") —
  `test_scope_explain_diagnose.py::test_an_outer_avg_over_the_classes_is_refused`.
- The removed spellings of a grouped constraint and of a grouped objective are
  refused by the parser (`../language/errors.md` §3) — `test_scope_removed_syntax.py`.

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| `per` | `when` in front | ✓ |
| `per` | a reducer's own `when` | ✓ |
| `per` | several key columns | ✓ |
| `per` | qualified key columns | ✓ |
| `per` | INT decisions | ✓ |
| `per` | NULL key values (a class of their own) | ✓ |
| `per` | `<>` | ✓ |
| `per` | MIN/MAX (easy, a bound on every member) | ✓ |
| `per` | MIN/MAX (two-level objective) | ✓ |
| `per` | AVG | ✓ |
| `per` | keyed decisions | ✓ |
| `per` | quadratic constraint | ✓ |
| `per` | quadratic constraint + `when` | ✓ |
| `per` | QP objective | ✓ |
| `per` | `when` + keyed decisions (triple) | ✓ |
| `per` | `when` + MIN/MAX (triple) | ✓ |
| `per` | `when` + AVG (triple) | ✓ |
| `per` | hard MIN/MAX constraints (Big-M per group) | ✓ |
| `per` | ABS in a reducer | ✓ |
| `per` | several decisions (BOOL + INT) | ✓ |
| `per` | `when` + several decisions | ✓ |
| `per` | equality constraint | ✓ |
| `per` | feasibility (no objective) | ✓ |

## MIN and MAX

> **Composed MIN/MAX coverage must use a multi-table join.** On a single-table `VALUES`
> source a logical column index and a chunk position coincide, so a composed path that
> read the wrong data column over a join would still pass.
> `test_composed_minmax_entity_scoped_multi_row` is the join-based guard; keep new
> composed tests on a join for the same reason.

Tests live in:
- `test/decide/tests/test_min_max.py` — primary MIN/MAX test file
- `test/decide/tests/test_min_max_multiterm.py` — row expressions that are not a single product term
- `test/decide/tests/test_per_objective.py` — two-level objectives
- `test/decide/tests/test_aggregate_local_when.py` — a reducer's own `when` with MAX
- `test/decide/tests/test_per_interactions.py` — hard MIN/MAX constraints under `per` (per-group Big-M)
- `test/decide/tests/test_edge_cases.py` — empty row sets under MIN/MAX (see [`coverage.md#edge-cases-and-data-shapes`](coverage.md#edge-cases-and-data-shapes))

MIN/MAX linearization is performed by `DecideOptimizer::RewriteMinMax`. It
classifies each occurrence as **easy** (naturally per-row, no Big-M) or
**hard** (needs a global auxiliary variable and per-row binary indicators).

### Scenarios covered

All oracle-verified unless noted.

- **Easy cases** (`test_min_max.py`, plus keyed variants in `test_entity_scope.py` and a reducer's own `when` in `test_aggregate_local_when.py`): `MAX(expr) <= K` / `MIN(expr) >= K` bound every member on its own row, with column coefficients (`MAX(x * expr) <= K`), on INT vars, under a `when`, under `per g: ... by (g)` (the same per-row bounds), `when` + `per` together, keyed MAX/MIN, a reducer's own `when` on an easy MAX. Over a bare decision with no `when` and no reducer filter the bound is absorbed into the decision's box (D41), so a negative K makes the decision signed (`test_scope_bare_clauses.py`).
- **Hard cases (Big-M indicators)** (`test_min_max.py`, `test_per_interactions.py`, `test_entity_scope.py`, `test_aggregate_local_when.py`): `MAX(expr) >= K`, `MIN(expr) <= K`, equality for both; multiple MIN/MAX constraints in one query; MIN/MAX in both constraint and objective; keyed hard MAX; `per g:` variants with Big-M per group (`max(e) by (g) >= K`, `min(e) by (g) <= K`, `max(e) by (g) = K` combining easy + hard per group); hard MAX with a reducer's own `when`; a reducer over data as the bound, `MAX(x) <= MAX(v)` (easy) and `MAX(x) >= MAX(v)` (hard), judged by the scope oracle (`test_max_against_a_reducer_bound`).
- **Objectives** (`test_min_max.py`): easy `MINIMIZE MAX(expr)` / `MAXIMIZE MIN(expr)` (plain, on INT, and under a `when` in front); hard `MAXIMIZE MAX(expr)` / `MINIMIZE MIN(expr)`.
- **Multi-term and constant row expressions** (`test_min_max_multiterm.py`): the inner expression is a linear combination, not a single product — `MAX((qty + 1) * x)` (distributes into two terms on one column), `MAX(qty * x + x)` (the same shape written out), `MAX(qty * x + 5)` (constant folds into the bound). Covered on the flat path (both directions), the composed path, and the two-level outer-MIN path with a **keyed** decision, where one column spans every row of a group so a single term is enough to repeat it. Each test pins the objective value against the oracle, so a linking row that is accepted but mis-linearized still fails.

#### Two-level objectives

`test_per_objective.py` covers `outer(per k: inner(e) by (k))` with outer ∈ {SUM,
MIN, MAX} and inner ∈ {SUM, MIN, MAX, AVG}: `sum(per k: max(...))` and
`sum(per k: min(...))` (both directions, easy and hard inner), `max(per k: sum(...))`,
`min(per k: sum(...))`, and `max`/`min`/`sum` over an inner `avg`.
`max(per g: max(c * x) by (g))` with mixed-sign coefficients is in `test_min_max.py`.
An outer AVG is refused ("not supported yet").

#### Composed MIN/MAX (additive LHS/objective with mixed aggregate terms)

Oracle-verified in `test_min_max.py`: easy-direction additive constraints and
objectives; hard-direction constraints and objectives; scalar-multiplied terms;
and subtraction such as `MAX(...) - MIN(...) <= K`. A composed clause under a
`per` key (`per g: sum(x * v) by (g) + max(when w: x * v) by (g) <= 12`), under a
`when` in front, and against a column or subquery bound answers, judged by the
scope oracle (`test_composed_minmax_per_scope_answers`,
`test_composed_minmax_front_when_answers`, `test_composed_minmax_column_rhs_answers`,
`test_composed_minmax_subquery_rhs_answers`). Composed MIN/MAX expressivity:
`../language/functions.md` and `../language/functions.md`.

#### Error cases (binder rejections)

`test_min_max.py`: `MAX(x) <> K` rejected (`test_max_notequal_error`). The
removed spellings of a grouped MIN/MAX objective are refused by the parser
(`../language/errors.md` §3, `test_scope_removed_syntax.py`).

`<>` is the only outer comparison the composed path refuses, pinned by
`test_composed_minmax_outer_not_equal_rejected` (over a whole-numbered column so
the generic fractional-`<>` refusal does not fire first).

#### Outer equality and `BETWEEN` on a composed MIN/MAX

`SUM(x*v) + MAX(x*v) = K` solves. An equality holds the composed sum from both
sides, so every `z_k` in it carries the envelope *and* the closing indicator
rows, and the outer row goes to the backend as one native `=`.
`test_composed_minmax_outer_equality_max` checks it against an oracle model that
spells that pinning out by hand; `test_composed_minmax_outer_equality_min`
covers the `MIN` arm on an instance where one selection is forced.

`BETWEEN` arrives at the composed path as its two directional halves, so both
are ordinary supported bounds and the constraint solves —
`test_composed_minmax_between_is_supported`. Equal endpoints mean the same thing
as `= K` and agree with it
(`test_composed_minmax_equality_via_degenerate_between`), but they are the more
expensive spelling: two comparisons allocate an auxiliary each, where `= K`
allocates one and pins both its sides.

#### Empty row sets

Both empty-set errors are raised before the solver runs, so an empty set never leaves
a MIN/MAX auxiliary free (a vacuous constraint or a meaningless objective): a `when` in
front that keeps no row ("when c matches no rows.", D4) and a reducer whose own `when`
keeps no row ("max(when c: e) has no rows.", D8). The tests
are in `test_edge_cases.py` and cover flat objectives in all four directions, hard and
easy constraints, composed `sum(...) + max(when c: ...)`, mixed empty and populated
reducers, SUM and AVG, and a `per` class the `when` in front leaves empty (no
constraint for it) — see [`coverage.md#edge-cases-and-data-shapes`](coverage.md#edge-cases-and-data-shapes) → "Empty row
sets".

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| MIN/MAX (easy) | `when` | ✓ |
| MIN/MAX (hard) | a reducer's own `when` | ✓ |
| MIN/MAX (easy) | `per` (per-row bounds) | ✓ |
| MIN/MAX (easy) | `when` + `per` | ✓ |
| MIN/MAX (inner or outer) | two-level objective | ✓ |
| MIN/MAX (easy) | keyed decision | ✓ |
| MIN/MAX (hard) | keyed decision | ✓ |
| MIN/MAX | INT variables | ✓ |
| MIN/MAX (hard) | `per` (per-group Big-M) | ✓ |
| composed MIN/MAX | `per`, `when` in front, column or subquery bound | ✓ (scope oracle) |
| MIN/MAX | multiple constraints in same query | ✓ |
| MIN/MAX | constraint + objective in same query | ✓ |

## AVG

Tests live in `test/decide/tests/test_avg.py` plus interactions in
`test_aggregate_local_when.py` and `test_per_objective.py`.

### Scenarios covered

All oracle-verified unless noted:

- **Core** (`test_avg.py`): `AVG(x) op K` constraints, flat AVG objective (= SUM argmax), `when c: avg(...)` (divides by the kept rows), `per g: avg(...) by (g)` (per-group average), `when c per g: avg(...) by (g)`, BOOL and INT variables, AVG in bilinear constraint, `AVG(x) <> K` (Big-M + RHS scaling, plain and under a `when`), AVG with no DECIDE variable (passthrough, no oracle needed).
- **A reducer's own `when`** (`test_aggregate_local_when.py`): `avg(when c: e)`, alone and under `per g: ... by (g)`.
- **Two-level objectives** (`test_per_objective.py`): `sum(per col: avg(x * cost) by (col))` with unequal groups, including extreme 1:5 group-size asymmetry; `max(per col: avg(...) by (col))` (easy MAX) and `min(per col: avg(...) by (col))` (easy MIN).

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| AVG | BOOL | ✓ |
| AVG | INT | ✓ |
| AVG | `when` in front | ✓ |
| AVG | a reducer's own `when` | ✓ |
| AVG | `per` | ✓ |
| AVG | `when` + `per` | ✓ |
| AVG (inner) | SUM / MAX / MIN (outer) in the two-level objective | ✓ |
| AVG | bilinear constraint | ✓ |
| AVG | `<>` (NE Big-M) | ✓ (denominator hoisted to RHS at execution time; LHS stays integer-valued SUM — see `physical_decide.cpp` `ne_avg_rhs_scale` flag) |
| AVG | keyed decision | ✓ (`test_entity_scope.py::test_entity_scoped_with_avg`) |

## ABS

Tests live in:
- `test/decide/tests/test_abs_linearization.py` — primary ABS coverage
- `test/decide/tests/test_per_interactions.py` — ABS in a reducer under `per` (ABS auxiliaries per group)

The ABS linearization is performed by `DecideOptimizer::RewriteAbs`. For each
`ABS(expr)` referencing a DECIDE variable, an auxiliary REAL variable `d` is
introduced with the lower envelope `d >= expr` and `d >= -expr`. ABS occurrences
that don't naturally pin `d` to `|expr|` (MAXIMIZE objective, or constraint
shapes that don't upper-bound `d`) additionally get a Big-M sign-indicator
binary `y` and the upper envelope `d <= expr + 2M(1-y)` and `d <= -expr + 2M*y`.
The classifier `TagAbsForBigM` runs before `RewriteAbs` and tags
Path-B occurrences, per ABS *occurrence* and by the sign it carries in
`LHS - RHS` rather than by the side it was written on. See
`../language/functions.md` for the full Path-A / Path-B
classification.

### Scenarios covered

- **Sound directions** (oracle-verified in `test_abs_linearization.py`): ABS in objective (basic, under a `when`, beside a `per` constraint on a separate SUM); per-row `ABS(expr) <= K`; aggregate `SUM(ABS(expr)) <= K` (plain and under a `when`); multiple ABS terms in one expression; mixed BOOL + REAL variables (`test_abs_mixed_vars`); ABS with no DECIDE variable (passthrough, no oracle needed).
- **`per` interaction**: ABS in a reducer under `per` (per-group auxiliaries) — `test_per_interactions.py::test_per_abs_aggregate`, oracle-verified.
- **Hard directions (Big-M)** — oracle-verified per-row `>=` and `=`, aggregate
  `SUM(ABS) >= K`, `MIN(ABS) >= K`, `MAX(ABS) >= K`, `BETWEEN`, and ABS on both
  sides of a comparison. The stress corpus C33–C37 supplies an additional model-shape
  smoke check.
- **Hard direction composed with `when` and `per`** (oracle-verified in
  `test_abs_linearization.py`): the sign-indicator envelope that pins each `d_i`
  is per-row and unconditional, while `when` and `per` act on the reducer that
  reads those auxiliaries.
  - `test_abs_constraint_aggregate_hard_ge_with_when` —
    `when l_linenumber <= 2: sum(abs(x - l_quantity)) >= K`. K sits above the
    masked rows' dispersion at `x = 0` but below the all-rows dispersion, so an
    implementation that summed every `d_i` would leave the constraint already
    satisfied and every `x` at 0. The test asserts a non-zero objective for
    exactly that reason.
  - `test_abs_constraint_aggregate_hard_ge_with_per` —
    `per grp: sum(abs(x - target)) by (grp) >= 50`. Both groups bind on their own
    (dispersions 25 and 45 at `x = 0`) while the pooled 70 would not, so a
    global-scoping bug reports objective 0 instead of 90.

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| ABS | BOOL | ✓ |
| ABS | INT | ✓ |
| ABS | REAL | ✓ |
| ABS | Multiple variable types | ✓ |
| ABS (objective) | `when` | ✓ |
| ABS (objective) | `per` (on a sibling constraint) | ✓ |
| ABS (per-row constraint, sound direction) | — | ✓ |
| ABS (aggregate constraint, sound direction) | — | ✓ |
| ABS (aggregate constraint) | `when` (kept rows reach the auxiliaries) | ✓ |
| ABS (aggregate constraint) | `per` (auxiliaries summed per group) | ✓ |
| ABS (per-row constraint, hard direction `>=`/`=`/BETWEEN) | Big-M sign-indicator | ✓ (oracle + stress C33–C37) |
| ABS (aggregate constraint, hard direction `SUM(ABS)>=K` / `MIN(ABS)>=K` / `MAX(ABS)>=K`) | Big-M on each aux | ✓ (oracle + stress C35–C36) |
| ABS (aggregate constraint, hard direction) | `when` (kept rows over Path-B-pinned auxes) | ✓ |
| ABS (aggregate constraint, hard direction) | `per` (per-group sums over Path-B-pinned auxes) | ✓ |

## Bilinear terms

Tests live in `test/decide/tests/test_bilinear.py` plus interactions
in `test_aggregate_local_when.py` and `test_quadratic_constraints.py`.

Bilinear terms (`x * y`, two different DECIDE variables) split into two
categories:

1. **Boolean × anything**: exact MILP via McCormick envelopes, works with both
   solvers. Bool × Bool uses simpler AND-linearization.
2. **General non-convex** (Real×Real, Int×Int, Int×Real): Q matrix off-diagonal
   entries, Gurobi only (NonConvex=2).

### Scenarios covered

#### McCormick (Boolean × anything — both solvers)

All oracle-verified, in `test_bilinear.py` unless noted:

- **Objectives**: Bool × Bool (AND-linearization), Bool × Real, Bool × Int; data-coefficient scaling (`profit * b * x`); MINIMIZE with data coefficient (both McCormick and AND-linearization shapes).
- **Constraints**: Bool × Bool constraints; Bool × Real constraint (McCormick feasibility).
- **Composition**: bilinear under a `when`, under `per` (per-group McCormick auxiliaries), under `when` + `per`; a keyed Bool × a per-row Real; a reducer's own `when` on a bilinear constraint and objective (`test_aggregate_local_when.py`).
- Backward compatibility: existing linear tests still pass.

#### Non-convex (Gurobi only)

All oracle-verified; most are gurobi-gated (skipped/rejected on HiGHS-only hosts), in `test_bilinear.py`:

- **Objectives** (gurobi-gated): Real × Real, Int × Int, Int × Real; coefficients from both factor sides and shape-equivalence checks (split shape = flat product, grouped data factor `(a*b)*(x*y)` = `a*b*x*y`); expansion identities `(x+1)*y` = `x*y+y` and `(x+y)*z` = `x*z+y*z`.
- Mixed linear + bilinear objective (not gurobi-gated).
- **Constraints** (`TestBilinearConstraints`): coefficients from both factor sides, split-shape equivalence, data-column coefficients; grouped data-factor aggregate and per-row constraints `(a*b)*(x*y) >= K` (gurobi-gated).

#### Error cases

In `test_bilinear.py` (`TestBilinearErrors` and `_expect_gurobi` pattern):
triple product (`x * y * z`) rejected; quad bilinear chain (`(b1*x)*(b2*y)`,
degree-4) rejected; missing upper bound on the non-Boolean factor in
Bool × non-Bool; HiGHS rejects non-convex bilinear.

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| Bilinear | Bool × Bool (AND-linearization) | ✓ |
| Bilinear | Bool × Real (McCormick) | ✓ |
| Bilinear | Bool × Int (McCormick) | ✓ |
| Bilinear | Real × Real (Gurobi Q) | ✓ |
| Bilinear | Int × Int (Gurobi Q) | ✓ |
| Bilinear | Int × Real (Gurobi Q) | ✓ |
| Bilinear | `when` in front | ✓ |
| Bilinear | a reducer's own `when` | ✓ |
| Bilinear | `per` | ✓ |
| Bilinear | `when` + `per` (triple) | ✓ |
| Bilinear | keyed Boolean factor | ✓ |
| Bilinear | MAXIMIZE objective | ✓ |
| Bilinear | MINIMIZE objective (with data coefficient) | ✓ |
| Bilinear | constraint | ✓ (Bool × Bool, Bool × Real, and Gurobi coefficient regression cases) |
| Bilinear | linear terms (mixed) | ✓ |
| Bilinear | QP self-product (mixed) | ✓ |

## Quadratic programs

Tests live in:
- `test/decide/tests/test_quadratic.py` — QP/MIQP objectives
- `test/decide/tests/test_quadratic_constraints.py` — QCQP (quadratic constraints)

Covers `POWER(expr, 2)` via Q matrix construction. Convex forms run on both
solvers; non-convex and MIQP require Gurobi. Three syntax forms supported:
`POWER(expr, 2)`, `expr ** 2`, `(expr) * (expr)` self-product, plus negated
variants.

### Scenarios covered (objectives)

All oracle-verified; gurobi-gated cases noted:

- **Convex/concave core** (`test_quadratic.py`): `MINIMIZE SUM(POWER(x - target, 2))` (Q=PSD), `MAXIMIZE SUM(-POWER(x - target, 2))` (Q=NSD), simple squared variable, various coefficient forms, all three syntax forms (`POWER`, `** 2`, self-product), multiple variables, TPC-H data QP, QP objective over a keyed decision.
- **Non-convex** `MAXIMIZE SUM(POWER(x, 2))` — gurobi-gated.
- **MIQP** (integer vars + convex QP, `test_maximize_convex_power_integer_case_b`) — gurobi-gated.
- **Mixed linear + quadratic**: same SUM (`SUM(POWER(x-t,2) + c*x)`), sibling SUMs, and negated quadratic + linear.
- **Composition**: QP objective under a `when`; QP objective beside a `per` constraint (`test_per_interactions.py`).
- **Two-level objectives**: `sum(per grp: sum(power(x - t, 2)) by (grp))` (binding per-group cap, unconstrained, and with no constant term), `sum(per grp: avg(power(x - t, 2)) by (grp))` with unequal groups (`test_quadratic.py`); hard inner `sum(per grp: max(power(...)) by (grp))` / `min` in `test_per_objective.py` — gurobi-gated.

### Scenarios covered (constraints — QCQP)

All oracle-verified, in `test_quadratic_constraints.py`:

- **Core shapes**: per-row `POWER(expr, 2) <= K` (gurobi-gated), aggregate `SUM(POWER(expr, 2)) <= K`, zero budget (exact match), binding vs non-binding constraints, multi-variable inner expression, negated `-POWER(expr, 2)`, scaled `K * POWER(expr, 2)`, data-dependent coefficients.
- **Composition**: quadratic constraint under a `when`, under `per` (one quadratic row per group), under `when` + `per` (the `when` filters before the classes form); multiple quadratic constraints per query; QCQP (quadratic objective + quadratic constraint); mixed linear + quadratic constraints; a keyed decision + QP; REAL and INT variables; bilinear + self-product mixed.
- Infeasible quadratic constraint (negative budget) — error test, not oracle-verified.

### Backend coverage

Every test above runs on the unforced fixture, which prefers Gurobi wherever it is
linked — so on a developer machine they only ever exercise the Gurobi path.
`TestHighsQuadratic` (`test_quadratic.py`) closes that gap for the one quadratic
class HiGHS accepts, a convex continuous QP: it pins the reported repro, a mixed
linear + quadratic objective, and a two-variable `POWER` group, each against
`oracle_solver` through `decidb_cli_highs`. Before it, nothing in the suite solved
a quadratic on HiGHS at all, which is how a HiGHS-only Hessian-convention defect
survived — it returned wrong answers with no error for as long as the backend has
existed.

A multi-variable `POWER` group is refused on HiGHS rather than solved, so the
cross-term case is a *rejection* test (`TestHighsRejection`) rather than a correctness
one. It is paired with `test_gurobi_solves_what_highs_refuses`, which runs the same SQL
through `decidb_cli_gurobi` and oracle-checks it: without that pair, a gate that was too
broad — refusing a class Gurobi also mishandles — would look identical from the HiGHS
side alone.

### Error cases

- Exponent rejections (`test_quadratic.py`): `POWER(x, 3)`, variable exponents.
- Multiple POWER groups in one objective rejected (`test_quadratic.py`).
- Degree > 2 rejections — self-product of POWER, product of two POWERs, variable × POWER — each tested in both objective (`test_quadratic.py`) and constraint (`test_quadratic_constraints.py`) positions.
- HiGHS rejects non-convex QP, MIQP, and a `POWER` group coupling two decision variables (`test_quadratic.py::TestHighsRejection` + `_expect_gurobi` pattern) and quadratic constraints (`test_quadratic_constraints.py`). Each is checked to refuse at plan time, with no model built.
- Infeasible quadratic constraint with tight `match=` (`test_infeasible_negative_budget`).

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| QP objective | `when` | ✓ |
| QP constraint | `when` | ✓ |
| QP constraint | `per` | ✓ |
| QP constraint | `when` + `per` | ✓ |
| QP objective | multiple variables | ✓ |
| QP constraint | multiple variables | ✓ |
| QP | BOOL (MIQP) | ✓ (gurobi-gated) |
| QP | INT (MIQP) | ✓ (gurobi-gated) |
| QP | REAL | ✓ |
| QP | keyed decision | ✓ |
| QP constraint | bilinear (mixed) | ✓ |
| QP objective | `per` constraint | ✓ |
| QP objective | linear terms in same SUM | ✓ |
| QP objective | linear terms in sibling SUM | ✓ |
| Negated QP objective | linear terms | ✓ |
| QP objective | two-level, outer SUM | ✓ |
| QP objective | two-level, inner MIN (hard) | ✓ |
| QP objective | two-level, inner MAX (hard) | ✓ |

## Subqueries

Tests live in:
- `test/decide/tests/test_cons_subquery.py` — uncorrelated scalar subquery RHS
- `test/decide/tests/test_cons_correlated_subquery.py` — correlated subquery RHS

### Scenarios covered

All oracle-verified:

- **Uncorrelated** (`test_cons_subquery.py`): scalar subquery as constraint RHS, aggregate subquery (AVG, SUM, etc.), an uncorrelated scalar subquery as the bound of a `per` constraint (one value for every class).
- **Correlated** (`test_cons_correlated_subquery.py`): per-row bound, AVG subquery as per-row bound on REAL, boolean gate, objective coefficients, under a `when`, NULL-producing correlated with COALESCE.
- **A correlated bound that varies across rows** (`test_cons_correlated_subquery.py`):
  with no `per` key every row bounds the reducer, so the tightest bound applies
  (`test_correlated_subquery_aggregate_rhs_tightest_bound`; per class under
  `per ps_suppkey:`, where every class has one row, in
  `test_correlated_subquery_aggregate_per_rhs_tightest_per_group`); a reducer's own
  `when` does not narrow the rows the bound is read on
  (`test_correlated_subquery_rhs_ignores_aggregate_local_when`). Under a `per` key a
  bound that varies within a class is the determination error
  (`test_scope_determination.py`).

#### Error cases

`test_error_binder.py`: a correlated bound that takes two values under `=`
(`test_correlated_subquery_aggregate_equality_rhs_rejected`), a subquery bound
referencing a DECIDE variable (tight match, `test_subquery_rhs_non_scalar`), a
scalar subquery returning multiple rows.

### Feature interactions covered

| Feature A | Feature B | Tested |
|-----------|-----------|--------|
| Uncorrelated scalar subquery | constraint RHS | ✓ |
| Uncorrelated scalar subquery | `per` constraint bound | ✓ |
| Correlated subquery | per-row bound | ✓ |
| Correlated subquery | aggregate constraint | ✓ |
| Correlated subquery | objective | ✓ |
| Correlated subquery | `when` | ✓ |
| Correlated subquery | BOOL | ✓ |
| Correlated subquery | INT | ✓ |
| Correlated subquery | REAL | ✓ |
| Subquery | DECIDE variable (rejected) | ✓ (error test) |

### Caveats

Per [oracle.md](oracle.md) §8, caveat 5: subqueries inside
SUCH THAT don't inherit the connection's `search_path`. Tables must be fully
qualified (e.g. `tpch.customer`) when subqueries reference cross-schema data.

## Errors

Tests live in:
- `test/decide/tests/test_error_parser.py` — parser-level syntax errors
- `test/decide/tests/test_error_binder.py` — binder-level semantic errors
- `test/decide/tests/test_error_infeasible.py` — infeasible and unbounded model detection

### Parser errors

`test_error_parser.py`, all expecting `decidb.ParserException`: DECIDE without
SUCH THAT, DECIDE without a variable name, MAXIMIZE/MINIMIZE without an
objective expression, an unknown type, comma-separated `SUCH THAT`
constraints ("use AND between constraints").

The removed spellings (`../language/errors.md` §3) are parser
errors whose message carries "is removed" and names the replacement —
`test_scope_removed_syntax.py`. The scoping language's own
parse errors (a `per` key or `per row` on the objective, a parenthesized key, an
expression in a key) are pinned in `test/decidb/test_scope_parser.test`. See
[`scope_suite.md`](scope_suite.md).

### Binder errors

All expect `decidb.InvalidInputException` or `decidb.BinderException`.
Categories covered in `test_error_binder.py` unless noted:

- Name/scope conflicts: variable conflicts with column, duplicate DECIDE variables, unknown variable in constraint.
- Unsupported syntax in SUCH THAT: IS NULL, `SUM(x) IN (...)`, non-DECIDE variable in constraint, non-SUM/AVG/MIN/MAX function in objective.
- Non-linear / invalid aggregate: no DECIDE variable in SUM, multiple DECIDE variables in SUM, nonlinear DECIDE variables.
- RHS shape: non-scalar BETWEEN / SUM RHS / `SUM = RHS`, DECIDE variable in BETWEEN bounds, IN RHS, or constraint RHS.
- Objective: a constant added to the objective is accepted
  (`test_objective_with_addition_succeeds`); a bare data column is rejected.
- Subquery: non-scalar subquery RHS, scalar subquery returning multiple rows.
- A decision in a `when` condition, plain and compound, and in a reducer's own `when`
  (`test_aggregate_local_when.py`): "x is a decision; when, per and by may only use
  data".
- A correlated subquery bound that takes two values under `=`
  (`test_correlated_subquery_aggregate_equality_rhs_rejected`).
- `OR` between constraints, in every scope spelling, and data-only aggregates as
  divisors (`TestRefusalsInEverySpelling`).
- The scope errors — a key that does not determine, a `when` that keeps no row, a
  reducer with no row for a group, key errors — are in the `test_scope_*.py` suites
  ([`scope_suite.md`](scope_suite.md)).

### Infeasibility detection

`test_error_infeasible.py::TestInfeasibleModels`, expecting
`decidb.InvalidInputException` matching `"infeasible"`: contradictory per-row
bounds (`x >= 10 AND x <= 5`), impossible SUM constraint (`SUM(x) >= 1000`
with too few rows), negative SUM upper bound, a `when` that forces every row to
zero beside `SUM(x) >= 1` (`when l_quantity > 0: x <= 0`), and
`sum(x) by (grp) = cap` with two caps in one group
(`test_equal_bound_varying_per_group_is_infeasible`). Its `per grp:` form raises
the determination error, "per grp does not determine cap", instead
(`test_equal_bound_varying_within_a_per_class_does_not_determine`).

### Unboundedness detection

`test_error_infeasible.py::TestUnboundedModels`: unbounded MAXIMIZE with
`INT` and with `REAL` (only lower bound).

Error messages are matched exactly (`(?i)unbounded` here, `(?i)infeasible`
for the infeasible class). Gurobi's ambiguous `INF_OR_UNBD` status is
disambiguated in the backend via a `DualReductions=0` re-solve (Gurobi's
documented recipe), so DecidB always reports the definitive status; the
gurobipy oracle applies the same recipe.

### Solver-specific error paths

| Scenario | Where |
|----------|-------|
| HiGHS rejects non-convex QP | `test_quadratic.py::TestHighsRejection::test_highs_nonconvex_qp_rejected` (forced-HiGHS tight match) + `test_quadratic.py` (`_expect_gurobi` pattern) |
| HiGHS rejects MIQP | `test_quadratic.py::TestHighsRejection::test_highs_miqp_rejected` (forced-HiGHS tight match) + `test_quadratic.py` (`_expect_gurobi`) |
| HiGHS rejects quadratic constraints | `test_quadratic_constraints.py` |
| HiGHS rejects non-convex bilinear | `test_bilinear.py` |
| Bilinear without upper bound on non-Boolean | `test_bilinear.py` |
| Triple product rejected | `test_bilinear.py` |
| `POWER(x, 3)` and variable exponents rejected | `test_quadratic.py` |

## Edge cases and data shapes

Tests live in:
- `test/decide/tests/test_edge_cases.py` — boundary conditions and degenerate inputs
- `test_large_scale.py`: oracle-checked scale checks (see `../performance/benchmarking/scale_tests.md`)
- `test/decide/tests/test_sql_joins.py` — JOIN sources
- `test/decide/tests/test_explain.py` — EXPLAIN output

### Boundary conditions

Oracle-verified in `test_edge_cases.py` unless noted: an input with no rows (an
empty result, not an error), single-row input, 10-column linear combination in constraint +
objective, 6 heterogeneous constraints in one query, large-coefficient numeric
stability (1e9 coeffs + NE Big-M), RHS = 0 forcing all variables to zero,
trivially loose constraint (all selected), negative objective coefficients,
feasibility problem (no objective), all-zero objective coefficients
(`MAXIMIZE SUM(x * 0)`), and per-row decisions on a 1-to-many fan-out JOIN
(`test_entity_scope.py::test_row_scoped_vars_on_fanout_join`).

Error tests (not oracle-verified):
- NULL coefficients (with COALESCE hint) — `test_edge_cases.py::test_null_coefficients`
- Unconstrained INT var in objective (mixed w/ bounded BOOL) — `test_error_infeasible.py::TestUnboundedModels::test_mixed_unbounded_integer_var`

Aggregate LHS versus aggregate RHS (`SUM(x*v) <= SUM(y*v)`) is supported and
oracle-verified in `test_canonicalize_side_agnostic.py::test_aggregate_vs_aggregate`.

### Data shapes

All oracle-verified, in `test_when_constraint.py`, `test_per_clause.py`,
`test_per_objective.py`, and `test_per_multi_column.py`: NULL in a `when`
condition column (the row is dropped), NULL in a `per` key (a class of its own),
a single `per` class, a `when` keeping every row, a `when` that leaves a `per`
class without rows (that class generates nothing), mixed conditional +
unconditional constraints, a different `when` on each constraint, several `per`
constraints on different columns.

### Empty row sets

`test_edge_cases.py` pins the two empty-set errors, each with its exact message
(see also `../language/scopes.md`):

- A `when` in front that keeps no row is "when c matches no rows." (D4): on
  constraints over an easy or hard `min`/`max`, a `sum` and an `avg`
  (`test_min_geq_constraint_when_empty` and its three mirrors,
  `test_sum_when_empty_rejected`, `test_avg_when_empty_rejected`), and on a flat
  MIN/MAX objective in both directions (`test_maximize_min_objective_when_empty` and
  its three mirrors).
- A reducer whose own `when` keeps no row is "agg(when c: e) has no rows." (D8): in
  hard constraints (`test_max_when_empty_constraint_hard`,
  `test_min_when_empty_constraint_hard`), beside a populated reducer
  (`test_sum_plus_max_when_empty_silently_vacates_constraint`,
  `test_composed_easy_min_when_empty_rejected`,
  `test_mixed_empty_and_populated_when_terms_constraint` and `..._objective`), and
  for AVG (`test_avg_constraint_when_filters_all_rows`).
- A `when` in front that leaves one `per` class without rows generates nothing for
  that class (`test_avg_per_constraint_with_empty_group`,
  `test_maximize_sum_max_per_with_empty_when_group`), while a reducer's own `when`
  that empties a group is "avg(when flag: x * val) has no rows for grp = 'B'."
  (`test_avg_per_constraint_inner_when_with_empty_group_rejected`).

### JOIN sources

All oracle-verified: 2-table and 3-table JOINs (`test_sql_joins.py`),
JOIN + keyed decisions (many tests in `test_entity_scope.py`).

### Scale / performance

`test_large_scale.py` holds a 501-row knapsack and a 2204-row order selection, each judged by the oracle. The
largest scale case is the 1500-customer problem in `test_entity_scope.py::test_entity_scoped_mixed_when_per`
(Gurobi-only).

### Solver coverage

The oracle always picks Gurobi (gurobipy required; oracle fixtures skip if
unavailable). The `_expect_gurobi` decorator in QP/bilinear tests accepts the
rejection message on HiGHS-only hosts. The `DECIDB_FORCE_SOLVER` env var pins
DeciDB's backend for specific tests via the `decidb_cli_highs` and
`decidb_cli_gurobi` fixtures.

| Scenario | Where | Oracle |
|----------|-------|--------|
| HiGHS rejects non-convex QP (MAXIMIZE SUM(POWER)) | `test_quadratic.py::TestHighsRejection::test_highs_nonconvex_qp_rejected` | error test |
| HiGHS rejects MIQP (integer vars + quadratic obj) | `test_quadratic.py::TestHighsRejection::test_highs_miqp_rejected` | error test |
| Gurobi ↔ HiGHS objective agreement on linear IP | `test_edge_cases.py::test_gurobi_highs_agree_on_objective` | cross-check |
| Infeasible QP constraint (tight `match=`) | `test_quadratic_constraints.py::test_infeasible_negative_budget` | error test |

### Infrastructure / meta-tests

EXPLAIN output format is covered in `test_explain.py`.
