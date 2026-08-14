# DECIDE Clause — Implemented Features

The `DECIDE` clause declares **decision variables** of a COP query. Each variable gets a value assigned by the solver, and the assigned values appear as new columns in the query result.

**Syntax** (variable types, multiple variables, table-scoped form, examples): see `../../00_project_overview/syntax_reference.md` §1–§2. This doc covers semantics and implementation only.

---

## Semantics beyond the syntax spec

- **Two clause orders.** The declaration may sit between `SELECT` and `FROM` (the
  paper's order) or inside the single block after `WHERE`. The parser has two
  optional slots — `decide_declaration` before `from_clause`, `decide_body` after
  `where_clause` — and `makeDecideClause()` (`grammar/grammar.cpp`) reassembles
  them into the one `PGDecideClause` the transform already consumed, so nothing
  downstream of the parser distinguishes the orders. Declaring in both slots, a
  declaration with no `SUCH THAT`, and a `SUCH THAT` with no declaration are all
  parser errors. The `in_decide_clause` lexer flag is cleared when the
  declaration slot reduces and re-armed on the `SUCH` token, so `CASE WHEN` in a
  `JOIN ... ON` or `WHERE` between the two slots still lexes as ordinary SQL.
- **The type is mandatory.** `x(INT)`, `x(BOOL)`, `x(REAL)` — there is no
  implicit default and no `IS` form. Both removed spellings still have grammar
  productions whose only job is to raise a message naming the fix.
- **`REAL`** is stored internally as `LogicalType::DOUBLE`; both HiGHS (`kContinuous`) and Gurobi (`GRB_CONTINUOUS`) support it natively. REAL enables value-assignment problems (imputation, repair, synthesis) as opposed to selection (BOOL) or counting (INT), and is a prerequisite for ABS() linearization.
- **Signed variables (negative domains).** The default lower bound for `INT`/`REAL` is 0, but it is a *default*, not a floor. A variable becomes signed when the query gives it an explicit negative lower bound — `x >= -K`, `x BETWEEN -K AND K`, or a negative literal in an `IN` domain. Mechanism: bound absorption initializes lower bounds to an "unset" sentinel (`ABSORBED_LOWER_UNSET` in `physical_decide.cpp`) so the `std::max` combiner keeps the tightest of multiple `>=` constraints for negatives too; `Finalize` resolves any still-unset variable to 0. The model builder takes the resolved lower bound directly instead of re-clamping it to the type default. Only finite negative bounds are supported — there is no fully-free ($-\infty$) domain (deliberately: a two-sided-unbounded variable is the case most likely to make a model unbounded). **Limitation:** implied-bound propagation does not fire for a variable whose lower bound is negative (it is sound to skip — see `04_optimizer`), so a signed variable used in a **bilinear** product needs an *explicit* finite upper bound (`x <= K`); it cannot rely on a `SUM(x) <= K` constraint to infer one. Tests: `test/decide/tests/test_signed_variables.py`.
- **Variable scope**: declared variables are available in `SUCH THAT`, `MAXIMIZE`/`MINIMIZE`, and the `SELECT` list (returned as output columns).
- **Three scopes.** `DecideVarScope` (`common/enums/decide.hpp`) is `ROW` (`x(INT)`, one column per result row), `ENTITY` (`T.x(INT)`, one per distinct entity) or `SCALAR` (`scalar x(INT)`, one for the whole query). Each declaration carries a `DecideVarScopeInfo` — the scope plus, for `ENTITY`, the index into `entity_scopes`. It is one struct rather than parallel arrays so the optimizer's auxiliary-variable appends cannot desync the scope from its entity index. The chain is `bind_select_node.cpp` → `BoundSelectNode` → `LogicalDecide` → `PhysicalDecide` → `SolverInput`, all under the field name `variable_scopes`.
- **Query-wide (`scalar`) variables**: `DECIDE scalar name(TYPE)` yields exactly one solver column regardless of input cardinality (paper §3.1). The grammar marks the declaration by prefixing the private type marker (`scalar_integer_variable`, see `scalar_variable_type` in `select.y`); the binder strips the prefix and records `DecideVarScope::SCALAR`, so the type comparisons stay scope-agnostic. `scalar T.x(TYPE)` is a parse error. `SCALAR` is an unreserved keyword and adds no grammar conflicts.
  - **Column layout**: `VarIndexer` is now four blocks — `[row | entity | scalar | global auxiliary]`. Scalars sit *below* `global_block_start` so the "user decide-variable columns occupy `[0, global_block_start)`" invariant that the model builder and the column labeller rely on keeps holding. `Get()` ignores the row for a scalar and `NumInstances()` returns 1; `InstanceColumn(var, inst)` walks a variable's own instances and replaces the block math that was duplicated at the bounds and provenance sites.
  - **Constraints**: a scalar may be compared against row-varying data (`ship <= cap`). The constraint fans out per row, but every generated row references the same column — that is what makes the decision shared. The row-scoped fast path in the model builder is skipped for scalars for the same reason it is skipped for entity-scoped variables (many rows, one column).
  - **As a term of an aggregate constraint**: a scalar is row-invariant, so it is a complete term of an aggregate constraint on its own — `SUM(x) - cap <= 4`, the paper's `max_shortfall` shape (§3.1). This mirrors the objective rule below and is canonical form's K3 ("every term is a reducer **or row-invariant**"). Two things had to agree for it to be right, and they fail differently: `ExtractAggregateConstraintTerms` used to *reject* the term outright, while both model-builder accumulators (ungrouped and `PER`) fan every term out over the group's rows, which for a one-column variable silently multiplies its coefficient by the row count. The accumulators now add a scalar's coefficient **once per constraint row**, matching the objective path. Since the canonicalization refactor the scalar may equally be written as the **bound** — `SUM(x) <= cap`, `demand - SUM(x) <= cap PER g` — which is the paper's Example 1 spelling; canonicalization moves it to the left, so the shape reaching the extractor is the same one. A **row-scoped** decision as the bound is the other half of K3 and is rejected there (*"non-aggregate term: y"*), because there is no single `y` for a number that has no row. Tests: `test/decide/tests/test_scalar_var_in_aggregate.py`, `test_scalar_scope.py::test_scalar_as_aggregate_rhs[_with_per]`, `test_canonicalize_side_agnostic.py`, golden corpus 55/56 and 75/76/77.
  - **Objectives**: a scalar contributes **bare**, not through a reducer — alone (`MINIMIZE cap`) or as an additive term beside reducers (`MINIMIZE cap - SUM(x)`). `AnalyzeObjective` recognises a purely-scalar objective (which carries no aggregate at all), and `ExtractAggregateObjectiveTerms` accepts a scalar leaf. The model builder adds the coefficient **once** instead of once per row; without that the coefficient would be silently multiplied by input cardinality.
  - **Reducers are rejected**: `SUM(cap)` / `AVG(cap)` / `SUM(x + cap)` raise a binder error naming the variable and the fix. There is one column, so nothing to reduce over, and the two plausible readings (coefficient 1 vs. coefficient `n`) are different optimization problems — so neither is chosen silently. Enforced in both `decide_objective_binder.cpp` and `decide_constraints_binder.cpp` via `DecideBinder::FindScalarDecideVariable`, which the binders can answer because `bind_select_node.cpp` passes them the set of `scalar`-declared names.
  - **Output**: the value is repeated on every result row (paper §3.1), which falls out of `Get()` resolving every row to the same column.
  - **Diagnostics**: a scalar has a single instance, so `FormatEscapingInstances` already suppresses the `affected_rows`/`affected_entities` cell and the unbounded report carries only `grows_toward`. The candidate provider returns no grouping candidates for a scalar — there is no subset of rows to characterize.
  - Tests: `test/decide/tests/test_scalar_scope.py`.
- **Table-scoped entity identification**: all columns from the source table form a composite key. During physical execution (Phase 1.5), the executor scans result rows, extracts the source-table columns for each scoped variable, and maps each distinct entity key to a single solver variable index — the **entity consistency guarantee**. There is no syntax for a custom key subset.
- **Aggregate semantics with table scope**: SUM/AVG aggregate over result rows, not entities — if an entity appears in 5 join-result rows, its shared variable contributes 5 times (standard SQL aggregation over the join result). A **relation-qualified reducer** opts out of that per reducer; see below.

## Relation-qualified reducers — `agg(D: expr)`

`SUM(D: expr)` reduces over `D`'s tuple identities instead of over join-result rows,
contributing one term per surviving `D` tuple (paper §3.2.2). Syntax and semantics:
`../../00_project_overview/syntax_reference.md` §5.1. Unqualified reducers are unchanged,
so this is opt-in and nothing existing changes meaning.

- **A qualifier is an entity scope with no variable.** The tuple-identity key a qualifier
  needs is exactly the key a `T.x(TYPE)` declaration needs, so both go through
  `FindOrCreateEntityScope` (`decide_binder.cpp`) and share one `entity_scopes` entry, one
  set of pinned key columns, and one `EntityMapping`. Qualifying by a relation that already
  has an entity-scoped declaration reuses its scope; qualifying by one that does not
  appends a key-only scope whose `scoped_variable_indices` is empty.
- **De-duplication is a row mask, not a new pipeline stage.** `BuildQualifierKeepMask`
  (`physical_decide.cpp`, Phase 2) keeps the first row of each `(PER group, entity id)` pair
  and drops the rest; `ApplyQualifierToFilter` ANDs that into the term's existing
  `TermFilterState`, the same slot aggregate-local `WHEN` uses. Consequences that fall out
  rather than being coded: `AVG`'s denominator counts surviving rows and so becomes the
  distinct-identity count; the empty-aggregate guard and coefficient zeroing already
  respect the mask; the paper's construction order (`when` → `per` → qualifier grouping →
  aggregation) is just where the mask is applied — after `row_group_ids` is settled.
  Soundness rests on the well-formedness rule: every row of an identity carries the same
  value, so *which* row survives cannot matter.
