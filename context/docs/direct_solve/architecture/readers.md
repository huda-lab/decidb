# Readers: what a rule reads of a query

A rule is an optimizer rule over the bound DECIDE operator, as DuckDB's rules read a `LogicalOperator`. It takes
`const LogicalDecide &` and reads the operator before any DECIDE rewrite: the SQL text and the solver's model are not
its input. Shared reader functions turn a clause into the form a rule checks. No record stands between a rule and the
operator.

## What a rule reads

| What | Where | Read by |
|---|---|---|
| The decisions, their scopes and keys | `decide.bound.variables`, `variable_scopes`, `keyed_scopes`, `keys` | the rule, directly; `DirectDomainOf` for BOOL, INT or REAL; `DirectReadKey` for a key as a plan groups by it |
| A clause's `when` and `per` | `BoundDecideClause::scope` | the rule, directly (`DirectClause::When`, `PerClass`) |
| A clause's comparison and right side | `BoundDecideClause::body` | `DirectClause::Comparison`, `Rhs` |
| A clause's left side as additive parts | derived | `DirectReadClauses` |
| The bound a per-row clause puts on one decision | derived | `DirectReadClauses` (`singleton_row`, `bound`, `variable_bound`) |
| The objective as additive parts | derived | `DirectReadObjective` |
| The source columns | `decide.SourceBindings()` | the rule and the value predicates (`direct_expression.hpp`) |
| The value checks | `decide.value_checks` | the rule driver alone |
| What the plan below DECIDE declares | derived from DECIDE's child | `DirectSourceFacts`, built by the rule that wants it (O1) |

A derived reading is made on demand: a rule tests the decisions first, which costs nothing, and calls a reader only
when those tests pass. A query whose decisions fit no rule is never split.

## The readers

- **`DirectReadClauses`** reads every clause into a `DirectClause`: a reference to the bound clause, the clause as a
  message names it (`written`), and the left side as `DirectPart`s, each `sign * scale * reducer(terms)` with the
  reducer's `by` key, its own `when` and its qualifier. A reduced clause has one part per additive term of its left
  side; a clause per row has one row-level part. The terms come from the solver path's term splitter
  (`src/planner/decide/decide_term_split.cpp`), so the two paths cannot read one term differently.
- **`DirectReadObjective`** reads the objective's parts the same way. The parts are a minimization: a query written
  MAXIMIZE has the flip in each part's sign. A query-wide decision beside reducers is a row-level part.
- **`DirectReadKey`** reads one key of `BoundDecide::keys`: each element's grouping expression (the collated one), its
  label and name, and `no_group`. A column element puts a row with a NULL in no class; an element that stands for a
  relation's columns (`per D:`) takes NULL as a value. The source columns alone cannot tell the two apart, which is
  why a rule reads the key and not its slots. A keyed decision's key and a reducer's `by` key are read by this one
  function.
- **`DirectSourceFacts`** reads DECIDE's child: `left` and `right`, `not_null`, `keys`.
- **`DirectRender`** and **`DirectPlace`** give an expression of the source as the user wrote it and where it stands,
  for a message that names a value.

## Conservative analysis

A reader returns false for what it cannot read exactly, and the rule that called it declines. A wrong rewrite cannot
follow from a gap in a reader. A decline carries no reason: the rule applies or it does not.

`DirectReadClauses` returns false at the first clause that is one of these:

- a clause the query text holds no place for (no name in a message);
- a comparison on a reducer's value, which became a helper decision (the VALUE shape, `BoundDecideClause::shape`);
- a term that is neither linear nor an ABS, or a term inside an ABS or a square that is not linear;
- an order-0 norm (it needs indicator variables);
- a `by` or `per` key that reads more than the source;
- a `per K:` in front of a clause per row;
- a reducer's own `per` whose body has variants (`KeyedScopeInfo::signature`, D71): its instances are the key's classes
  split by the body's values, which the operator numbers and no plan reads.

`DirectReadObjective` returns false for an unread part, a reducer that holds a reducer (`holds_reducer`, D81), an
objective with a variant signature (produced once, D72, which the DECIDE operator checks), and a constant offset that
is not finite.

A frame selector has no reader of its own. One over a decision is an opaque term, which the clause reader declines.
One over data stands in a right side, a coefficient or a `when`, and the value predicate every rule proves those with
(`DirectIsDecisionFreeDeterministic`) refuses it: a frame reads the row its position selects on a timeline, so it is
no value of the source row.

