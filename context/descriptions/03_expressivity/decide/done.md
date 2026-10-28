# DECIDE Clause — Implemented Features

The `DECIDE` clause declares **decision variables** of a COP query. Each variable gets a value assigned by the solver, and the assigned values appear as new columns in the query result.

**Syntax** (variable types, multiple variables, keyed and query-wide declarations, examples): see `../../00_project_overview/syntax_reference.md` §1–§2. The meaning of `per` on a declaration is in [`../scope/done.md`](../scope/done.md) §2. This doc covers semantics and implementation only.

---

## Semantics beyond the syntax spec

- **Two clause orders.** The declaration may sit between `SELECT` and `FROM` (the
  paper's order) or inside the single block after `WHERE`. The parser has two
  optional slots — `decide_declaration` before `from_clause`, `decide_body` after
  `where_clause` — and `makeDecideClause()` (`third_party/libpg_query/grammar/grammar.cpp`) reassembles
  them into the one `PGDecideClause` the transform already consumed, so nothing
  downstream of the parser distinguishes the orders. Declaring in both slots, a
  declaration with no `SUCH THAT`, and a `SUCH THAT` with no declaration are all
  parser errors. The `in_decide_clause` lexer flag is cleared when the
  declaration slot reduces and re-armed on the `SUCH` token, so `CASE WHEN` in a
  `JOIN ... ON` or `WHERE` between the two slots still lexes as ordinary SQL.
- **The type is mandatory.** `x(INT)`, `x(BOOL)`, `x(REAL)` — there is no
  implicit default and no `IS` form. A declaration with no type and the `x IS INTEGER`
  form each keep a grammar production whose only job is to raise a message naming the
  fix (`DECIDE variable "x" needs a type; write x(INT), x(BOOL) or x(REAL)`).
- **`REAL`** is stored internally as `LogicalType::DOUBLE`; both HiGHS (`kContinuous`) and Gurobi (`GRB_CONTINUOUS`) support it natively. REAL enables value-assignment problems (imputation, repair, synthesis) as opposed to selection (BOOL) or counting (INT), and is a prerequisite for ABS() linearization.
- **Signed variables (negative domains).** The default lower bound for `INT`/`REAL` is 0, but it is a *default*, not a floor. A variable becomes signed when the query bounds it alone below zero, read by value rather than by spelling — `x >= -K` and its affine and scaled spellings (`x - 1 >= -K - 1`, `2 * x >= -2K`, `-x <= K`), `x BETWEEN -K AND K`, a negative literal in an `IN` domain, or an easy `min(x) >= -K` in the same spellings with no `when` and no reducer filter, each absorbed into the box like `x >= -K` (`../scope/done.md` §3; `../../01_pipeline/05_optimizer/done.md`, "Bound absorption"). A bound whose other side is data stays a row and does not lower the floor. Mechanism: bound absorption initializes lower bounds to an "unset" sentinel (`ABSORBED_LOWER_UNSET` in `physical_decide.cpp`) so the `std::max` combiner keeps the tightest of multiple `>=` constraints for negatives too; `Finalize` resolves any still-unset variable to 0. The model builder takes the resolved lower bound directly instead of re-clamping it to the type default. Only finite negative bounds are supported — there is no fully-free ($-\infty$) domain (deliberately: a two-sided-unbounded variable is the case most likely to make a model unbounded). **Limitation:** implied-bound propagation does not fire for a variable whose lower bound is negative (it is sound to skip — see `../../01_pipeline/05_optimizer/done.md`), so a signed variable used in a **bilinear** product needs an *explicit* finite upper bound (`x <= K`); it cannot rely on a `SUM(x) <= K` constraint to infer one. Tests: `test/decide/tests/test_signed_variables.py`.
- **Variable scope**: declared variables are available in `SUCH THAT`, `MAXIMIZE`/`MINIMIZE`, and the `SELECT` list (returned as output columns).
- **Three scopes.** `DecideVarScope` (`common/enums/decide.hpp`) is `ROW` (`x(INT)` or
  `per row: x(INT)`, one column per result row), `ENTITY` (`per K: x(INT)`, one per class
  of the key `K`; the language calls this *keyed*) or `SCALAR` (`per (): x(INT)`, one for
  the whole query). Each declaration carries a `DecideVarScopeInfo` — the scope plus, for a
  keyed one, the index into `entity_scopes` and the key as written (`declared_key`), which
  messages quote (*"declared per D.depotID"*). It is one struct rather than parallel
  arrays so the optimizer's auxiliary-variable appends cannot desync the scope from its
  key scope. The chain is `decide_declarations_binder.cpp` → `BoundSelectNode` →
  `LogicalDecide` → `PhysicalDecide` → `SolverInput`, all under the field name
  `variable_scopes`.
- **Keyed variables — `per K: x(TYPE)`.** `BindDeclarations` resolves the key through
  `FindOrCreateKeyScope` (`decide_binder.cpp`): its elements are columns and relations
  of the `FROM` clause (a relation stands for its stored columns), the key is a sorted
  set of bindings, and declarations and reducers with equal keys share one
  `EntityScopeInfo`. The key is exactly what is written. During physical execution
  (Phase 1.5, `BuildEntityMappings`) the executor reads each row's key columns and maps
  each distinct key — NULL included, as one value — to one solver column, so every row of
  a class shows the same value. Each declarator has its own key; a decision is
  referenced by its bare name. Semantics and errors: `../scope/done.md` §2;
  `../../01_pipeline/02_binder/done.md` → "Keyed declarations".
- **Query-wide variables — `per (): x(TYPE)`** yield exactly one solver column regardless
  of input cardinality (paper §3.1). The declaration carries no key scope:
  `DecideVarScopeInfo::Scalar()`.
  - **Column layout**: `VarIndexer` is four blocks — `[row | keyed | query-wide | global auxiliary]`. Query-wide columns sit *below* `global_block_start` so the "user decide-variable columns occupy `[0, global_block_start)`" invariant that the model builder and the column labeller rely on keeps holding. `Get()` ignores the row for a query-wide variable and `NumInstances()` returns 1; `InstanceColumn(var, inst)` walks a variable's own instances.
  - **Constraints**: a query-wide decision may be compared against row-varying data (`ship <= cap`). The constraint fans out per row, but every generated row references the same column — that is what makes the decision shared. The per-row fast path in the model builder (`CanUseRowScopedFastPath`) is skipped for it for the same reason it is skipped for keyed variables (many rows, one column).
  - **As a term beside a reducer**: a query-wide decision is row-invariant, so it is a complete term of an aggregate constraint on its own — `sum(x) - cap <= 4`, the paper's `max_shortfall` shape (§3.1) — or its bound (`sum(x) <= cap`, `per g: demand - sum(x) by (g) <= cap`, one row per variant of `demand` in a class); canonicalization moves it to the left either way. The model-builder accumulators (ungrouped and grouped) add its coefficient **once per model row**, not once per member row, matching the objective path. A per-row or keyed decision beside a reducer (`sum(x) <= y`) takes the value shape instead: the reducer becomes a helper decision and the clause is one row per class (`../scope/done.md` §3). Tests: `test/decide/tests/test_scalar_var_in_aggregate.py`, `test_scalar_scope.py`, `test_canonicalize_side_agnostic.py`, golden corpus 55/56 and 75/76/77.
  - **Objectives**: a query-wide decision contributes **bare**, not through a reducer — alone (`MINIMIZE cap`) or as an additive term beside reducers (`MINIMIZE cap - sum(x)`). `AnalyzeObjective` (`src/optimizer/decide/decide_linear_form.cpp`) recognises a purely query-wide objective (which carries no aggregate at all), and `ExtractAggregateObjectiveTerms` accepts a query-wide leaf. The model builder adds the coefficient **once** instead of once per row. Beside a `MIN` or `MAX` it is refused (*"DECIDE: 'cap' beside MIN or MAX is not supported yet"*).
  - **A reducer is rejected iff its body is row-invariant** on the decision side: `sum(cap)` / `avg(cap)` / `sum(cap1 + cap2)` raise a binder error naming the fix (*"cap is a query-wide decision, so SUM(cap) has nothing to aggregate over; use cap on its own"*). There is one column and nothing to reduce over, and the two plausible readings (coefficient 1 vs. coefficient `n`) are different optimization problems — so neither is chosen silently. A body that *mixes* a query-wide decision with row-varying data is an ordinary scalar-times-vector reduction: `sum(cost * cap)` means `(Σ cost)·cap`, and `sum(x + cap)` means `sum(x) + n·cap` over the counted rows. Enforced once, in `DecideBinder::ClassifyReducerCall` via `DecideBinder::IsRowInvariantExpression` (`decide_binder.cpp`), which knows the query-wide names because `BindDeclarations` passes them; the objective and constraint binders inherit it. The weighting the accepted case implies is carried to stage 06 by `DecideTerm::reduction` — see `../sql_functions/done.md` → "A Query-Wide Decision Multiplied by a Vector, Inside a Reducer".
  - **Output**: the value is repeated on every result row (paper §3.1), which falls out of `Get()` resolving every row to the same column.
  - **Diagnostics**: a query-wide decision has a single instance, so an unbounded report carries no escape count and no `scope`, only `runaway_+inf` / `runaway_-inf`; the candidate provider returns no grouping candidates for it.
  - Tests: `test/decide/tests/test_scalar_scope.py`, `test_scope_declare.py`.
- **Reducers count result rows**: SUM/AVG aggregate over result rows, not classes — if a
  keyed decision's class appears in 5 join-result rows, its shared variable contributes 5
  times (standard SQL aggregation over the join result). A reducer's own key —
  `sum(per D: e)` or the colon form below — opts out of that per reducer.

## The colon form — `SUM(D: e)`, `SUM(D, T: e)`

`SUM(D: e)` reduces over `D`'s tuple identities instead of over join-result rows,
contributing one term per surviving `D` tuple (paper §3.2.2). It means `sum(per D: e)`:
the same key scope, the same checks, the same answer, and it may take a `by`. Syntax:
`../../00_project_overview/syntax_reference.md` §5.1; a reducer's own `per`:
`../scope/done.md` §4. A reducer with no key of its own counts result rows, so the key
is opt-in.

- **One key scope for `SUM(D: e)` and `sum(per D: e)`.** The key is the listed relations' stored columns,
  found or created as the exact key scope `per D` uses (`FindOrCreateExactScope`,
  `decide_binder.cpp`), so the two share one `entity_scopes` entry, one set of
  pinned key columns and one `EntityMapping`. A body that reads a value `D` does not fix
  — a column of another relation, a per-row decision, a decision keyed elsewhere — makes
  each tuple one value per variant (D65): the canonicalizer gives that reducer a scope of
  its own (`EntityScopeInfo::signature`, `../scope/done.md` §5) whose entities are the
  variants. A scope with no decision declared on it has empty `scoped_variable_indices`.
- **De-duplication is a row mask, not a new pipeline stage.** `BuildQualifierKeepMask`
  (`physical_decide.cpp`, Phase 2) keeps the first row of each class within each group,
  among the rows the reducer reads (its own `when` included), and drops the rest;
  `ApplyQualifierToFilter` ANDs that into the term's `TermFilterState`, the slot a
  reducer's own `when` uses. Consequences that fall out rather than being coded: `AVG`'s
  denominator counts surviving rows and so becomes the distinct-identity count; the
  empty-group check and coefficient zeroing already respect the mask. Soundness rests on
  the identity being the variant: every row of one carries the same value, so *which*
  row survives cannot matter.
- **The mask is per term, not per clause**, so `SUM(D: a * open) + sum(b * ship)` in one
  objective de-duplicates only the first term.
- **A filter on the colon form** is written inside the reducer: `sum(when c per D: e)`.
- **Composed MIN/MAX carries the key too.** `SUM(D: a * open) + max(b * ship)` routes
  through the composed path, which keeps its own term struct
  (`ComposedMinMaxTerm::qualifier_scope_idx` on `LogicalDecide`, stamped by the optimizer
  from the same tag) and ANDs the same mask into that term's filter mask. Objective and
  constraint are separate code paths and both do it. Applied uniformly to every term kind
  rather than skipped for `MIN`/`MAX`: it is provably a no-op there, so one code path
  beats a special case.
- **`MIN`/`MAX` need no de-duplication.** Dropping repeats of a value already present
  cannot move an extremum. The key is accepted, carried, and has no effect for them.
- **Several relations at once — `SUM(D, T: e)`.** The tuple identity is the
  concatenation of each named relation's own key, so a row is a duplicate only when it
  repeats on every named key at once. Naming a relation removes the fan-out it would
  otherwise contribute; a relation left unnamed still contributes its fan-out
  uncollapsed. The body may draw columns and decisions from any of the named relations.
  The key is a set of columns sorted by binding, so `SUM(D, T: ...)`, `SUM(T, D: ...)` and
  `sum(per D, T: ...)` share one scope and one `EntityMapping`. Naming *every* relation
  the query joins is a no-op, since the composite key already is the join-result row; the
  forms diverge only once some joined relation is left out of the list. Below the binder
  — the optimizer's `qualifier_scope_idx` stamping, the prepared model's term structs and
  `BuildQualifierKeepMask` / `BuildEntityMappings` — a scope is an opaque index into
  `entity_scopes`. Details: `../../01_pipeline/02_binder/done.md` → "Relation-qualified
  reducers", `../sql_functions/done.md` → "Several Relations at Once — A Composite
  Qualifier".
- **A query-wide (`per ()`) decision is allowed inside**, matching paper §3.2.2's
  carve-out: it is row-invariant, so it contributes the same value to every tuple
  regardless of which relation owns it (`SUM(D: opening_cost * cap)`). It is refused only
  when it is the *whole* body on the decision side (`SUM(D: cap)`), by the row-invariance
  rule every reducer takes (`ClassifyReducerCall`).
- **EXPLAIN prints the colon form as written** (D38). `RenderReducer` prints `sum(D: e)`
  for an aggregate tagged `COLON_FORM_TAG` and `sum(per D: e)` otherwise; a shared scope
  keeps the key text of the spelling bound first.
- **What `D` does not fix is a variant**, as for `sum(per D: e)` (D65): a column of
  another relation, a per-row decision (a different variable on every joined row, so
  each row is its own value), a keyed decision whose key `D` does not hold. That is the
  trap the colon form now carries: `SUM(D: opening_cost + unit_cost * ship)` counts a
  depot once per route. `EXPLAIN ANALYZE` prints how many values it read from how many
  tuples.
- **Refused at bind time** (`BindQualifiedReducer`): a qualifier that is not a relation
  name, an unknown relation name, a qualifier on a non-`SUM`/`AVG`/`MIN`/`MAX` function
  (*"DECIDE: norm(D: ...) is not supported yet; a relation qualifier takes SUM, AVG, MIN
  or MAX"*), and a body of query-wide decisions alone.
- Tests: `test/decide/tests/test_qualified_reducer.py`, `test_scope_reducer_inner.py`.

### Code Pointers — the colon form

- **Grammar**: `third_party/libpg_query/grammar/statements/select.y`, `c_expr` alternatives
  `func_name '(' func_arg_list ':' func_arg_list ')' [decide_by]` (`makeQualifiedReducer`).
  The qualifier is parsed as an argument list, not a dedicated name list: `sum(D, T: e)`
  and `sum(a, b)` share a prefix that LALR(1) cannot separate at the comma, so both sides
  are read as argument lists and the qualifier's shape is checked in the C action. The
  decision point becomes `:` vs `)` after a completed `func_arg_list`, which is
  conflict-free. A scope inside a reducer (`func_name '(' decide_scope ':' ...`) starts
  with `when` or `per`, so the two never compete.
- **Parse node**: `PG_AEXPR_QUALIFIED_REDUCER` (`parsenodes.hpp`) →
  `QUALIFIED_REDUCER_TAG` FunctionExpression (`transform_operator.cpp`), shaped
  `tag(aggregate, relation_name...)`.
- **Parsed-tree rewriting**: none. The reducer reaches the binder exactly as written.
  `DecideCanonicalizer` needs no protection for the tag either: it sits on a term, and the
  pass never opens a term.
- **Binder**: `DecideBinder::BindQualifiedReducer` resolves the relations to an exact key
  scope and appends `MakeQualifiedReducerTag(scope_idx)` and `COLON_FORM_TAG` to the bound
  aggregate's `alias`; `BindScopedReducer` appends the same scope tag for
  `sum(per D: e)`. `DecideQualifierContext` (`decide_binder.hpp`) carries what they need
  from the declarations binder, which is still filling `entity_scopes` at that point.
- **Alias tags are composable** (`common/enums/decide.hpp`): `AddDecideTag`,
  `HasDecideTag`, `ExtractDecideTagPayload`. A keyed `AVG` is tagged by the binder and
  again by the AVG→SUM rewrite, so tags concatenate and are matched by search rather than
  equality.
- **Term plumbing**: `qualifier_scope_idx` on `DecideTerm`, `BilinearConstraintTerm`,
  `QuadraticGroup` and the objective's `BilinearTerm` (`decide_prepared_model.hpp`),
  stamped by `ApplyAggregateMetadata` / `QualifierScopeOf` (`decide_linear_form.cpp`).

## Linearity / Non-Linearity

Linear expressions are always supported; bilinear (`x * y`) and quadratic (`POWER(x, 2)`) terms are supported via dedicated rewrites/solver paths — see `../bilinear/done.md` and `syntax_reference.md` §4. Triple and higher products (`x * y * z`) are rejected by the binder (`decide_binder.cpp`).

## Use Cases by Variable Type

| Task Category | Typical Variable | Type |
|---|---|---|
| Subset selection (knapsack, sampling), outlier removal, counterfactuals | `keep` | `BOOL` |
| Scheduling / assignment | `hours_assigned` | `INT` |
| Data imputation / repair / synthesis | `imputed_distance` | `REAL` |

---

## Code Pointers

- **Grammar**: `third_party/libpg_query/grammar/statements/select.y`
  - `variable_type: INT_P | REAL | BOOL_P`  (the type is mandatory)
  - `typed_decide_variable: ColId '(' variable_type ')' | decide_per ':' ColId '(' variable_type ')'` — the keyed form wraps the name in the `SCOPE_PER_TAG` marker (`applyDecideScope`)
  - Further alternatives exist only to raise a message naming the fix: a declaration with no type (`x`, `per K: x`), the `x IS INTEGER` form, and the declaration spellings the language refuses (`syntax_reference.md` §10.3)
  - `typed_decide_variable_list: typed_decide_variable | list ',' typed_decide_variable`
- **Binder** (variable processing loop, scopes, type mapping): `DecideDeclarationsBinder::BindDeclarations` in `src/planner/expression_binder/decide/decide_declarations_binder.cpp` (the DECIDE section of `bind_select_node.cpp` only calls it)
  - REAL → `LogicalType::DOUBLE`, INT → `LogicalType::BIGINT`, BOOL → `LogicalType::INTEGER` (the DuckDB-facing type — INT is 64-bit because nothing bounds a decision to int32; BOOL stays INTEGER so the variable can appear in arithmetic like `x * weight`, which BOOLEAN cannot implicitly cast into)
  - Boolean type detected via `type_marker == "bool_variable"` and recorded in `is_boolean_var[i]` — the domain signal, independent of the DuckDB type above
  - `is_boolean_var` carries the `[0,1]` domain from here through `LogicalDecide` to `PhysicalDecide`; nothing is synthesized into the `SUCH THAT` tree for it (see below)
- **ILP model builder** (variable type handling): `src/decidb/formulation/ilp_model_builder.cpp`
  - DOUBLE/FLOAT → `is_integer = false`, type-default bounds `[0, 1e30]`
  - `LogicalType::BOOLEAN` → `is_binary = true`, bounds `[0, 1]`
  - INT → `is_integer = true`, type-default bounds `[0, 1e30]`
  - Lower bound: the input's resolved lower bound (from `physical_decide.cpp`) is taken **directly**, not `std::max`-ed with the type default — otherwise an explicit negative bound would be clamped back to 0. Upper bound is still intersected (`std::min`) with the type default. See "Signed variables" above.
  - A safety-net downgrade runs after the bound merge: if a variable would be `is_binary` but its merged bounds fall outside `[0,1]` (an unusual explicit user pin, e.g. `x >= -1` on a declared BOOL), it is reported as a plain bounded integer instead of a contradictory (binary, out-of-range) column.
- **Every BOOLEAN-domain variable's `[0,1]` box is applied directly to the solver column, never synthesized as a constraint.** This covers user-declared `x(BOOL)` (DuckDB type INTEGER) exactly like it covers optimizer-created auxiliaries with a real `LogicalType::BOOLEAN` type (MIN/MAX indicators, NE indicators) and INTEGER-typed auxiliaries that need to participate in arithmetic (IN-domain, L0 indicators): `PhysicalDecide::Finalize` (`physical_decide.cpp`) reports `LogicalType::BOOLEAN` to `SolverInput::variable_types` for any variable where `is_boolean_var[var]` is true, regardless of its DuckDB-facing type, and the ILP model builder's type-driven bounds above take it from there. `is_boolean_var` is the single authoritative domain signal end to end; `TraverseBoundsConstraints` in `physical_decide.cpp` only ever absorbs genuine user-written bound constraints (`x <= 10`, `BETWEEN`, an explicit BOOLEAN pin like `x = 1`), never the domain itself.
- **Solver backends**: HiGHS `!is_integer → kContinuous` (`deterministic_naive.cpp`); Gurobi `!is_integer && !is_binary → GRB_CONTINUOUS` (`gurobi_solver.cpp`)
- **Physical execution** (DOUBLE output path): `physical_decide.cpp` — returns raw `double` solution values for REAL vars
- **Keyed variables**:
  - `EntityScopeInfo` struct: `src/include/duckdb/planner/operator/decide/logical_decide.hpp` — the key as written (`table_alias`), its column bindings and types, the relations it reads (`source_table_indices`), the decisions declared on it (`scoped_variable_indices`)
  - `FindOrCreateKeyScope` / `FindOrCreateExactScope`: `src/planner/expression_binder/decide/decide_binder.cpp` — resolve a key and share one scope per key
  - `VarIndexer`: `src/include/duckdb/decidb/formulation/ilp_model.hpp` — maps class ids to solver variable indices, de-duplicating across result rows
  - Class mapping (Phase 1.5, `BuildEntityMappings`): `src/execution/operator/decide/physical_decide.cpp`
  - Physical index resolution: `src/execution/physical_plan/plan_decide.cpp`