- **The mask is per term, not per clause**, so `SUM(D: a * open) + SUM(b * ship)` in one
  objective de-duplicates only the first term.
- **Aggregate-local WHEN on a qualified reducer works on both sides, regardless of
  WHEN/comparison order**: `SUM(D: expr) WHEN cond <= bound` and
  `SUM(D: expr) <= bound WHEN cond` both bind and execute in constraints, matching the
  objective side. This needed a grammar production, not a binder change — the binder's
  `ContainsDecideAggregate`/`GetExpressionType` already unwrap `WHEN_CONSTRAINT` around a
  qualified reducer correctly (proven by the objective side, which never had a trailing
  comparison to lose). The grammar had no route to *produce* that shape for the
  WHEN-before-comparison order: a qualified reducer is not a `func_application`, so it
  couldn't take the `func_application WHEN_DECIDE decide_when_condition` production
  aggregate-local WHEN already used, and fell instead to the loose, whole-constraint
  `a_expr WHEN_DECIDE b_expr` production, which swallowed a trailing `<= bound` into the
  WHEN condition. Fixed by adding a qualified-reducer-specific mirror of that production
  (see Code Pointers below).
- **Composed MIN/MAX carries the qualifier too.** `SUM(D: a * open) + MAX(b * ship)` routes
  through the composed path, which keeps its own term struct
  (`ComposedMinMaxTerm::qualifier_scope_idx`, stamped by the optimizer from the same tag)
  and ANDs the same mask into that term's filter mask. Objective and constraint are separate
  code paths and both do it. Applied uniformly to every term kind rather than skipped for
  `MIN`/`MAX`: it is provably a no-op there, so one code path beats a special case.