The readers trust the invariants of the bound operator for everything else (one source child, the four reducers,
comparison and `IN` leaves, a `SUCH THAT` that is an `AND` of clauses, at most one `when` in front of a clause) and
state them as `D_ASSERT`s, because no query can break them and an untestable fallback would be dead code. A change
that admits a new shape teaches a reader to read it or to return false.

## Why readers and no intermediate representation

- **One representation.** The bound operator states each fact once, typed. A copy of it for the rules is a second
  place for the same fact, which the two paths can then read differently.
- **The shared analysis is shared code.** A clause is split into terms and a per-row bound is classified by the
  functions the solver path calls (`DecideTermSplitter`, `DecideReadSingletonRow`), so a rule and the solver agree by
  construction.
- **Matching a familiar subtree is unsafe.** The solver's flattened model is built later and can drop a term. Reading
  the complete bound clause means a rule sees everything the user wrote.

## Things worth knowing

- `DirectSourceFacts::left` and `right` hold the source columns of each side when DECIDE's own child is a cross
  product, and are empty otherwise. Every row of one side then meets every row of the other, which a rule cannot learn
  from the columns. A one-side `WHERE` keeps it, because filter pushdown runs before the DECIDE optimizer and places
  the filter inside its side; a filter across both sides makes a join, and any operator between DECIDE and the cross
  product leaves both empty. Looking through operators would need a proof per operator, so the reader does not.
- `not_null` holds the source columns that a table the source scans directly declares `NOT NULL` (a `PRIMARY KEY`
  column carries the constraint too). `keys` holds each `UNIQUE` or `PRIMARY KEY` constraint whose columns the scan
  reads and the table declares all `NOT NULL`. The reader walks down from DECIDE's child through filters and cross
  products, which pass values through unchanged, to each table scan; any other operator ends the walk, because a
  projection or an outer join can make a NULL the table never holds. A declared constraint holds for every row a plan
  reads; column statistics describe the data when the plan was built, so they prove nothing.
- A per-row comparison carries the solver path's own reading of it (`DecideReadSingletonRow`): `singleton_row` when it
  is a bound on one decision, and `bound`, the interval of values it admits as DuckDB evaluates the comparison (D64).
  `variable_bound` is set when the solver path folds it into its decision's box instead of emitting a model row (an
  unconditional bound whose ends fold at plan time, in any spelling, D41). Such a bound replaces the variable's default
  lower bound of 0. A constant lower bound under a `when` replaces it on the rows the `when` keeps
  (`DecideSingletonRow::LiftsDefaultFloor`); every other bound only intersects with it. Direct rules run before the
  MIN/MAX rewrite marks an easy extremum, so only a per-row bound reads here.
- A SUM skips a row whose data is NULL. The clause reader takes the NULL tests canonicalization added
  (`BoundDecideNullTestExpression`) out of the part's `when` and guards each coefficient with them: the coefficient
  reads 0 on a skipped row. The guard is a `BoundDecideNullGuardExpression`, a Decorator over the `CASE`: a render
  prints the value it protects, and `DecideUnguarded` reads that value where a rule needs a constant. The rule driver
  unwraps the guard in the plan it installs, so DuckDB's rewriter simplifies it as any other expression.
- The value checks (`LogicalDecide::value_checks`) are each a BOOLEAN that is true on a valid row and raises on any
  other. The rule driver puts them under every rule's plan (`DirectAddRowChecks`,
  [framework/done.md](../framework/done.md) § Row checks), so a rule does not see them; the solver path keeps them on
  the DECIDE operator, which runs them before it reads a value (`DecideTreeColumns::CheckData`). An empty group needs
  no check: a rule reads no row there, as the solver path skips its instance (D68).
- `norm(e, p)` is replaced by its definition when the query is canonicalized (an L0 norm of a decision stays a marker:
  a reducer with no function, which the clause reader declines). A definition means what it says, so a rule reads it
  as the SUM or MAX it is; a rule refuses anything it does not prove.
- Estimates (row counts) never prove anything a rule relies on.

Details: `direct_clause_reader.cpp`, `direct_source_facts.cpp` and the declarations in `direct_rule.hpp`. The C++ cases
in `test_decidb_direct_readers.cpp` list what each reader covers.
