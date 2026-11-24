# Stage 06 — Model formulation

Translates the evaluated, solver-neutral `SolverInput` into a `SolverModel`: flat
variable arrays, constraints in COO, and an optional Q matrix. It owns the
variable layout, coefficient accumulation, bounds and indexing, and it owns the
**linearization** of the formulations stage 05 chose — the rows that encode a
tagged construct and the Big-M constants that scale them. It does no
SQL-expression canonicalization and knows nothing about any backend.

**Key source files**

- `src/decidb/formulation/ilp_model_builder.cpp` — `SolverModel::Build()`
- `src/decidb/formulation/ilp_linearization.cpp` — global-auxiliary allocation and
  `LowerDecideConstructs`, the entry point; the passes themselves live alongside it in
  `linearization_bigm.cpp` (Big-M constants and the per-row range walks),
  `linearization_minmax.cpp`, `linearization_not_equal.cpp` and
  `linearization_bilinear_abs.cpp`
- `src/include/duckdb/decidb/formulation/ilp_linearization_internal.hpp` — the helpers
  those files share
- `src/include/duckdb/decidb/formulation/ilp_model.hpp` — `VarIndexer`, `SolverModel`, `ModelConstraint`
- `src/include/duckdb/decidb/formulation/solver_input.hpp` — the input contract

Stage 06 has two entry points (`formulate_model.cpp`), and `PhysicalDecide` calls the pair. `PrepareModel` runs once
for a query, on the model as the emitter wrote it and the box the plan declares: the ABS envelopes join the rows,
the floors a `when` lowers are resolved, products and helpers are floored, and implied bounds are propagated, in that
order. `FormulateModel` runs once for each box the model is formulated against.

`Build()` takes `SolverInput` by **non-const** reference so raw global constraints
can be moved rather than copied, plus a pre-built `VarIndexer` threaded in from
`PhysicalDecide::Finalize()` so it is constructed exactly once.

---

## 1. `VarIndexer`: the four-block layout

```
Block 1: per-row vars (ROW)        [0, num_rows * num_row_vars)
Block 2: keyed vars (ENTITY)       sum of num_entities per scope
Block 3: query-wide vars (GLOBAL)  one column each
Block 4: global auxiliaries        ABS / MIN-MAX / McCormick auxiliaries
```

Scalars sit **below** `global_block_start` deliberately: the invariant "user
decide-variable columns occupy `[0, global_block_start)`" is relied on by the
model builder and by the column labeller in diagnostics.

| Block | Index formula |
|---|---|
| Per-row | `row * num_row_vars + row_var_offset[var_idx]` |
| Keyed | `keyed_var_base[var_idx] + class` |
| Query-wide | `scalar_column[var_idx]` — `row` is ignored |
| Global | `global_block_start + g` |

Three accessors matter:

- **`Get(var_idx, row)`** — resolve through a *result row*. A scalar ignores `row`;
  every row resolves to the same column.
- **`InstanceColumn(var_idx, inst)`** — walk a variable's *own* instances: rows for a
  per-row variable, class ids for a keyed one, the single column for a query-wide one.
- **`NumInstances(var_idx)`** — how many instances that variable has.

Construction and ownership:

- **`VarIndexer::Build()`** copies the keyed mappings so the indexer owns every part of
  its lookup state and can outlive the `SolverInput`. It is built once early in
  `Finalize`, threaded through `SolveModel` and `Build`, then moved onto
  `gstate.var_indexer` for readback in `GetData`. `total_vars` is refreshed just
  before the solve, once every auxiliary global has been appended.

There is no non-owning constructor. Focused tests use the same owning path as
production, so keyed-mapping lifetime has one contract everywhere.

**A decision read through a frame** (`../../language/guide.md` §5.6, D78) is a term on that decision, at its instance
at the row the frame selects: stage 08 resolves the row where it writes the term (`DecideInstanceOfRow::frame`), so
stage 06 knows no frame. Several rows can read one instance that way (`avg(at(first: x) ...)`), so the clause says
that a row may name a column twice (`EvaluatedConstraint::repeats_columns`) and its terms are summed per column, as a
helper's members written in are. The type, the box and the name a pass reads for the term are the decision's own.

**An instance with no column.** A keyed decision's absent class, and a helper instance stage 08 wrote where it is read
(`SolverInput::written_in`, §9 "A helper written where it is read"), own no column: `instance_slot` holds
`INVALID_INDEX` for it, a query-wide helper written in has no scalar slot (`NumInstances` is 0), `HasColumn` is false
and `ColumnOrNone` returns `INVALID_INDEX`. No term stage 06 reads names such an instance, so `Get` is never asked of
one. The indexer is laid out once (`assembly.var_indexer`).

---

## 2. Variable types and bounds

Type-driven defaults:

| `variable_types[var]` | `is_binary` | `is_integer` | Default bounds |
|---|---|---|---|
| `BOOLEAN` | true | true | `[0, 1]` |
| `INTEGER` / `BIGINT` | false | true | `[0, 1e30]` |
| `DOUBLE` / `FLOAT` | false | false | `[0, 1e30]` |

`PhysicalDecide::Finalize` reports `BOOLEAN` for any variable with
`is_boolean_var` set, whatever its DuckDB-facing type, so a declared `x(BOOL)` and
an INTEGER-typed IN-domain / L0 indicator both reach the `[0,1]` box through this
one type-driven path.

**Bound merging is asymmetric, and deliberately so** (`ilp_model_builder.cpp`):

- **Lower** — taken **directly** from `input.lower_bounds`, not `max`-ed with the
  type default. The physical operator already resolved each variable's lower bound,
  defaulting unconstrained variables to 0 and honoring explicit negative bounds, so
  it is authoritative. Taking a `max` would clamp a legitimate negative bound back
  to 0.
- **Upper** — intersected with `min`, because the type default is a true ceiling
  (`1.0` for BOOLEAN).