- **`MIN`/`MAX` need no de-duplication.** Dropping repeats of a value already present
  cannot move an extremum. The qualifier is accepted, carried, and has no effect for them.
- **Rejections** (all at bind time, in `BindQualifiedReducer` /
  `CheckQualifiedReducerBody`): a column or entity-scoped decision from another relation, a
  row-scoped decision, a query-wide (`scalar`) decision, an unknown relation name, a
  qualifier on a non-`SUM`/`AVG`/`MIN`/`MAX` function. Multi-relation qualifiers
  (`SUM(D,T: ...)`) are rejected in the grammar action.
- Tests: `test/decide/tests/test_qualified_reducer.py`.

### Code Pointers — qualified reducers

- **Grammar**: `third_party/libpg_query/grammar/statements/select.y`, `c_expr` alternative
  `func_name '(' func_arg_list ':' func_arg_list ')'`. The qualifier is parsed as an
  argument list, not a dedicated name list: `sum(D, T: e)` and `sum(a, b)` share a prefix
  that LALR(1) cannot separate at the comma, so both sides are read as argument lists and
  the qualifier's shape is checked in the C action. The decision point becomes `:` vs `)`
  after a completed `func_arg_list`, which is conflict-free — `%expect` stayed at 8.
  A second alternative, `func_name '(' func_arg_list ':' func_arg_list ')' WHEN_DECIDE
  decide_when_condition`, mirrors `func_application WHEN_DECIDE decide_when_condition` so
  a qualified reducer followed directly by `WHEN` also takes the tight aggregate-local-WHEN
  route instead of the loose whole-constraint one; the qualifier validation is duplicated
  inline rather than factored into a shared helper (bison actions can't call each other and
  this file has no C prologue to hang one on — the file already accepts this style of
  duplication elsewhere). This added one more shift/reduce conflict of the same shape as
  the existing `func_application`/`WHEN_DECIDE` one, so `%expect` moved to 9.
- **Parse node**: `PG_AEXPR_QUALIFIED_REDUCER` (`parsenodes.hpp`) →
  `QUALIFIED_REDUCER_TAG` FunctionExpression (`transform_operator.cpp`), shaped
  `tag(aggregate, relation_name)` exactly like the aggregate-local WHEN tag.
- **Parsed-tree rewriting**: none. A qualified reducer reaches the binder exactly as
  written; nothing rewrites a constraint before binding. `DecideCanonicalizer` needs no
  protection for the tag either: it sits on a term, and the pass never opens a term.
- **Binder**: `DecideBinder::BindQualifiedReducer` resolves the relation, enforces
  well-formedness, and stamps `MakeQualifiedReducerTag(scope_idx)` on the bound aggregate's
  `alias`. `DecideQualifierContext` (`decide_binder.hpp`) carries what it needs from
  `bind_select_node.cpp`, which is still filling `entity_scopes` at that point.
- **Alias tags are now composable** (`common/enums/decide.hpp`): `AddDecideTag`,
  `HasDecideTag`, `ExtractDecideTagPayload`. A qualified `AVG` is tagged twice — once by
  the binder, once by the AVG→SUM rewrite — so tags concatenate and are matched by search
  rather than equality. The AVG and MIN/MAX indicator readers were converted accordingly.
- **Term plumbing**: `qualifier_scope_idx` on `Term`, `BilinearConstraintTerm`,
  `QuadraticGroup` and `Objective::BilinearTerm` (`physical_decide.hpp`), stamped by
  `ApplyAggregateMetadata` / `QualifierScopeOf`.

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
  - `typed_decide_variable: ColId '(' variable_type ')' | ColId '.' ColId '(' variable_type ')'`
  - Four further productions exist only to reject the removed spellings (bare
    `x`, and `x(INT)`) with a message naming the fix
  - `typed_decide_variable_list: typed_decide_variable | list ',' typed_decide_variable` (includes table-qualified syntax)
- **Binder** (variable processing loop, type mapping): `src/planner/binder/query_node/bind_select_node.cpp`
  - REAL → `LogicalType::DOUBLE`, BOOL/INT → `LogicalType::INTEGER` (the DuckDB-facing type — kept INTEGER even for `BOOL` so the variable can appear in arithmetic like `x * weight`, which BOOLEAN cannot implicitly cast into)
  - Boolean type detected via `type_marker == "bool_variable"` and recorded in `is_boolean_var[i]` — the domain signal, independent of the DuckDB type above
  - `is_boolean_var` carries the `[0,1]` domain from here through `LogicalDecide` to `PhysicalDecide`; nothing is synthesized into the `SUCH THAT` tree for it (see below)
- **ILP model builder** (variable type handling): `src/decidb/utility/ilp_model_builder.cpp`
  - DOUBLE/FLOAT → `is_integer = false`, type-default bounds `[0, 1e30]`
  - `LogicalType::BOOLEAN` → `is_binary = true`, bounds `[0, 1]`
  - INT → `is_integer = true`, type-default bounds `[0, 1e30]`
  - Lower bound: the input's resolved lower bound (from `physical_decide.cpp`) is taken **directly**, not `std::max`-ed with the type default — otherwise an explicit negative bound would be clamped back to 0. Upper bound is still intersected (`std::min`) with the type default. See "Signed variables" above.
  - A safety-net downgrade runs after the bound merge: if a variable would be `is_binary` but its merged bounds fall outside `[0,1]` (an unusual explicit user pin, e.g. `x >= -1` on a declared BOOL), it is reported as a plain bounded integer instead of a contradictory (binary, out-of-range) column.
- **Every BOOLEAN-domain variable's `[0,1]` box is applied directly to the solver column, never synthesized as a constraint.** This covers user-declared `x(BOOL)` (DuckDB type INTEGER) exactly like it covers optimizer-created auxiliaries with a real `LogicalType::BOOLEAN` type (MIN/MAX indicators, NE indicators) and INTEGER-typed auxiliaries that need to participate in arithmetic (IN-domain, L0 indicators): `PhysicalDecide::Finalize` (`physical_decide.cpp`) reports `LogicalType::BOOLEAN` to `SolverInput::variable_types` for any variable where `is_boolean_var[var]` is true, regardless of its DuckDB-facing type, and the ILP model builder's type-driven bounds above take it from there. `is_boolean_var` is the single authoritative domain signal end to end; `TraverseBoundsConstraints` in `physical_decide.cpp` only ever absorbs genuine user-written bound constraints (`x <= 10`, `BETWEEN`, an explicit BOOLEAN pin like `x = 1`), never the domain itself.
- **Solver backends**: HiGHS `!is_integer → kContinuous` (`deterministic_naive.cpp`); Gurobi `!is_integer && !is_binary → GRB_CONTINUOUS` (`gurobi_solver.cpp`)
- **Physical execution** (DOUBLE output path): `physical_decide.cpp` — returns raw `double` solution values for REAL vars
- **Table-scoped variables**:
  - `EntityScopeInfo` struct: `src/include/duckdb/planner/operator/logical_decide.hpp` — table alias + entity column indices per scoped variable
  - `VarIndexer`: `src/include/duckdb/decidb/ilp_model.hpp` — maps entity keys to solver variable indices, deduplicating across result rows
  - Entity mapping (Phase 1.5): `src/execution/operator/decide/physical_decide.cpp`
  - Physical index resolution: `src/execution/physical_plan/plan_decide.cpp`