A box inverted by rounding alone is one point: a floor above its top by no more than one part in 1e9
(`DECIDE_ROUNDED_BOX_RELATIVE_TOLERANCE`, which direct solve's `DirectRowBox` reads too) is closed onto the top, since
boxes are derived in floating point from several sources (a bound divided by its coefficient, a helper's box through an
AVG's 1/n); a wider inversion stays, an infeasible model. `SolverModel::Build` closes every column once its box is
final, whatever formed or narrowed it: a decision's box, a helper's reach, a per-row bound folded in, a global
auxiliary's box (`test_edge_cases.py::test_rounded_box_is_one_point`, the tolerance relative to the top included). A
`binary` column's bounds are clamped to `[0,1]`: a BOOLEAN is 0 or 1 whatever is written on it (`x >= -1` widens
nothing; D41 lowers a numeric decision's floor, not a BOOLEAN's).

Bounds and type flags then expand across `NumInstances(var)` for each variable, each
instance's floor read through `SolverInput::InstanceLower`.

**The instance box refines the decision box.** A constant lower bound under a `when` or
a reducer filter (`EvaluatedConstraint::lifted_floor`) replaces the default floor of 0 on
the instances its kept rows reach, and only there (`ResolveLiftedFloors`,
`instance_floors.cpp`, run after the rigid snapshot and before implied bounds). A keyed or
query-wide instance is lowered when any of its rows is kept. The box stays one per
decision (a Flyweight), and `instance_lower_bounds[v]` refines it:
- `lower_bounds[v]` is the lowest floor of any instance: a lowered instance's tightest kept
  bound, 0 for an instance no such bound keeps. It is therefore a valid floor of every
  column, and every pass that reads the decision's box stays sound.
- `instance_lower_bounds[v][i]` is 0 where the default floor holds and −∞ where the
  instance follows the decision's box. An instance's floor is
  `max(instance_lower_bounds[v][i], box[v])`.
- The exact value of a lowered instance (−5, or −3 where two bounds keep a row) comes from
  the clause's own row, which `TryFoldPerRowBound` folds into the column, and which a
  diagnosis can loosen.
- `rigid_lower_bounds[v]` is re-opened, as for a folded user bound. A default instance
  stays at 0 through its refinement, so a diagnosis widens only the lowered instances and
  never quotes the default floor.

Storing "default or follow" per instance, rather than each instance's value, keeps the
refinement valid when a later pass moves the decision's box: `DeriveReducerValueBounds`
raises a helper's floor after this runs, and `OpenElasticColumnBox` widens it for a
diagnosis. A decision with a lower bound written (an unconditional floor, a BOOL, a signed
helper or a product) never takes the default, so a conditional floor only intersects with it and the
pass skips it. The Big-M sizings (`FlatColumnBox`, `DecideTightPerRowBigM`) and the `<>`
range collapse read each instance's floor through the same accessor, so a default
instance is sized and collapsed from 0.

Global auxiliaries take their own bounds and types from
`input.global_lower_bounds` / `global_upper_bounds` / `global_variable_types`. A helper
decision of the value rewrite (`SolverInput::reducer_value_links`) holds a reducer's
value, which may be negative: its definition is `Signed()`, so its box has no
default floor when the rigid box is captured, and `DeriveReducerValueBounds` (§9) boxes it by its members' reach. The rigid box
therefore holds a helper open below, so a diagnosis never lifts a floor it never had.

---

## 3. Objective

A flat `obj_coeffs` of size `total_vars`, zero-initialized. The objective is one row, so its linear part reaches
stage 06 as one coefficient per instance of each decision it reads (`SolverInput::objective_coefficients[decision]
[instance]`), which the builder places at `InstanceColumn(decision, instance)`. Stage 08 sums the objective's terms
into it (`DecideModelEmitter::ObjectiveCoefficients`): the terms' columns over the input's rows are its working state
and are freed there, a decision with one instance per class costs one number per class, and a helper written where
it is read is its members. The objective's constant arrives whole in `objective_constant`.

Every decision's coefficients take this one form, so a per-row decision's are read twice, once summed by stage 08 and
once placed here: 5 ms over 2,000,000 rows (`benchmark/decide/queries/clauses_times_rows.sql`, 30 ms in
`execution.tree_model` against 25 ms with one pass). Handing a per-row column over as it is would be a second route
for one shape.

For a keyed variable, every row of a class maps to the same column, and their
coefficients **accumulate** — which is exactly right: one keyed decision participates
in the objective across all of its rows. Every merged sum grows through one helper,
`AddCoefficient(target, value, written)` (`ilp_model.hpp`): the accumulator, the per-row map,
the objective's linear and product-group sums, quadratic rows, fixed terms moved to the
bound side, implied bounds, `<>`, MIN/MAX rows and value links. A sum past a DOUBLE raises
`DecideInvalidValueMessage`'s TOO_LARGE role naming the clause (`constraint_sources`) or the
objective (`objective_written`), never an InternalException or a solver API error. The model's hand-off checks raise
the same for a non-finite column bound, coefficient, objective entry or Q entry; a column is named by the first row
that reads it, else by the objective. A Big-M or ABS refusal with no open column (`ThrowUnboundedBigMNaming`) does too:
the range over finite boxes overflowed, and no decision lacks a bound. A literal coefficient that is not finite folds
into no box (`FoldKnownBound`), so its row's coefficient check refuses it. A coefficient the linear form computes
(like terms merged, a reducer's factor applied) keeps its inputs (`DecideTerm::inputs`); the read that refuses it
names the first input a column holds that is not finite on the failing row, else the clause
(`../../direct_solve/framework/done.md`, "Number messages").

The model is always a minimization and carries no sense. A `MAXIMIZE` query arrives as the minimization of its
negation, carried by every term's sign and the constant offset (`DecideCanonicalizer::CanonicalizeObjective`, stage 04).

---

## 4. Constraints

Each `EvaluatedConstraint` becomes one or more `ModelConstraint`s.

### One list per kind of row

Execution puts each clause row in the list of its kind (`DecideModelEmitter::Route`, `../08_execution/done.md`), and
each reader of `SolverInput` reads the lists it needs, without a test for the kind of a row:

- **`constraints`** holds every linear row: a clause's, a per-row bound, an ABS envelope (§9 ABS), and under
  `DIAGNOSE` an elastic folded row (a bound the optimizer folded into the column box, kept for the repair model).
- **`not_equals`** (`NotEqualClause`) holds a `<>`: its row and, for an aggregate one, the values each of its rows
  excludes (`excluded`: every value the group's rows give, each once, none for a row whose bound is NULL, an `AVG`'s
  times the group's size). It
  waits there until `LinearizeNotEqual` expands it into conditional rows and binaries, and is empty after. A `<>` is a
  pseudo-operation, and the expansion legalizes it before the model is built, as LLVM's LegalizeDAG does before
  instruction selection. The kind is the list, and the comparison's own `COMPARE_NOTEQUAL`, so the clause carries no
  tag and no index.
- **`quadratics`** (`QuadraticClause`) holds a clause with terms of degree 2: its row, which holds the linear terms,
  and its product groups. The builder states them as one block, in their order, in `SolverModel::quadratic_constraints`.
- **`bilinear_links`**, **`abs_links`** and **`reducer_value_links`** hold the formulations stage 05 chose.

The lists are one per kind, as `SolverModel` and the backend APIs keep them. DuckDB keeps one list of a class
hierarchy (`BoundConstraint`) and tests each constraint's kind where it reads it. Stage 06 departs from that because
these kinds live for different spans: a `<>` is used up before the build, and a quadratic row goes to a backend list of
its own. One list would test the kind at every reader. The model dumps keep the row order of `constraints`.

**A row that defines an auxiliary says so.** `EvaluatedConstraint::defined_var` is the auxiliary the row defines
(from the DEFINES tag), or `INVALID_INDEX`: the two lower-envelope rows of an ABS (`aux >= inner`, `aux >= -inner`)
define its auxiliary. A pass that asks what a clause demands of a column skips the rows that define it by that field
(`DemandedAuxReach`), and `EmitAbsEnvelopes` refuses an envelope row that does not define its auxiliary. The model row
carries no ABS mark. `FindUnboundedContributor` takes the column it skips as an argument.

Two passes read every clause row whichever list holds it, through `SolverInput::ForEachClauseRow`:
`OpenElasticColumnBox` (the elastic box of a column) and `DemandedAuxReach` (what a row demands of an ABS auxiliary).
Both ask of a row only its bound and its terms. Whether a `<>` or a quadratic row should size those boxes is open
(`../../diagnostics/infeasible/todo.md`).

### Emitted rows

`ClauseEmittedRows` builds a clause's rows once and keeps them on the clause (`EvaluatedConstraint::emitted_rows`),
for every path below: implied-bound propagation, the aggregate `<>` expansion and `SolverModel::Build`, which takes
them. They stay on the record because they are a function of the record and the column space, both fixed before the
first read; an object of stage 06 holding them would carry the same state through four signatures. The one pass that
edits a record after the first read (`FormulateModel` sizing a `norm(e, 0)` row) edits rows that read skips. `TermsHaveTheirOwnColumns()` returns true
only when (a) every decision the rows name (`variable_indices`, each once) is **row-scoped** and (b) no row names a
column twice (`repeats_columns`). Then each `(term, row)` pair has a column of its
own, so coefficients are copied straight.

Anything else — keyed or query-wide variables, or a repeated decide variable — sums each
row's terms per column in `SparseCoeffAccumulator`. The accumulator picks one strategy
per clause and reuses scratch across rows: a dense `vector<double>` indexed by flat column
plus a `touched` list when the decide-variable columns (`global_block_start`) fit 2^20
slots, or a sorted/merged `(idx, coeff)` pair list otherwise.

### A term beside the reducers: counted once by execution

The builder sums every row's coefficient of every term. A term outside the reducers
(`SUM(x) - max_shortfall <= cap`, `MAXIMIZE sum(x) + w * s`) counts once: execution kept
its coefficient on one row, the first kept row or its group's first row, and zeroed the
others (`CountOnce`, `../08_execution/done.md`). A query-wide decision inside a reducer
(`SUM(opening_cost * cap)`) keeps every row, so its coefficient is the sum of the counted
rows' data. The rule has one owner, so a writer cannot leave the builder a term whose
count it reads from elsewhere. `TermsHaveTheirOwnColumns()`
sends a query-wide term to the accumulator, since its one column would collide with itself. See `../../language/functions.md` ("Query-wide decisions").

### The rows a clause emits

A clause reaches stage 06 as one record per row it emits (`EvaluatedConstraint::records`, `ClauseRecords`): an
aggregate clause's groups that hold a row (one row in all for a clause of no groups), a per-row clause's kept rows (a
kept row, a member of an easy MIN/MAX, or under `per K:` a class's first kept row). A record holds the row's terms as
`(decision, instance, coefficient)`, the form a helper's members are listed in; the constant part of its left side;
its bound; the values it admits when it bounds one decision; its group, which names it and its class set; and its
first data row.

**A term names the instance it reads.** The instance is the row for a decision per row, the row's class for a keyed
one, 0 for a query-wide one; through a frame, the decision's instance at the row the frame selects. The node that writes a term holds the row, so stage 08 reads the instance there
(`DecideInstanceOfRow`), and stage 06 finds a term's column from the decision and the instance alone
(`VarIndexer::InstanceColumn`), as LLVM's machine IR names a register and not the source line that set it. A
product group's term names its instance at each place of its columns (`TermInstances`). So stage 06 reads no map
from rows for a helper, and a helper's scope hands it none: `KeyedMapping::row_to_class` is filled for a declared
decision alone, which the output reads back by row (`VarIndexer::ColumnOrNone`, `InstanceOf`). A
helper's group names the instance it defines (`ReducerValueMembers::group_instances`), and a row that defines an
auxiliary names the auxiliary's instance in its own term (`ClauseRecords::InstanceOf`). The data row of a record is
kept for a diagnosis, which names the row (`ClauseRecords::data_rows`). Every reader of a clause reads the records: implied bounds, the Big-M and range walks
(`DecideRowSignedRange`, `DecideRowTermRange`), `<>`, ABS, the lifted floors, the diagnosis and the builder. None
reads an array as long as the input or regroups one. The emitter writes them (`DecideModelEmitter::Rows` for a
per-row clause, `DecideWriteClauseRecords` for an aggregate one; `../08_execution/done.md` § A clause is read over the
rows it keeps).

The builder states one `ModelConstraint` per record: the terms resolved to solver columns (`ClauseEmittedRows`), and
the bound less the record's constant and the constants of the inlined helpers the row reads.

A quadratic clause's linear part is read the same way; its product groups are evaluated over the rows the clause
keeps, so the clause lists, for each row it emits, the places its products are read at (`QuadraticClause::rows`) and
the data rows they stand for (`data_rows`).

**A row that bounds one decision goes into its column box instead** (`TryFoldPerRowBound`).
When the clause is a bound on one decision (`x <= cap`, `2 * x >= lo`, `cap * x <= 10`,
`0.1 * x = c`), stage 08 has already evaluated, on each row, the values of `x` the comparison
admits (`EvaluatedConstraint::admitted_lower` / `admitted_upper`, from the classifier
`DecideReadSingletonRow`, `../05_optimizer/done.md`). The builder intersects that column's
`col_lower` / `col_upper` with them and emits no row. This is the per-row counterpart of
singleton-row folding, which folds a literal bound once per decision in the optimizer; a
data-valued bound differs per row and is known only here. The fold:

- **narrows, never replaces.** The admitted range is intersected with the box it finds, so it
  keeps the meaning of the row: a negative data value does not lower a decision's floor of 0,
  unlike a literal `x >= -3` (D41). A constant floor under a `when` has already replaced
  the default on the instances it keeps (§2), so its row sets their exact value. Several
  rows on one column (a keyed or query-wide decision) all hold.
- **admits what DuckDB admits.** A whole-numbered decision's range ends on whole numbers that
  pass the written comparison as DuckDB evaluates it, so `0.1 * x <= c` with `c = 0.3` admits
  3, as SQL does; the builder does no division or rounding of its own.
- **leaves an open end open.** `x <= +inf` admits every value, so the box stays as it is. An
  end that admits nothing (`x >= +inf`) keeps the row, which `SolverModel::ProvesInfeasible`
  answers before any backend.
- **does not run under diagnosis** (`SolverInput::diagnose`, set by the `DIAGNOSE`
  prefix). The elastic engine relaxes rows and quotes the clause from their provenance
  (`x <= cap_col + delta`), so a diagnosed statement keeps the rows.

A row with two decisions stays a row. A box the fold inverts is caught by the sanity check
(§8), which answers INFEASIBLE as the row would have. Constants derived from the formulation
box (Big-M, McCormick) are computed before the fold, against a box at least as wide, so they
stay valid. Gurobi's presolve turns such rows into bounds on its own; HiGHS's QP solver does
not, and a 9,600-row box-bounded QP that ran past 60 s as rows solves in 0.4 s as bounds. The
golden corpus (cases 102 to 110) shows each branch: lower, equality, coefficient -1,
coefficient 2, two decisions, an infinite bound, `DIAGNOSE`, a data coefficient that changes
sign, and a DECIMAL coefficient.

AVG terms are already coefficient-scaled by stage 08, by the rows the reducer reads in
its group: the kept rows of its `by` group, less those its own `when` drops, one per class
under its own `per` (`../08_execution/done.md` §5). The builder consumes the evaluated
coefficients directly.

Every term, in the three paths (through `ClauseEmittedRows`) and in the linear objective, is written through one
function (`ForEachTermColumn`): a decision at its column, or an inlined helper as the members of the instance its row
reads, each member term at its own row's column, times the term's coefficient, accumulated with the row's other terms.
The members' constants, scaled, join the row's fixed LHS terms (`EmittedRows::constants`), moved to the bound and
recorded in `rhs_mechanism_offset` as those are (§7), so a diagnosis quotes the bound as written; in the objective they
join its constant (`obj_constant`), as a kept helper's definition carried them into it. A clause skipped the rows
reading an absent instance (stage 08), so one reaching the builder is an internal error; the objective reads one as
no value, as it reads the spare column of an absent class (D84). A row over one decision that may fold into its box
never reads an inlined helper: the plan keeps the column of a helper such a row reads (`ChooseHelperForms`, stage 05).
A term that names an instance with no column is an internal error, whatever the decision's scope
(`VarIndexer::InstanceColumn`).

### Quadratic and bilinear constraints

Clauses with product groups (a square or a product of two decisions) are in `SolverInput::quadratics`. The builder
reads that list in a loop of its own, through the quadratic constraint builder, into
`model.quadratic_constraints`. Linear fixed LHS terms use the same `INVALID_INDEX` convention and are subtracted from
the RHS over the selected row set before the quadratic constraint is finalized. A decision beside the reducers enters
once per row set, because execution left it on one row (`CountOnce`): `SUM(POWER(x, 2)) + s <= 30` is not
`... + n*s`.

### Normalization on emission

Every path pushes through `PushNormalizedConstraint()` rather than
`model.constraints.push_back()`. It drops empty-LHS tautologies (`0 <= k` for
`k>=0`, `0 >= k` for `k<=0`, `0 = 0`).

A *violated* empty-LHS row — `0 <= -1` after bounds folding and term
cancellation, as written by `SUM(0 * x) <= -1` or `x - x <= -1` — is **kept**
(`ConstantRowFails`, which allows 1e-9 for the cancellation). A singleton row whose
coefficient is 0 on a data row (`cap * x <= -1e-10` where `cap` is 0) is judged by
DuckDB's own comparison instead, through the values the classifier admits (all or
none), so `-1e-10` fails as SQL says. The verdict is stored on the row
(`ModelConstraint::constant_fails`) and read by `ProvesInfeasible`. The row stays
coefficient-free and keeps its own
`origin.source_clause_id`, which is what lets the infeasible-diagnosis engine name the
clause and report a least-change repair (`SUM(0 * x) <= 0`) instead of a bare
status. The infeasibility is decided by DeciDB, not by a solver:
`SolverModel::ProvesInfeasible` reads the model's rows, and `SolveModel` answers
INFEASIBLE on it, so no backend is handed a row with no coefficients.

Emitting nothing and throwing instead would take the whole model with it — every
later consumer, diagnosis included, then has no model to read.

### Row order

The builder states every raw row (`SolverInput::global_constraints`: the helpers' definitions, the ABS and bilinear
links, the `<>` disjunctions) before the clause rows that read what they define. `LinearizeReducerValueLinks` writes the
links in the order they are stored, an inner helper's before an outer one's, so a definition stands before every use.
The order is for the solver: Gurobi solves the identical 20K-row Q4 model
(`benchmark/decide/queries/q4_minmax_nested.sql`) in 0.12s with the helper's rows first and 10.0s with the clause row
first, and the column order makes no difference.

### Reservation

Before the loop the builder sums an upper bound on `model.constraints.size()`
(aggregate → 1 or `num_groups`, per-row → `num_rows`) and reserves once. The HiGHS
backend does the same for its COO arrays.

---

## 5. `ApplyComparison()`

One function maps a comparison to a row's sense and bound, for a linear row and a quadratic one alike.

| `ExpressionType` | Sense | RHS |
|---|---|---|
| `COMPARE_GREATERTHANOREQUALTO` | `'>'` | direct |
| `COMPARE_GREATERTHAN` | `'>'` | `floor(rhs) + 1.0` |
| `COMPARE_LESSTHANOREQUALTO` | `'<'` | direct |
| `COMPARE_LESSTHAN` | `'<'` | `ceil(rhs) - 1.0` |
| `COMPARE_EQUAL` | `'='` | direct |

`'>'` and `'<'` mean `>=` and `<=` — standard ILP convention. Strict inequalities
are converted by adjusting the RHS (`DecideStrictBound`), which is exact for an
integer-valued left side and not for a continuous one. The builder checks nothing here:
the whole integer-step refusal is stated on declared types in stage 04's shape check (`CheckIntegerStep`), so only an
integer-valued left side reaches a strict comparison.

Whether a coefficient may be fractional is a *type* judgement made at plan time, not a
value check here: reading values would make a query's **validity** depend on the table's
contents, so inserting one fractional row could make a working query illegal. See
`../04_canonicalizer/done.md` §3.11 for the rule and what it costs.

A strict `<` / `>` over a REAL variable is also deliberately not folded into
column bounds upstream (stage 05), for the same reason the step is unsafe.

---

## 6. Quadratic objective

When `input.objective_product_groups` is non-empty, Q is built for the form
`minimize xᵀQx + cᵀx` — **not** the `(1/2) xᵀQx` form that some solvers take. A
stored value is the plain coefficient of its monomial: `q_vals[k]` multiplies
`x[q_rows[k]] · x[q_cols[k]]` exactly once, so `3·x²` stores 3 and `4·x·y` stores 4.
`ilp_model.hpp` states this convention on the struct, and every backend adapter
converts to its own solver's spelling rather than assuming one (§5 of
`../07_solver/done.md`).

Stage 08 evaluates every product group (`EvaluatedProductGroup`, `solver_input.hpp`) on each row: its weight `w`
(the group's sign times its coefficient, 0 on a row the group does not read) and the coefficients of the linear
form `l = a·x + c` it multiplies, and of `r = b·x + d` for a product. One loop (`AddProductGroupRow`) adds each
group on each row, in the objective and in a quadratic constraint alike:

1. **A square**, `w·(a·x + c)²`, is the symmetric rank-one update `w·a·aᵀ` (BLAS `SYR`): `Q[i,i] += w·aᵢ²` on the
   diagonal and `Q[i,j] += 2·w·aᵢ·aⱼ` off it. The factor of 2 is not a convention choice: expanding the square
   produces the pair `aᵢ·aⱼ·xᵢxⱼ` twice, once each way round, and only one triangle is stored.
2. **A product**, `w·(a·x + c)·(b·x + d)`, is the general rank-one update `w·a·bᵀ`: each pair `(aᵢ, bⱼ)` is added
   to the monomial `xᵢxⱼ` it is.
3. **Constants** — the cross terms with a form's constant go to `obj_coeffs` (`2wc·a` for a square, `wd·a + wc·b` for
   a product) and the product of the constants to the objective's constant. In a constraint they go to the row's
   linear part and its right side.
4. **Storage** — accumulated in a map over the lower triangle, then serialized to `q_rows` / `q_cols` / `q_vals`.

**Convexity is judged per group, not guaranteed by construction.** A square with weight `w` adds `w·a·aᵀ`, which is
positive semi-definite for `w > 0` and negative semi-definite for `w < 0`. The objective is minimized, so a sum of
squares is convex exactly when every weight is positive (a square written under `MAXIMIZE` arrives with its weight
negated). A product is indefinite. The builder records `nonconvex_quadratic` from
`IsNonconvexProductGroup`, the predicate stage 05's plan-time prediction reads (`../05_optimizer/done.md` → "The
model-class gate"). A weight whose sign only the query knows (data) has `weight_sign` 0 and counts as non-convex.

A square of a form over more than one decision variable, and any product, is *singular*
— one row of `A` gives a Q of rank one, whose minimizers form a flat valley rather
than a point. Gurobi handles that. HiGHS is declared not to support the
`singular_quadratic` model class, so stage 05 refuses the query before rows are read.

---

## 7. Provenance carried on every row

`ConstraintProvenance` (`solver_input.hpp`) travels with each emitted constraint so
diagnosis can report at the **user-clause** level rather than at raw matrix rows. An ordinary query records the
clause of each row (`origin.source_clause_id`) and builds nothing else: every other field is what a diagnosis reads,
and is stamped only under `DIAGNOSE` (`SolverInput::diagnose`).

**One origin value.** `DecideRowOrigin` (`decide_source_info.hpp`) holds the three ids of where a row came from: the
source clause (display), the removal group (the atomic DROP identity) and the repair group (elastic grouping;
execution numbers every clause it evaluates -- the lowered clauses, then under DIAGNOSE the folded (singleton-row)
bounds' clauses -- so no two clauses share one). It is a Value Object, held by composition and copied whole, as LLVM
copies a `DebugLoc`, so a site that copies an origin cannot drop one of the ids. Every record that describes a row (a
clause row, a `ModelConstraint`, a `GeneralConstraint`, a bilinear link) holds its origin as `provenance.origin` (the
link, as `origin`). A row a lowering states and a native construct are moved into the model with their provenance,
which is kept whole under `DIAGNOSE` and cut to the source clause otherwise (`KeepProvenance`).

**A clause row carries its provenance from the emitter.** `EvaluatedConstraint::provenance` holds the origin and the
clause-level facts execution reads off the clause: the `kind` (user parameter vs rigid mechanism row), the elastic
`shape` (does this row share one slack with its siblings or get its own; `DescribeBound` sets it, from whether the
clause's whole right side is one query-wide scalar), and the `rhs_label` of a data-backed bound. Under `DIAGNOSE` the
builder starts each row it states from that record (`ClauseProvenance`) and adds what only it knows: the group's key and
name, the class set, the LHS offset, the data row, whether the LHS was an aggregate, and the strict comparison's
original literal (so a suggestion can be re-quoted against it). Outside `DIAGNOSE` the row starts from the source clause
alone. The flat column of the `<>` disjunction binary the row belongs to (`indicator_col`) is stated by the lowering
that allocates it.

Six emission sites build a row from its clause's provenance -- linear and quadratic, each in ungrouped
aggregate / grouped aggregate / per-row shape -- and one free function in `ilp_model_builder.cpp` places the row, so
that no site stamps a group without its name:

- `StampConstraintProvenance(provenance, eval_const, group_key, lhs_offset, row)` sets the `group_key` and the
  `group_name` derived from it, `is_aggregate`, the folded LHS offset (`rhs_mechanism_offset`), and `class_set` from
  `EvaluatedConstraint::group_class_set` (`ClassSetOf`), the index of the group's classes in
  `SolverModel::class_sets` (moved from `SolverInput::class_sets` by `Build`; D28), each of a `Kind`: `KEY_VALUES` (a
  `per K:`'s values), `KEPT_ROWS` (no `per` key) or `WHOLE_CLAUSE` (`per ()`, one class with no key value). The
  per-row sites pass the row they emit: an easy MIN/MAX's member rows take their class set from their group and a
  `per K:` clause's row from its class, and the row sets `data_row` (the row it was emitted at, DIAGNOSE's `row`) and
  `per_kept_row` (a row with no class set: each kept row is a class of its own). A clause that reads a MIN/MAX helper
  is one of the aggregate rows, so its class set is the same.

No site stamps the clause's `when c per K:` qualifier: a diagnosis reads it from the
source registry by `source_clause_id` (`FindConstraintSource`,
`decide_diagnostic_render.cpp`), the one place it lives.

The comment marking the six sites numbers them 1-6 in emission order. No site stamps a term's coefficient or name:
a diagnosis quotes the clause as written (`SourceAwareLhs`, `../../diagnostics/infeasible/done.md`).

---

## 8. Sanity checks

After building:

1. **Bounds** — every variable must have finite bounds. An inverted box
   (`lower > upper`) is kept, so the model survives for the elastic engine;
   stage 07 short-circuits to `INFEASIBLE` without handing it to a backend.
2. **Objective coefficients** — every entry finite.
3. **Constraints** — `indices` and `coefficients` the same size, every index in
   `[0, total_vars)`, every coefficient finite, RHS not NaN (infinity is allowed
   for range representations).

These catch evaluation and model-building bugs where the message can still name
the column, rather than at the solver where it cannot.

---

## 9. Linearization

The `src/decidb/formulation/` linearization files hold the half of a formulation that
only becomes writable once coefficients are numbers. Stage 05 decides *which*
encoding a construct gets and records it as a tag on the constraint; this unit
turns the tag into rows. Everything in it is a pure function of `SolverInput`
data — evaluated coefficients, variable bounds, row and group ids. No expression
tree, no executor, no data scan.

### Implied bounds

`DecidePropagateImpliedBounds()` derives a second, data-driven source of column
bounds. For a constraint `Sum_t a_t x_t (<=|=) K` where every variable is
non-negative and every coefficient is non-negative, each instance satisfies
`x <= K / a` — the other terms only help — so the shared upper bound is
`max_r K_r / a_r`. A per-row constraint over one variable (`x <= cap`) drops no
other term, so it bounds a signed variable as well, by a bound that may be
negative; an aggregate one (`lhs_is_aggregate`, `SUM(x) = -3`) sums every row's
instance and keeps the sign condition. This turns declared-unbounded variables
into bounded ones, which is what makes a finite and tight Big-M possible. Only
provably-implied bounds are applied, so the feasible region and the optimum are
unchanged. A bound below the column's floor is applied too: the box is empty, which
proves the model infeasible, and the solver says so (the model builder closes one under
the floor by rounding alone). The pass reads each clause's rows as the builder
emits them (`ClauseEmittedRows`, also read by the `<>` expansion), so a bound and the rows it is derived from
cannot disagree. Whether a clause bounds every instance of a decision is asked of the decision's instances
(`VarIndexer::TermInstances`), never of the input's rows, so a clause costs what it emits.

`a_r` is the variable's **combined** coefficient at that row. A variable can hold
several additive terms of one constraint (`2*ship + 3*ship <= 10`, or two reducers
over the same decision), and all of them name the same solver column, so the pass
walks distinct variables and sums every term naming each one — `10/5 = 2`, not
`10/3`. The addition belongs here rather than in canonicalization because
coefficients are evaluated numbers by this point; at stage 04 they are still
unevaluated expressions over data columns, and combining them there would mean
opening terms algebraically, which that stage does not do.

The pass is a single sweep, not a fixpoint: a bound derived for one variable is
not fed back to tighten others in the same pass. That is sound and only leaves
tightness on the table for chained implications.

A row the clause does not emit is not constrained, so a decision is bounded only when a row the clause emits
bounds each of its instances.

### The formulation box

Every constant this stage derives — a Big-M, a McCormick envelope — is valid only
against a specific column box. `FormulationBox` (`solver_input.hpp`) names that
dependency, and each constant-deriving entry point takes it explicitly:
`DecideTightPerRowBigM()`, `LowerDecideConstructs()` (through `FlatRowReach` /
`FlatColumnBox`), and `LinearizeBilinear()`. `FormulationBox::OfSolvedModel(input)` is
the box the solved model declares.

Passing the box explicitly makes "which box is this constant valid for?" a question the
call site answers, not one about call order. It matters because the box does move: an infeasibility diagnosis
re-opens every user-folded and implied bound (`OpenElasticColumnBox`, stage 08) and
then runs against a strictly larger box than the retained constants assume.

Three cases, kept distinct:

| Reads | Who | Why |
| --- | --- | --- |
| `rigid_*` | `ClassifyNEConstraint`, `DecideRowSignedRange` | decides *structure* (is a disjunct dead), so the verdict must not turn on a bound a repair may lift |
| `FormulationBox` | the three constant-deriving sites above | a constant belongs to the box it was derived from |
| `lower_bounds` / `upper_bounds` | column declarations (e.g. a bilinear auxiliary floored at its product's range) | a write to the box, not a reading of it |

A constant may **not** read `rigid_*`. That box is open wherever the user supplied the
only finite bound, and these sites refuse rather than guess when a box is open, so the
query would be rejected while asking for the bound the user already wrote.

Passing the box explicitly changes no constant on the solved path — that call site passes
`OfSolvedModel`, which is what the sites read before. What it buys is a second formulation
against a *different* box, which is how an infeasibility diagnosis gets constants valid for
the widened box a repair searches instead of inheriting the solved model's. See
[`../../diagnostics/infeasible/done.md`](../../diagnostics/infeasible/done.md)
"The elastic model is a formulation, not a matrix patch".

### Big-M constants

`DecideTightPerRowBigM()` returns the maximum over active rows of the row's
effective bound magnitude plus its worst-case term contribution, plus a 1-unit
margin covering the integer-step band of the `<>` rewrite; its per-row worst case,
`DecideRowTermRange()`, sums each term's largest magnitude and serves only it. Every
other Big-M is sized from the expression's **signed interval**, walked by
`SignedRowRange::Of` (`ilp_linearization_internal.hpp`; `DecideRowSignedRange()` reads a constraint's row through it):
ABS, `<>` and the value rewrite's helpers (every MIN/MAX among them) share that one walk,
and terms that cancel (`c*x - c*y`) are measured together rather than each at its worst. Each end is walked on its own,
so a decision with a positive coefficient and a floor gives the row a finite floor whatever its ceiling. An end that no
contributing box derives is **open** (`lo_open` / `hi_open`; `Lo()` / `Hi()` read it as an infinity). An end that every
box derives but whose value overflows a DOUBLE is not open: it is a number too large for a DOUBLE, folded into the
auxiliary's box as an infinity, and the hand-off names the clause that reads it, on every path and both backends, as it
names every sum past a DOUBLE. Reading it as open would make one number an error in one clause and the solver's to judge
in another.
`AuxRange::Span()` is the auxiliary-family twin, for the MIN/MAX auxiliaries that are
pinned against a whole expression rather than compared to a fixed bound. An
`AuxRange` covers its members and nothing else: each end starts at the first member
that reaches it, so a family over [100, 200] spans 100. A family that covers no row
stays [0, 0], which the sites that pin an empty auxiliary to 0 rely on.

A tight M does not make a large coefficient safe. Past about `c * bound / K ≈ 5e5` on
HiGHS, an integer decision inside the integrality tolerance times `c` meets the row on
its own, whatever M is; that is precision in the user's numbers, and the number-range
warning (`../07_solver/done.md` §3) names it.

**There is no fixed Big-M.** When a contributing variable has no finite bound, no
constant dominates its range, and the query is **refused** — naming a column to
bound and the edit that bounds it. It is not given a large constant instead. That is
the one failure mode worth designing against: a guessed `M` the true range exceeds
does not error, it silently cuts the feasible region and returns a confidently wrong
optimum. ABS, `<>` and MIN/MAX all refuse; none falls back to a fixed large constant.

The refusal applies on **both** backends. Leaving one on the floor would mean the
same query answers correctly on Gurobi and wrongly on HiGHS, with no story for the
divergence — and a query's legality is not a property of the machine it runs on.

It fires only when the bound is genuinely unknowable. A box from implied-bound
propagation counts, and so does an ABS auxiliary's box: `DeriveAbsAuxiliaryBounds`
narrows the auxiliary's column to the range it pins it to, so an outer MIN/MAX over
`ABS(...)` sees a finite column. **That is why ABS is linearized first** — every
linearizer after it derives its `M` from column boxes, so an auxiliary they read must
already be boxed. Run them in the other order and `MAX(ABS(x - 5)) >= 3` over a
bounded `x` is refused for a bound that was there to be computed.

A non-finite effective bound is refused here, in SQL terms, rather than reaching
the model validator as an internal error. Callers that can read an infinity by
direction classify it first, so what still arrives is a bound with no reading at
all — NaN — and the auto-`M` `norm(e, 0)` links, whose `M` bounds an expression
rather than answering a comparison.

### Extremum links — the one place a MIN/MAX auxiliary is pinned

**A family of one member is that member** (deck p. 112). `EmitExtremumLink` states it as one equality,
`result - member = constant`, with no envelope, indicator or Big-M -- so a side no box bounds needs none.

Every MIN/MAX in the model is the same statement: some auxiliary column equals the
extremum of a family of linear expressions. `EmitExtremumLink()` is the only thing
that writes one, and `ExtremumLinkSpec` is the only description of one. One site feeds it: a MIN/MAX helper of the value
rewrite per group, in a constraint of any shape and in the objective, a nested reducer's among them. It writes no
formulation by hand.

A link has **two independent sides**, and which of them the surrounding model already
supplies is a property of the *site*, not of the construct:

- The **envelope** (`result >= member` for MAX) holds the result at or above every
  member. An objective that maximizes it supplies this for free.
- The **closing** side pins the result down onto some member: one Big-M row per
  member and a `SUM(y) >= 1` that makes one bind. An objective that minimizes it
  supplies this for free.

A helper needs the side its readers push it against, and stage 05 records which (`DecideHelperDefinition::push`, which the tree reads into the link's `need_envelope` and
`need_closing`;
`../05_optimizer/done.md`, "MIN/MAX"): a MAX read pushed down needs the envelope alone, read pushed up the closing side
alone, and read both ways (`=`, `<>`, a factor whose sign is known only when the query runs) both. A clause that reads
`MAX(e) >= K` needs `z <= MAX(e)`, the closing side: a `z` inflated past every member would satisfy a bound nothing else
does. One that reads `MAX(e) <= K` needs the envelope, and the closing side would be rows for nothing, since nothing
pushes `z` down.

**Native is the closing side's alternative, never the envelope's.** The envelope costs
one row per member, needs no constant to dominate anything, and is *implied* by a
general constraint rather than contradicted by it. So where a spec asks for it, it is
emitted on both arms. The choice — `result = MIN/MAX(cols)` stated for the backend, or
the Big-M family that encodes it — is made once, inside `EmitExtremumLink`, and the
rule is `NativeConstructPolicy`, the one rule an ABS reads too (§ABS): native is the default, and a declared
construct is lowered only where the lowering is measured faster. The lowering is measured faster for MIN/MAX, so a
MIN/MAX is stated natively where the family's reach is underivable and there is therefore no Big-M at all.

A member stage 05 defined for a non-linear body (`max(power(x, 2))`) is a helper column
with no box (`../05_optimizer/done.md`, "Non-linear reducer bodies"), so its family is
always underivable and every extremum over it takes the native arm, an extremum over a
nested helper defined over such members included. `ExtremumArgumentColumn` hands the
helper itself to the general constraint (a sole column), so no pinned copy is added. Stage
05 refuses such a query on a backend with no native MIN/MAX, so the lowering arm never meets
one.

**A link's rows are mechanism.** Every row of a link is `STRUCTURAL`, rigid under DIAGNOSE. A closing row carries the
Big-M and encodes `z = MIN/MAX(members)`: offered to the elastic engine as a clause the user may edit, its slack would
be in the auxiliary's units rather than the user's bound's. The user's clause is the row that reads the helper, which
holds the bound in the user's own units and carries its source (`../../diagnostics/infeasible/done.md`,
"Rigid shapes").

**A helper's rows name the clause that reads the helper.** Every row a helper's definition writes (a link's pin,
envelope, closing and selector rows, and a SUM or AVG helper's definition row) takes the clause that first reads the
helper as its origin (`ExtremumLinkSpec::source_clause_id`, `ReducerValueLinkSpec::source_clause_id`), so a refusal of
one of them, a number too large or HiGHS's load window, names that clause. DIAGNOSE sees nothing new: its removal
groups key on `removal_group_id`, which these rows never set, its labels skip `STRUCTURAL` rows, and its elastic blocks
take only `USER_PARAMETER` rows.

**A member becomes a column only when it is not one already** (`ExtremumArgumentColumn`,
§9a). `MAX(2 * x)` and `MAX(x + 1)` are genuine expressions and earn a column, boxed by
that member's own reach.

**The closing Big-M is resolved by the caller**, because the family a link reduces
over is not always the per-row family: an outer MIN/MAX over group *sums* reduces over
the group-sum family, which leaves any single row's range as soon as a group holds more
than one row. Every caller passes the family its own members live in, and the Big-M and
the blame column both come from it, so a link cannot be scaled off one family while its
members sit in another.

**The value is always `AuxRange::Span()` — the family's full reach, constants
INCLUDED.** A deactivated member row reads `z - expr_var <= M + const`, whose worst case
is `family.hi - member.lo`; `family.hi` belongs to whichever row reaches highest, so one
row's constant is measured against a *different* row's. Only within a single row does a
constant cancel in the `(aux - expr)` difference. A reach that ignores the constants is
sound only while every row carries the same one: for `MAX(x + c)` with `c` a data column
it gives an `M` too small to be valid, and a too-small `M` cuts off legal answers rather
than merely loosening the relaxation. Corpus queries 89–92 cover it.

**The clause reads the helper.** A MIN or MAX in a constraint is one column per group, the helper's instance, and the
clause is an ordinary row over it, `z <op> K` with the clause's SUM and AVG terms beside it. Both arms produce that row,
so an infeasible MIN/MAX is diagnosed identically whatever backend the host has, and the bound is never spread across a
per-row Big-M family (`e_r - M*y_r >= K - M`), which would hide `K` inside every row. The cost is one column and one row
per group; the link's rows carry `z` in place of the bound. The extremum column is labelled with the reducer as the plan
prints it (`DecideHelperDefinition::label`), so a repair names `max(x * c)` rather than an internal column.

**No indicator is allocated before the rows that read it.** Stage 05 allocates none: a MIN/MAX is a helper column there.
The binaries a Big-M pinning needs are global-block columns `EmitExtremumLink` creates for the members it actually
writes, so the native arm allocates none at all.

### Helper decisions of the value rewrite

Stage 05 replaces each decision reducer of a VALUE-shape clause, each MIN or MAX of any other clause or of the
objective, and each range frame over decisions, by a helper decision (`../05_optimizer/done.md`); stage 08 lists its
members per group -- a `by` group, or a frame's window -- into a `ReducerValueLinkSpec` (`solver_input.hpp`):
`ReducerValueMembers`, per group the row it is read at and its members, each a constant and
terms (decision, row, coefficient), with the link's label. Listed, not rows with a group each,
because a range's windows share rows where a `by` key's groups partition them; one form for
every link, which the three passes below read. The helper's own column enters only here
(`linearization_reducer_value.cpp`); a member's terms reach the extremum link through an
`ExtremumMemberSource` as a row's do.

`DeriveReducerValueBounds` brackets, per group, the reducer's value — the sum of the
members' signed brackets for SUM/AVG (an AVG's coefficients already carry the 1/n), their
cover for MIN/MAX — and boxes each helper instance by its own group's reach
(`instance_reach_lower` / `instance_reach_upper`, by the instance each group names),
each end on its own; an end no member bounds stays open. The decision's box is the cover of
every instance's reach, so every pass that reads it stays sound, and only ever tightens.
The reach is not the floors' refinement: a floor holds against any box, the rigid one
included, while a reach is derived from the members' current boxes and holds only for the
formulation it came from. It is written afresh by every `FormulateModel`, so a diagnosis
derives it from its widened members. The model builder, the closing Big-M, `FlatColumnBox`
(through `FormulationBox::InstanceUpper` / `InstanceLower`) and the ABS argument boxes read
the instance box (`InstanceBox` with `reach` set); the `<>` range collapse keeps the rigid
view (`InstanceBox` default) and never reads a reach. A member may be an ABS
auxiliary, a product with a helper (`b * sum(x)`) or hold one, and a product's factor may be an ABS auxiliary, so for
every model `FormulateModel` alternates `DeriveBilinearAuxiliaryBounds` and `DeriveAbsAuxiliaryBounds` (neither
refusing) with this derivation and its backward half until no box moves, and only then runs the two that refuse (a
bilinear factor with no ceiling names the unbounded member, not the helper). A model with none of these constructs
settles in one pass. The backward half, `DeriveReducerMemberBounds`, lets a helper's box bound
the helpers it is defined from (a nest, guide §5.5): each term of a SUM or AVG is at most the
helper's top less every other term's floor where the definition states `value >= members`,
at least its floor less every other's top where it states `value <= members`; a MAX's
envelope keeps every member under its top, a MIN's over its floor. Repeated columns of one
group are summed first (`CombineColumns`: `sum(t.w * max(x))` is `2m + m`), and one box
covers every instance, so it tightens only when every instance was bounded, by the loosest.
The box is a bound on the helper's columns, so an instance with none -- a one-member MIN/MAX
group written into its readers -- neither tightens it nor is bounded by it: its readers bound
its member directly.
So a nested MIN/MAX whose closing side needs a Big-M finds one in a bound written only at the
root (`per t.g: sum(max(x) by (t.g, t.h)) by (t.g) = 10`). Every derivation only tightens.
The loop stops when no box moves; at the first box that crosses, which proves the model
infeasible (the solver then reports it as for any empty box); or after twice as many passes
as there are ABS, product and helper links plus one, a box still closing in by ever smaller
steps then standing as it is. Each ABS derivation starts afresh
(`range_unbounded` is reset), so a range open on one pass may close on the next. The backward half passes over a group
none of whose terms is a helper -- every group of a reducer over decisions alone -- unread: only a helper's column takes
a bound there.

`LinearizeReducerValueLinks` then states each definition over the groups
`BuildMinMaxGroups` splits the evaluated rows into (each group's rows; a member is built only
when a row is emitted, `MinMaxRowMembers`, late materialization). A SUM/AVG
helper gets one flat row per group in `global_constraints`, on the side stage 05 recorded
(the epigraph form): `value - members' expressions >= members' constants` when the clauses
push the value down (`need_envelope`), `<=` when up (`need_closing`), `=` when both. The
members' columns are accumulated first, in one scratch row every group of every helper reuses
(a keyed decision may stand in several rows of a group; one dense row per group was an
allocation the size of the column space per window), STRUCTURAL, with the clause's provenance.
A helper stage 08 wrote where it is read is not among the links, so it gets no definition. A quadratic body is not in the row: stage 05
defined it per row, so the members are columns. A MIN/MAX helper gets one extremum link per
group through `EmitExtremumLink`, its result column the helper's instance for that group, on
the same sides; the closing Big-M is the instance's box end against the family's far end
(the instance's ceiling `- family.lo` for MAX, `family.hi -` its floor for MIN), so a small
group's Big-M is its own reach, not the widest group's. Where that constant does not exist the
lowering arm refuses naming a column to bound and the native arm states the extremum, as
for every other MIN/MAX.

A group whose reach overflows a DOUBLE at an end the helper's settled box leaves open is refused with TOO_LARGE once
its definition is emitted (`RefuseOverflowedReach`). `DeriveReducerValueBounds` meets that infinity against the open
end's 1e30 and keeps the 1e30, a bound the data do not give, so the check reads the reach against the settled box: a
clause that bounds the helper (`max(...) <= 5`) makes the overflow harmless, as it does for an open member. It runs
after the definition rows, so a sum or a Big-M the definition itself overflows names its own cause first. Every
TOO_LARGE of a helper link names the clause that first read the helper as written: the refusal, the extremum link, a
member or group sum past a DOUBLE (`LinearizeReducerValueLinks`), a member's constant (`DecideModelEmitter::ValueLinks`)
and a member's computed coefficient (`DecideTreeColumns::Numbers`), through `ReducerValueLinkSpec::source_clause_id` or
the helper definition's `origin` and `DecideWrittenClause` (the objective when none reads it), on both backends
(`test_invalid_value_message.py`, `HELPER_SUMS`).

### A helper written where it is read

A helper decision costs a column, a definition row and its reads; written where it is read, it costs none of them, and
its members' terms stand in each reader instead. Which helper is written in is the plan's
(`DecideReducerValueDefinition::form`, `ChooseHelperForms`, stage 05; context/docs/pipeline/decisions.md R42 to R44,
R47),
and stage 08 writes it (`DecideModelEmitter::HandLink`, `WriteMembersIn`; `../08_execution/done.md` § A helper the plan
writes where it is read): a `WRITTEN_IN` SUM or AVG whole, a `ONE_MEMBER_WRITTEN_IN` MIN or MAX group by group,
where a group has one member (deck p. 112, R44), and a `ONE_ROW_WRITTEN_IN` SUM or AVG group by group, where one row
of its reading clause reads the group (R47): the two facts the data supplies. Stage 06 decides nothing and expands
nothing:

- **The links** (`reducer_value_links`) hold the groups that keep a column, and each gets its definition.
- **A clause's records** name the members' decisions where a term read a written-in instance, the members' constants
  in the record's constant. `EvaluatedConstraint::repeats_columns` says a row may then name one column in several
  terms, so the builder sums its terms per column. `variable_indices` lists the decisions the rows name, each once,
  for every clause; the decision of each term slot is the emitter's own working column
  (`EvaluatedClauseColumns::term_variables`) and stays behind when the record is taken (`Take`).
- **The objective's terms on such a helper** are summed into its members' instances by stage 08
  (`SolverInput::objective_coefficients`, §3); the constants they carry are in `objective_constant`.
- **The column layout** gives a written-in instance no column (`SolverInput::written_in`, §1).

`x >= sum(from 2 previous to previous else 0: x) over (...) - t.w` has one column and one row per input row, the
windows written into their anchors' rows, where a helper per window doubled both. The running example has the deck's
model, 8 columns and 7 rows (p. 115). DIAGNOSE formulates its repair model from the same input, so it reads the same
rows, and its findings name them as written. Tests: `test_anr_inlining.py` and `test_anr_nested.py` (model sizes and
answers), `test/common/test_decidb_helper_form.cpp` (the plan rule), `test/common/test_decidb_helper_inlining.cpp`
(the layout, the summed row and the objective's members, on hand-built inputs).

### MIN/MAX objectives

A MIN/MAX objective cannot be a coefficient vector, so it is a helper decision like any other (stage 05): the objective
reads its column with the term's coefficient, and `LinearizeReducerValueLinks` defines it above. A nested one,
`outer(per k: inner(e) by (k))`, is one helper per level (D81).

Which sides the link emits is which of the two the objective supplies for free. The objective is a minimization, and a
negative sign reads the other extremum: `MAXIMIZE min(e)` minimizes `-min(e)`. A MAX pushed down (`MINIMIZE max(e)`) or
a MIN pushed up (`MAXIMIZE min(e)`) is driven onto the extremum by the minimization, so the closing side is free and
only the envelope is emitted, with no binaries. The other two (`MAXIMIZE max(e)`, `MINIMIZE min(e)`) push the helper
away, so the envelope is free and the closing side is emitted instead.

The members are the rows the objective's reducer reads (stage 08 folds the objective's `when` into the members),
whatever their coefficients: a row whose data is 0 is the member 0, not an absent one. A reducer with no member row has
no value, so the objective keeps no term for it (D68).

### `<>` disjunctions

`LinearizeNotEqual()` encodes `x <> K` as the disjunction it is: two **conditional
rows**, `z == 0 => LHS <= K-1` and `z == 1 => LHS >= K+1`, on a binary of that
instance's own. That is the only spelling it emits. Nothing here asks which backend is
in play and nothing here computes a bound — a conditional row needs no constant to
dominate it, so whether a contributing variable is bounded is a question for
`LowerDecideConstructs` and only for it.

Both halves carry the clause's `indicator_col` **on the row**, which is what lets the
elastic engine group them and offer removal rather than loosening half a disjunction.
That is also why `<>` is stated as a conditional row rather than as a general
constraint: a general constraint carries no row for diagnosis to reach, and dropping
the clause is the only repair a `<>` has.

Two guards run before anything is emitted:

- **The left side is linear and integer-valued.** The ±1 band is exact only on the integer
  lattice; on a continuous quantity it would cut the feasible points in `(K-1, K+1)`. The
  stage 04 integer-step check admits only an integer-valued left side, so the builder does not
  check it. A product or a power of decisions on the left never arrives: stage 05 refuses
  it.
- **`NEIsIntegerValuedRhs`** *drops* a comparison whose bound no integer can equal,
  because every assignment already satisfies it. Emitting the pair anyway would
  wrongly exclude `floor(K)` and `ceil(K)`. An infinite `K` is the same case for the
  same reason.

A per-row spelling with a row-varying bound passes over only its non-integer rows, instead of dropping the whole
constraint.

**The binary is allocated where the disjunction is written**, one global per emitted
instance, and labelled with its clause as written (`QuoteWrittenClause`, by the clause's
`origin.source_clause_id`). Stage 05 records only the plan's account of the clause and allocates nothing: a
row-scoped Boolean per data row would be wrong for three of the four things a `<>` can become. An aggregate spelling
needs one binary per *group*, a collapsed one needs a column but no disjunction, and a dropped one needs nothing. None
of those is knowable before the data is seen.

#### The range collapse

`LHS <> K` is a disjunction only when `K` sits strictly **inside** the range the LHS can
actually reach. When the reachable range lies wholly on one side of `K`, one branch is
dead and the survivor is a plain inequality — no indicator term, no Big-M, and an LP
relaxation that is tight instead of empty. `ClassifyNEConstraint` reads the four cases off
the signed interval:

| Range vs `K` | Emitted |
|---|---|
| `K` interior | the two-row Big-M disjunction |
| range never falls below `K` | `LHS >= K+1`, one row |
| range never exceeds `K` | `LHS <= K-1`, one row |
| `K` unreachable | nothing — the row excludes nothing |

This matters because the Big-M pair's relaxation carries no information at all: setting
`z = 0.5` slackens both rows by `M/2`, so the LP admits `LHS = K` — the very value being
forbidden — and the disjunction only starts to bind under branching. The common
`SUM(x) <> 0` over decisions that cannot go negative is exactly the collapsible case, and
it becomes `SUM(x) >= 1`.

`DecideRowSignedRange` computes the interval, respecting coefficient signs where
`DecideRowTermRange` takes magnitudes — a Big-M only has to dominate a range, but a
collapse has to know which side of `K` it lies on. An open side reads as an infinite
endpoint (`Lo()` / `Hi()`) rather than declining outright, because both collapses are one-sided: a decision
that is merely non-negative has a finite floor and no ceiling, and that floor alone
licenses `>= K+1`.

**The collapse may only read the rigid box** (`SolverInput::rigid_lower_bounds`), meaning a
variable's intrinsic domain — BOOLEAN 0/1, default non-negativity — and nothing else. The
column box also accumulates user bounds folded by stage 05 and implied tightenings from
`DecidePropagateImpliedBounds`, and both are backed by rows the elastic engine *reverts*
during infeasibility diagnosis so the loosenable row is the sole enforcer. A Big-M constant
may read the full box, since a loosened bound only makes it conservative; a rewrite that
changes what a constraint *means* may not, because it cannot be reverted along with the
bound it consumed. So `x <= 5 AND SUM(x) <> 0` collapses on the intrinsic floor, while
`x >= 0 AND x <= 5 AND SUM(x) <> 0` — the same feasible set — keeps the disjunction.

Per-row and aggregate spellings differ in granularity. Groups are already emitted
independently, so each gets the encoding its own range earns. A per-row constraint shares
one `EvaluatedConstraint` across every row, so a mixed verdict would mean splitting it into
up to three constraints with complementary row masks; instead the verdict must be unanimous
across active rows, and a mixed one keeps the disjunction unsplit.

A collapsed row **still allocates its binary**, appearing in no row, because the ordinary
formulation keeps the same column layout across equivalent `<>` shapes. Atomic DROP does
not depend on that column: the source comparison's `origin.removal_group_id` is propagated to
every emitted descendant and the diagnostic candidate physically omits the whole group.
A row dropped as a tautology allocates nothing at all.

**Aggregate spellings expand per group.** They need one binary per group rather than per
row, and their LHS is the group's rows summed onto one row, so `ExpandAggregateNotEqual`
accumulates each group's coefficients and emits the same two conditional rows over the
sum. A decision beside the reducers enters once per group, since execution left it on the
group's first row (`CountOnce`; `SUM(x) + s <> 5` is not `... + n*s`); a clause with one takes no range collapse, since
the per-row
ranges would count it once per row. Groups skipped by the integer-RHS guard allocate no binary, so the model stays
clean.

### Bilinear products

`LinearizeBilinear()` emits the rows of each `w = b * x` link once per instance of `w`, in flat
columns (as a helper's definition is), from the box of the instance of `x` that instance
reads (`VarIndexer::InstanceUnder`). A query-wide product is one block, a keyed one a block per
key, a row-scoped one a block per row (the scope of `w` is stage 05's `JoinScopes`); the
instance fixed at 0 for rows that read no key links nothing. For two Booleans the block is the
AND, `w <= b`, `w <= x`, `w >= b + x - 1`. With `b` Boolean and `x` in `[L, U]` the
McCormick envelope is:

| Row | Condition |
|---|---|
| `w <= U*b` | always |
| `w >= x - U*(1-b)` | always |
| `w <= x - L*(1-b)` | `w`'s floor `< 0`; collapses to `w <= x` otherwise |
| `w >= L*b` | `w`'s floor `< 0` only |

**A product has no default floor of 0.** It takes its factor's sign (`b * x` is `x` where `b = 1`), so its definition is
`Signed()` (`DecideBooleanProductDefinition`): no decision's default floor is applied
to it, `ResolveLiftedFloors` leaves it alone, and the bound check before solving gives it its factor's floor. Its
column's floor is the product's range, `min(0, L)` over the lowest `L` of the instances, raised by a floor written on
it (`b * x >= -4`, or `>= 0`). `FloorBilinearAuxiliaries` sets it from the factors' boxes before implied-bound
propagation, once the lifted floors have lowered them, so `b * x` over x >= 0 is a non-negative term of a budget row
(the goldens' product columns take their ceiling of 8 that way); `LinearizeBilinear` raises it to the range over the
final box, which a helper factor (`b * sum(x)`) only has after its members are derived. Where that floor is 0 or
more it implies the lower corner, so three rows suffice; where it is negative all four are emitted for every
instance, one whose own `L >= 0` included, so the product reaches the negative value of `x` at `b = 1` and is 0 at
`b = 0`. The floor is the product's range, not the default 0, so a floor written on the product holds, a `when` floor
lowers the product's other rows with it, and `b * x + 3 <= 1` is not refused as never negative. A missing finite
upper bound on `x` is refused by name, since there is no envelope without it; where `x` is an ABS helper the refusal
quotes the `ABS(...)` as the user wrote it (the tree's `ABS` node carries that text), never the helper. A factor that is
a helper or an ABS auxiliary has no box until the derivations above settle, so the refusal is decided on the settled
boxes and names the member.

### L0 counts

An L0 count `norm(e, 0)` is a sum of indicator columns, one per instance, each tied to the counted expression by rows
on both sides: `M*z - e >= 0` and `M*z + e >= 0` hold `e` at 0 where `z = 0`, and `|e| - tolerance*z >= 0` holds
`|e|` at the tolerance or more where `z = 1`. The indicator is therefore pinned in both directions, so the count is
exact whether a clause caps it, floors it or equates it, and whether the objective minimizes or maximizes it. A value
of `e` strictly between 0 and the tolerance has no feasible indicator. An auto `M` is the span of `e` over the rows and
is refused where `e` has no finite bound.

The plan states which rows take an auto `M`: the indicator's F node holds its rows in a fixed order, and the emitter
stamps the two `M*z` rows with their indicator (`EvaluatedConstraint::sized_indicator`). `FormulateModel` sizes
exactly those rows and reads no row's shape, so `norm(abs(e), 0)`, whose `M*z` rows hold only the indicator and an ABS
helper, is sized as any other. The `M` is derived once the boxes of ABS helpers, products and helper decisions have
settled, since `e` may hold any of them, and before those constructs' refusals, so a count over an open decision is
refused as the count. A refusal names an ABS helper by the ABS as written.

### ABS

Stage 05 records each ABS (an `ABS` entry of `LogicalDecide::helper_definitions`) and emits no row. The tree builds its two envelope rows,
`aux >= inner` and `aux >= -inner`, beneath the ABS's F node, as it does for a definition, and the emitter evaluates
both into `SolverInput::abs_links` (`AbsLinkSpec`: the auxiliary, whether the ABS is exact, the ABS as written and as a
plan prints it, the two rows). Every row of an ABS is emitted in this stage. The ABS is a pseudo-operation, as in LLVM's
legalization: the lowering either states it natively (legal) or expands it.

**The envelope is emitted at hand-over.** `EmitAbsEnvelopes` moves both rows into `input.constraints` as the evaluated
model is handed over, before any pass reads the rows, so implied-bound propagation reads `ABS(x) <= 3` as `x <= 3`
through them. The pair holds `aux` at or above `|inner|`. Where what pushes on the ABS holds `aux` down (a positive ABS
under `MINIMIZE`), the pair alone pins `aux = |inner|`. Where it pushes `aux` up (a positive ABS under `MAXIMIZE`, which
arrives negated, or a hard direction) the pair does not, `aux` is free to run above `|inner|`, and stage 05 marks the
ABS exact. Only an exact ABS gets the rows below.

**An envelope row holds no aggregate.** Stage 05 gives every reducer a function reads a helper decision
(`../05_optimizer/done.md`, the objective's value rewrite), so each envelope row is one instance of its F edge's scope,
built by `DecideModelEmitter::Rows`, and the phases below read it per row. `EmitAbsEnvelopes` refuses a row that holds
an aggregate as an internal error (`test/common/test_decidb_abs_envelope.cpp`).

**Every ABS is boxed, exact or not.** `DeriveAbsAuxiliaryBounds` narrows each auxiliary's column to the largest
`|inner|` any row reaches. An ABS that is not exact has no Big-M to size, but a reader may need its top: a product with
a BOOL decision (`sum(b * abs(x - 2))` under MINIMIZE), a MIN or MAX, a `<>`. The box is sound for it: every reader
pushes it down, so its value is `|inner|`, which the top holds. An open reach leaves its box open, and a reader that
needs the box refuses it through its own check.

**Each sign row has its own M.** `DeriveAbsAuxiliaryBounds` takes the signed interval
`[lo, hi]` of `inner` over every row (`AbsLinkSpec::inner_lo` / `inner_hi`). The
row `aux + inner <= A2·y` holds `2·inner` when `y = 1` pins `aux = inner`, so
`A2 = 2·max(0, hi)`; the row `aux - inner <= A1·(1 - y)` holds `-2·inner` when `y = 0`
pins `aux = -inner`, so `A1 = 2·max(0, -lo)`. One shared constant, `hi - lo`, is too
small for an interval that is not symmetric (`[-1, 100]` needs 200 on one side), and a
side the interval never reaches needs no relaxation at all.

The link is keyed by the **auxiliary**, since only the lowered form has a sign indicator: see "one binary, on one
form only" below.

This site is **strict about bounds**: a contributing variable with no finite bound is
named and refused, and the indicator paths make the same refusal in the same words. An infinity in the bound carries the constant part of the expression the user
wrote inside `ABS()`, so that refusal names the expression and the row, not the
linearization.

**One decision, made once.** `DeriveAbsAuxiliaryBounds` is phase 1 and runs for both forms. `EmitAbsConstraints` is
phase 2, and the one place native against lowered is decided for an ABS: `policy.Use(link.range_unbounded)`, the rule
MIN/MAX read (`NativeConstructPolicy`). Native is the default: a backend that declares the construct states it
natively, and a construct is lowered only where the lowering is measured faster. MIN/MAX is: on the Q9 shape the native
form ran 1.7x-3.3x slower, and 40x slower on `MAX(e) >= K` at 30K rows. ABS is not: on Gurobi on Q10 (200K rows) the
lowered form ran 12.0-12.2 s against 12.2-12.3 s native, with the same objective, a tie. So an exact ABS is stated
natively wherever the backend declares it, and expanded on a backend that does not (HiGHS, or Gurobi under
`DECIDB_NATIVE_CONSTRUCTS=off`). Native is the only form for an ABS with no finite Big-M, and a backend without it
refuses that case. `DECIDB_NATIVE_CONSTRUCTS=force` states every declared construct natively; for an ABS that changes
nothing.

- **Native** (`StateAbsNatively`): a column `t` per active row, an equality row `t = inner`, and a general constraint
  `aux = |t|`. The envelope stays: it is implied by `aux = |t|` and carries the clause provenance the elastic engine
  reads.
- **Lowered** (`ExpandAbs`): a sign indicator `y` per instance of the auxiliary and the two Big-M rows above, which pin
  `aux` from above.

Phase 2 runs after the `VarIndexer` exists, because both forms emit in flat columns and a general constraint names them.

`refuse_when_unbounded` is the one place the forms differ in *outcome*, for an exact ABS: the lowered form
has no finite `M` over an unbounded contributor, and where the backend declares no native ABS
`DeriveAbsAuxiliaryBounds` refuses, naming a column to bound. Where it declares one, the auxiliary is left unboxed
(`AbsLinkSpec::range_unbounded`), the native form states it, and the query answers. That divergence is the capability's
payoff.

**One binary, on one form only.** The sign indicator exists to switch the Big-M rows, and the native form states
`aux = |t|` with none. `ExpandAbs` allocates it with `AddGlobalBinaryAux`, one global binary per instance of the
auxiliary, as MIN/MAX does, and labels it with the ABS as a plan prints it, which a diagnosis names it by. Stage 05
allocates none: an indicator allocated before the form is known is row-scoped, and on the native form it would cost one
free binary *per data row*, referenced by no row and no general constraint. The solver presolves it away, so it is never
a wrong answer, but it is built, stored and marshalled across the solver API for every row of the input. An ABS that is
not exact has no indicator either.

**Each `t` is boxed by its own row.** `StateAbsNatively` brackets the argument column from
the row's own C1: `inner_r = k_r - sum_{v != aux} c_v x_v`, walked with
`DecideRowSignedRange` (skipping the auxiliary) and folded in with
`AuxRange::CoverRowEnds` — the same per-row walk every MIN/MAX auxiliary uses, keeping each end it can derive
independently. One interval per link, the **cover over rows**, would hand every row
but the extreme one more room than it can reach, and a single unbounded row would leave
every row's column free. A slack continuous column is a measured cost at the root LP relaxation, which is
why this is worth doing rather than a tidiness. A column is left free only where
nothing at all is derivable — which is exactly the query the native form exists to
answer.

**The argument is read whole.** `k` is C1's bound with its constant terms moved across
(`ClauseRecords::EffectiveBound`), in phase 1's interval and in the row `t = inner` alike (`ReadAbsArgument`, which both
forms read): a frame's `else` read as data (`abs(x - at(previous else 50: x) over (...))`) is a term of
no decision, and read off the bound alone it was left out -- `|x1 - 50|` read as `|x1|`.
The argument's columns are summed per column before the row is written: a frame
that selects the anchor itself (`abs(x - at(last: x) over (...))` at the last period)
lands on the column its other term reads, and a solver refuses a row naming a column
twice.

Phase 1 also **narrows the auxiliary's column box** to `max(|lo|, |hi|)`, for both forms. The
formulation pins `aux = |inner|`, so that is a valid upper bound — and the only one anything
derives, since `aux >= inner` / `aux >= -inner` bound the auxiliary from below only.
That box is why this runs before every other linearizer ("Big-M constants" above).

---

## 9a. General constraints — what is NOT a row

`SolverModel::general_constraints` carries the constructs the chosen backend takes
whole. Each record is a kind, a result column, argument columns, and the same
`ConstraintProvenance` every row carries.

It names **columns, not expressions**, and that is the design decision. Every backend's
general-constraint API relates variables to variables, so a record carrying an
expression would force each adapter to synthesize the same auxiliary column and
equality row — that is routing, and routing belongs in the gate. An adapter only
translates, and it never receives a kind its backend did not declare, so it may treat
an unknown one as an internal error. `SolveModel` asserts that contract before handing
the model over.

The linear half of a native construct is therefore an ordinary row emitted alongside
it: `t = inner` for ABS, and one such row per member for a MIN/MAX family, plus the
extremum column the general constraint pins.

**Unless the member is already a column.** `ABS(x)`, `MAX(x)` and `MIN(x)` name a
decision variable directly, and there `t = x` restates a column that already exists —
one column and one equality row per data row, saying nothing. They are not free: a
general constraint reads its arguments, so presolve cannot substitute them away, and
the copies survive into the solve. Measured on `MAXIMIZE MAX(x)` at 15K rows, 0.97s
with the copies against 0.29s without, same answer. So an exact renaming — one variable
term, coefficient 1, no constant — passes the variable's own column to the general
constraint. `MAX(2 * x)` and `ABS(x - target)` are genuine expressions and still earn a
column.

Those columns are **boxed by the same `AuxRange` walk the Big-M constant comes from**,
never left free as a matter of course. A free continuous column is a measured
performance cliff (see stage 05's auxiliary-box section), and it is not a consequence
of going native. A column is free only where no range is derivable at all — which is
exactly the query the native path exists to answer, since a general constraint needs no
bound where a Big-M would.

**Each end is decided separately.** `AuxRange` carries `lo_unbounded` and
`hi_unbounded`, not one flag, because the two ends fail independently: `x >= 0` with no
ceiling has a derived floor and no derivable roof, and the auxiliary over it is emitted
`[0, 1e30]`. Only a Big-M needs both ends, since a constant has to dominate the whole
span — `Span()` asserts `!Unbounded()` and every refusal site tests the same. Boxing
is the one caller entitled to read the ends apart, because half a box is still a box,
and it is strictly better than none for the root simplex.

The lowered rows the construct would have produced are **not** also emitted; a model
holds one or the other, never both. The two exact rows stage 05 already wrote
(`aux >= inner`, `aux >= -inner`) do stay: they are implied by `aux = |t|`, they need
no Big-M, and they carry the clause provenance the elastic engine reads.

### Indicator constraints — a row, conditioned

`SolverModel::indicator_constraints` is the second native list, and it exists because
`<>` is naturally a conditional **row**, not a result-column relationship.

A `<>` clause has no scalar row of its own. Its two disjunction rows *are* the clause, both
`USER_MECHANISM`; a conditional row — `binary == value` implies this row — states that
semantic form directly. Exact grouped removal filters the indicator representation with
the same `origin.removal_group_id` used for matrix, quadratic, and native-general constraints.
That is why the two native lists remain separate: one relates columns, the other conditions
a row.

An `IndicatorConstraint` therefore **is a `ModelConstraint`** with a condition, rather than a
restatement of its fields. A conditional row and the matrix row it lowers to are the same row
with the same provenance: the lowering keeps the row and adds the term that switches it. The
input and the model hold one type for each of a row, a conditional row and a native construct,
so the builder moves them and copies no field.

`z == 0 => LHS <= K-1` and `z == 1 => LHS >= K+1` say what a Big-M pair says, with no
constant to dominate the row — so no contributing variable needs a finite bound.

Two consequences follow from a construct having no rows. `DumpSolverModel` renders
general constraints explicitly, or the corpus diff would read a native construct as
rows quietly disappearing. And `BuildUnboundedRayFallbackModel` **declines** a model
that has any — or any indicator constraint — exactly as it declines a quadratic one: the
ray model rebuilds the matrix row by row, so a conditional row cannot come along either,
and dropping one would relax the model past what a ray argument permits.

---

## 9b. `LowerDecideConstructs` — the one place a construct is lowered

The last pass of this stage. Every construct site above emits the **semantic** form and
nothing else: a `<>` becomes a pair of conditional rows whether or not any backend can
state one. This pass reads what the chosen backend declared — the answer stage 05
recorded on the plan, carried here as a value, not a fresh question to a backend — and
rewrites whatever it cannot state into ordinary rows.

Lowering a conditional row is one rewrite: give it a Big-M term on its own binary, sized
so the row is exactly slack when the condition is off.

| Condition | Sense | Emitted |
|---|---|---|
| `z == 0` | `<=` | `a·x - M z <= b` |
| `z == 0` | `>=` | `a·x + M z >= b` |
| `z == 1` | `<=` | `a·x + M z <= b + M` |
| `z == 1` | `>=` | `a·x - M z >= b - M` |

`M` is the distance from the row's bound to the far end of its own reach —
`max(a·x) - b` for a `<=` row, `b - min(a·x)` for a `>=` one — read off the boxes of the
columns actually in that row, with each coefficient's sign respected before an end is
blamed. Relaxed by exactly that much, the row admits everything the columns can produce
and cuts nothing. Plus a one-unit margin: the `<>` bound is `K±1` on an integer lattice,
so a unit of slack costs nothing there and keeps the relaxed branch clear of
floating-point wobble in the solver's own row activity.

Derived **per half**, from that half's own row, not one constant maximised over every
row of the clause: on the corpus an aggregate `<>` over four binary columns gets
`M = 22` and `M = 2` where a shared constant would be 21 on both.

It runs last because a Big-M reads column boxes and the sites above are what narrow
them. It refuses, naming a column to bound, where no finite `M` exists — and that
refusal belongs here and only here, because a construct the backend states needs no
constant to dominate it. `VarIndexer::OwnerOf` is what lets a pass working in flat
columns still name a decide variable in that message.

---

## 10. Source map

| Concern | Location |
|---|---|
| `SolverModel::Build`, all constraint paths, Q construction | `src/decidb/formulation/ilp_model_builder.cpp` |
| `LowerDecideConstructs`, global-auxiliary allocation, flat column boxes | `src/decidb/formulation/ilp_linearization.cpp` |
| Implied bounds, Big-M constants, per-row range walks | `src/decidb/formulation/linearization_bigm.cpp` |
| Extremum links (every MIN/MAX) | `src/decidb/formulation/linearization_minmax.cpp` |
| `<>` collapse classification and Big-M disjunction | `src/decidb/formulation/linearization_not_equal.cpp` |
| McCormick and ABS rows | `src/decidb/formulation/linearization_bilinear_abs.cpp` |
| The value rewrite's helper decisions: boxes and definitions | `src/decidb/formulation/linearization_reducer_value.cpp` |
| Helpers shared across those files | `src/include/duckdb/decidb/formulation/ilp_linearization_internal.hpp` |
| `ExtremumLinkSpec`, `ExtremumMemberSource` — what a link is stated from | `src/include/duckdb/decidb/formulation/ilp_linearization.hpp` |
| `BilinearLinkSpec`, `AbsLinkSpec`, `ReducerValueLinkSpec` — the formulation tags | `src/include/duckdb/decidb/formulation/solver_input.hpp` |
| `VarIndexer`, `SolverModel`, `ModelConstraint` | `src/include/duckdb/decidb/formulation/ilp_model.hpp` |
| `SolverInput`, `EvaluatedConstraint`, `NotEqualClause`, `QuadraticClause`, `ConstraintProvenance`, `CoefficientColumn` | `src/include/duckdb/decidb/formulation/solver_input.hpp` |
| `DecideRowOrigin` | `src/include/duckdb/common/decide_source_info.hpp` |
| Golden model corpus (the characterization oracle) | `test/decide/golden/` |

## Arms no test reaches, kept (BLOAT-30)

Each is a guard a stage boundary or a typed read needs:

  - **Stage handoffs.** The comparison type stage 05 hands over (`SolverModel::Build`: `<>` is consumed earlier); the
  column-box finiteness check in `SolverModel::Build`, the one place an infinite box from any rewrite is refused (a
  MIN/MAX auxiliary's reach is coefficient times box, so `b * y` with `b = 1e308` passes a DOUBLE).
- **Typed reads.** A coefficient label read back as an optional weight label and as a number (`strtod` accepts `""`,
  `inf`, `nan`).
